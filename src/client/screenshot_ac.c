/*
Copyright (C) 2024 Anticheat Screenshots

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

//
// Client-side anticheat screenshot capture and send (async)
//

#include "client.h"
#include "refresh/refresh.h"
#include "common/async.h"

// Cvars
static cvar_t *cl_ac_screenshot_enabled;
static cvar_t *cl_ac_screenshot_auto;
static cvar_t *cl_ac_screenshot_interval;

// Screenshot format constants
#define AC_SCREENSHOT_FMT_WEBP  1

// Maximum dimensions for WebP (fits comfortably in 32KB)
#define AC_SCREENSHOT_MAX_WIDTH  640
#define AC_SCREENSHOT_MAX_HEIGHT 480
#define AC_SCREENSHOT_MIN_WIDTH  320
#define AC_SCREENSHOT_MIN_HEIGHT 240

// State
static unsigned cl_ac_screenshot_last;
static bool cl_ac_screenshot_pending;

/*
Work structure passed between main thread and async worker thread.
Lifespan:
  - Allocated on main thread before Com_QueueAsyncWork
  - Com_QueueAsyncWork copies the asyncwork_t (which points to this via cb_arg)
  - work_cb runs on worker thread: reads pixels, downscales, compresses
  - done_cb runs on main thread (via Com_CompleteAsyncWork): sends via netchan
  - Freed in done_cb
*/
typedef struct {
    // input (ownership of pixels transfers here)
    byte   *pixels;
    int     width, height, bpp, rowbytes;
    int     target_width, target_height;
    // output from worker thread
    byte   *image_buf;
    size_t  image_size;
    int     out_width, out_height;
    int     status;     // 0 = ok, <0 = error
} ac_screenshot_work_t;

/*
===============
cl_ac_screenshot_work_cb

Worker thread: downscale pixels and compress to WebP.
All CPU-heavy work happens here, never touches GL context.
===============
*/
static void cl_ac_screenshot_work_cb(void *arg)
{
    ac_screenshot_work_t *work = arg;
    screenshot_t s_src, s_small;
    int ret;

    // Set up source screenshot for downscale
    memset(&s_src, 0, sizeof(s_src));
    s_src.pixels = work->pixels;
    s_src.width = work->width;
    s_src.height = work->height;
    s_src.bpp = work->bpp;
    s_src.rowbytes = work->rowbytes;

    // Downscale
    memset(&s_small, 0, sizeof(s_small));
    ret = IMG_Downscale(&s_small, &s_src, work->target_width, work->target_height);
    if (ret < 0) {
        work->status = ret;
        Z_Free(work->pixels);
        work->pixels = NULL;
        return;
    }

    // Free original pixels after downscale
    Z_Free(work->pixels);
    work->pixels = NULL;

    // Compress to WebP with adaptive quality loop
    // Try decreasing qualities until image fits in 32KB netchan limit
    {
        static const int qualities[] = { 35, 25, 15 };
        int i, num_qualities = 3;
        for (i = 0; i < num_qualities; i++) {
            ret = IMG_CompressWebP_AC(&s_small, &work->image_buf, &work->image_size, qualities[i]);
            if (ret < 0) {
                work->status = ret;
                Z_Free(s_small.pixels);
                return;
            }
            if (work->image_size <= 32000)
                break;
            Z_Free(work->image_buf);
            work->image_buf = NULL;
        }
        if (work->image_size > 32000) {
            work->status = Q_ERR(EOVERFLOW);
            Z_Free(work->image_buf);
            Z_Free(s_small.pixels);
            return;
        }
    }

    work->out_width = s_small.width;
    work->out_height = s_small.height;
    work->status = 0;

    Z_Free(s_small.pixels);
}

/*
===============
cl_ac_screenshot_done_cb

Main thread callback: send the compressed WebP via netchan.
Called from Com_CompleteAsyncWork on the next frame.
===============
*/
static void cl_ac_screenshot_done_cb(void *arg)
{
    ac_screenshot_work_t *work = arg;

    if (work->status < 0) {
        Com_EPrintf("AC Screenshot: Async compress failed: %s\n", Q_ErrorString(work->status));
        Z_Free(work->image_buf);
        Z_Free(work);
        cl_ac_screenshot_pending = false;
        return;
    }

    if (cls.state < ca_connected) {
        Com_DPrintf("AC Screenshot: Disconnected before send, discarding\n");
        Z_Free(work->image_buf);
        Z_Free(work);
        cl_ac_screenshot_pending = false;
        return;
    }

    // Check size fits in message
    if (work->image_size > 32000) {
        Com_EPrintf("AC Screenshot: Image too large (%zu bytes), discarding\n", work->image_size);
        Z_Free(work->image_buf);
        Z_Free(work);
        cl_ac_screenshot_pending = false;
        return;
    }

    // Send to server via netchan
    // Protocol: [clc_screenshot][byte format][short width][short height][long image_size][image_data...]
    MSG_WriteByte(clc_screenshot);
    MSG_WriteByte(AC_SCREENSHOT_FMT_WEBP);
    MSG_WriteShort(work->out_width);
    MSG_WriteShort(work->out_height);
    MSG_WriteLong((int)work->image_size);
    MSG_WriteData(work->image_buf, work->image_size);
    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
    SZ_Clear(&msg_write);

    Com_DPrintf("AC Screenshot: Sent %dx%d WebP (%zu bytes)\n",
                work->out_width, work->out_height, work->image_size);

    Z_Free(work->image_buf);
    Z_Free(work);
    cl_ac_screenshot_pending = false;
}

/*
===============
CL_AC_Init

Register anticheat screenshot cvars
===============
*/
void CL_AC_Init(void)
{
    cl_ac_screenshot_enabled = Cvar_Get("cl_ac_screenshot_enabled", "1", 0);
    cl_ac_screenshot_auto = Cvar_Get("cl_ac_screenshot_auto", "0", 0);
    cl_ac_screenshot_interval = Cvar_Get("cl_ac_screenshot_interval", "30", 0);
}

/*
===============
CL_AC_SendScreenshot

Capture current frame and queue async WebP compress + send.
Called when server sends "cmd \177c screenshot_ac\n"

Only GL readback (IMG_ReadPixels) runs on the main thread.
Downscale + WebP compress happen on the worker thread.
Netchan_Transmit happens on the main thread next frame via done_cb.
===============
*/
void CL_AC_SendScreenshot(void)
{
    ac_screenshot_work_t *work;
    screenshot_t s_full;
    asyncwork_t async;
    int ret;
    int new_width, new_height;

    if (!cl_ac_screenshot_enabled->integer)
        return;

    if (cls.state < ca_connected)
        return;

    // Prevent concurrent screenshots
    if (cl_ac_screenshot_pending) {
        Com_DPrintf("AC Screenshot: Already pending, skipping\n");
        return;
    }

    // Read framebuffer (GL call, must be on main thread)
    memset(&s_full, 0, sizeof(s_full));
    ret = IMG_ReadPixels(&s_full);
    if (ret < 0) {
        Com_EPrintf("AC Screenshot: Failed to read pixels: %s\n", Q_ErrorString(ret));
        return;
    }

    // Calculate target dimensions maintaining aspect ratio
    // Target: 640x480 max, scale down if source aspect doesn't match
    {
        float src_aspect = (float)s_full.width / (float)s_full.height;
        float dst_aspect = (float)AC_SCREENSHOT_MAX_WIDTH / (float)AC_SCREENSHOT_MAX_HEIGHT;

        if (src_aspect > dst_aspect) {
            // Source is wider than 4:3, constrain by width
            new_width = AC_SCREENSHOT_MAX_WIDTH;
            new_height = (int)(AC_SCREENSHOT_MAX_WIDTH / src_aspect);
        } else {
            // Source is taller than 4:3, constrain by height
            new_height = AC_SCREENSHOT_MAX_HEIGHT;
            new_width = (int)(AC_SCREENSHOT_MAX_HEIGHT * src_aspect);
        }
    }

    // Clamp to reasonable minimums
    if (new_width < AC_SCREENSHOT_MIN_WIDTH) new_width = AC_SCREENSHOT_MIN_WIDTH;
    if (new_height < AC_SCREENSHOT_MIN_HEIGHT) new_height = AC_SCREENSHOT_MIN_HEIGHT;

    // Allocate work structure
    work = Z_Malloc(sizeof(*work));
    work->pixels = s_full.pixels;
    work->width = s_full.width;
    work->height = s_full.height;
    work->bpp = s_full.bpp;
    work->rowbytes = s_full.rowbytes;
    work->target_width = new_width;
    work->target_height = new_height;
    work->image_buf = NULL;
    work->image_size = 0;
    work->out_width = 0;
    work->out_height = 0;
    work->status = 0;

    // Queue async work
    async.work_cb = cl_ac_screenshot_work_cb;
    async.done_cb = cl_ac_screenshot_done_cb;
    async.cb_arg = work;

    cl_ac_screenshot_pending = true;
    Com_QueueAsyncWork(&async);

    Com_DPrintf("AC Screenshot: Queued async capture %dx%d -> %dx%d\n",
                s_full.width, s_full.height, new_width, new_height);
}

/*
===============
CL_AC_Run

Periodic automatic screenshot check, called from CL_Frame
===============
*/
void CL_AC_Run(void)
{
    if (!cl_ac_screenshot_auto->integer)
        return;
    if (!cl_ac_screenshot_enabled->integer)
        return;
    if (cls.state < ca_active)
        return;
    if (cl_ac_screenshot_pending)
        return;
    if (cls.realtime - cl_ac_screenshot_last < (unsigned)(cl_ac_screenshot_interval->integer * 1000))
        return;

    cl_ac_screenshot_last = cls.realtime;
    CL_AC_SendScreenshot();
}

/*
===============
CL_AC_Screenshot_f

Console command handler for screenshot_ac
This is called when the server requests a screenshot via stuffcmd
===============
*/
static void CL_AC_Screenshot_f(void)
{
    CL_AC_SendScreenshot();
}

/*
===============
CL_AC_RegisterCommands

Register client-side anticheat commands
===============
*/
void CL_AC_RegisterCommands(void)
{
    Cmd_AddCommand("screenshot_ac", CL_AC_Screenshot_f);
}

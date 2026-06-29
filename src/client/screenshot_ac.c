/*
Copyright (C) 2024 Anticheat Screenshots

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

//
// Client-side anticheat screenshot capture and send
//

#include "client.h"
#include "refresh/refresh.h"

// Cvars
static cvar_t *cl_ac_screenshot_enabled;
static cvar_t *cl_ac_screenshot_quality;
static cvar_t *cl_ac_screenshot_scale;
static cvar_t *cl_ac_screenshot_auto;
static cvar_t *cl_ac_screenshot_interval;

// State
static unsigned cl_ac_screenshot_last;

/*
===============
CL_AC_Init

Register anticheat screenshot cvars
===============
*/
void CL_AC_Init(void)
{
    cl_ac_screenshot_enabled = Cvar_Get("cl_ac_screenshot_enabled", "1", 0);
    cl_ac_screenshot_quality = Cvar_Get("cl_ac_screenshot_quality", "45", 0);
    cl_ac_screenshot_scale = Cvar_Get("cl_ac_screenshot_scale", "0.25", 0);
    cl_ac_screenshot_auto = Cvar_Get("cl_ac_screenshot_auto", "0", 0);
    cl_ac_screenshot_interval = Cvar_Get("cl_ac_screenshot_interval", "30", 0);
}

/*
===============
CL_AC_SendScreenshot

Capture current frame, compress to JPEG, and send to server
Called when server sends "cmd \177c screenshot_ac\n"
===============
*/
void CL_AC_SendScreenshot(void)
{
    screenshot_t s_full, s_small;
    byte *jpeg_buf = NULL;
    size_t jpeg_size = 0;
    int ret;
    int new_width, new_height;

    if (!cl_ac_screenshot_enabled->integer)
        return;

    if (cls.state < ca_connected)
        return;

    // Read framebuffer
    memset(&s_full, 0, sizeof(s_full));
    ret = IMG_ReadPixels(&s_full);
    if (ret < 0) {
        Com_EPrintf("AC Screenshot: Failed to read pixels: %s\n", Q_ErrorString(ret));
        return;
    }

    // Calculate downscaled dimensions
    new_width = (int)(s_full.width * cl_ac_screenshot_scale->value);
    new_height = (int)(s_full.height * cl_ac_screenshot_scale->value);

    // Clamp to reasonable minimums
    if (new_width < 160) new_width = 160;
    if (new_height < 120) new_height = 120;

    // Clamp to maximum that fits in 32KB message
    // 320x240 at quality 50 should be well under 32KB
    if (new_width > 480) new_width = 480;
    if (new_height > 360) new_height = 360;

    // Downscale
    memset(&s_small, 0, sizeof(s_small));
    ret = IMG_Downscale(&s_small, &s_full, new_width, new_height);
    if (ret < 0) {
        Com_EPrintf("AC Screenshot: Failed to downscale: %s\n", Q_ErrorString(ret));
        Z_Free(s_full.pixels);
        return;
    }

    // Compress to JPEG
    ret = IMG_CompressJPEG(&s_small, &jpeg_buf, &jpeg_size, cl_ac_screenshot_quality->integer);
    if (ret < 0) {
        Com_EPrintf("AC Screenshot: Failed to compress JPEG: %s\n", Q_ErrorString(ret));
        Z_Free(s_small.pixels);
        Z_Free(s_full.pixels);
        return;
    }

    // Check size fits in message (MAX_MSGLEN = 32KB, reserve some for header)
    if (jpeg_size > 32000) {
        Com_EPrintf("AC Screenshot: JPEG too large (%zu bytes), skipping\n", jpeg_size);
        Z_Free(jpeg_buf);
        Z_Free(s_small.pixels);
        Z_Free(s_full.pixels);
        return;
    }

    // Send to server via netchan
    MSG_WriteByte(clc_screenshot);
    MSG_WriteShort(s_small.width);
    MSG_WriteShort(s_small.height);
    MSG_WriteLong((int)jpeg_size);
    MSG_WriteData(jpeg_buf, jpeg_size);
    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
    SZ_Clear(&msg_write);

    // Reset command history to discard usercmds accumulated during
    // blocking screenshot capture/compress (prevents MAX_PACKET_USERCMDS warning)
    for (int i = 0; i < CMD_BACKUP; i++) {
        cl.history[i].cmdNumber = cl.cmdNumber;
    }

    // Reset ALL transmit state to prevent MAX_PACKET_USERCMDS accumulation.
    // The screenshot blocks the main thread, during which outgoing_sequence
    // gets incremented by Netchan_Transmit but usercmds keep piling up.
    cl.lastTransmitTime = 0;
    cl.lastTransmitCmdNumber = cl.cmdNumber;
    cl.lastTransmitCmdNumberReal = cl.cmdNumber;

    Com_DPrintf("AC Screenshot: Sent %dx%d JPEG (%zu bytes)\n",
                s_small.width, s_small.height, jpeg_size);

    // Cleanup
    Z_Free(jpeg_buf);
    Z_Free(s_small.pixels);
    Z_Free(s_full.pixels);
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

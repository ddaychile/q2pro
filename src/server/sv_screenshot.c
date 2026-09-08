/*
Copyright (C) 2024 Anticheat Screenshots

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

//
// Server-side anticheat screenshot receive and forward
//

#include "server.h"

/*
===============
SV_ParseScreenshot

Parse incoming clc_screenshot message from client and forward to AC server
Protocol: [clc_screenshot][byte format][short width][short height][long image_size][image_data...]
===============
*/
void SV_ParseScreenshot(void)
{
    int width, height, image_size;
    byte *image_data;

    if (!sv_client) {
        return;
    }

    // Read format byte (0=jpeg, 1=webp)
    int format = MSG_ReadByte();

    // Read dimensions
    width = MSG_ReadShort();
    height = MSG_ReadShort();
    image_size = MSG_ReadLong();

    // Validate size
    if (image_size < 100 || image_size > 32000) {
        Com_WPrintf("Screenshot: Invalid size %d from %s\n", image_size, sv_client->name);
        SV_DropClient(sv_client, "invalid screenshot size");
        return;
    }

    if (width < 1 || height < 1 || width > 1920 || height > 1080) {
        Com_WPrintf("Screenshot: Invalid dimensions %dx%d from %s\n", width, height, sv_client->name);
        SV_DropClient(sv_client, "invalid screenshot dimensions");
        return;
    }

    // Read image data (MSG_ReadData returns pointer to read buffer)
    image_data = MSG_ReadData(image_size);
    if (!image_data) {
        Com_WPrintf("Screenshot: Failed to read data from %s\n", sv_client->name);
        return;
    }

    Com_DPrintf("Screenshot: Received %dx%d format=%d (%d bytes) from %s\n",
                width, height, format, image_size, sv_client->name);

#if USE_AC_SERVER
    // Forward to anticheat server if connected
    AC_ForwardScreenshot(sv_client, width, height, format, image_data, image_size);
#else
    (void)format;
#endif
}

/*
===============
SV_Screenshot_f

Console command: sv_screenshot <player_name>
Request a screenshot from a specific player
===============
*/
void SV_Screenshot_f(void)
{
    client_t *cl;
    char *name;

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: sv_screenshot <player_name>\n");
        return;
    }

    name = Cmd_Argv(1);

    // Find client by name
    FOR_EACH_CLIENT(cl) {
        if (cl->state < cs_spawned)
            continue;
        if (!strcmp(cl->name, name)) {
            // Send command to client requesting screenshot
            SV_ClientCommand(cl, "screenshot_ac\n");
            Com_Printf("Screenshot requested from %s\n", cl->name);
            return;
        }
    }

    Com_Printf("Player '%s' not found\n", name);
}

/*
===============
SV_ScreenshotList_f

Console command: sv_screenshots
List all connected players and their screenshot status
===============
*/
void SV_ScreenshotList_f(void)
{
    client_t *cl;
    int count = 0;

    Com_Printf("+----------------+----------+\n");
    Com_Printf("|  Player Name   |  State   |\n");
    Com_Printf("+----------------+----------+\n");

    FOR_EACH_CLIENT(cl) {
        if (cl->state < cs_spawned)
            continue;

        Com_Printf("|%-16s| %8s |\n", cl->name, "active");
        count++;
    }

    Com_Printf("+----------------+----------+\n");
    Com_Printf("%d players connected\n", count);
}

/*
===============
SV_ScreenshotAll_f

Console command: sv_screenshotall
Request a screenshot from all connected players
===============
*/
void SV_ScreenshotAll_f(void)
{
    client_t *cl;
    int count = 0;

    FOR_EACH_CLIENT(cl) {
        if (cl->state < cs_spawned)
            continue;
        SV_ClientCommand(cl, "screenshot_ac\n");
        count++;
    }

    Com_Printf("Screenshot requested from all %d players\n", count);
}

/*
===============
SV_ParseACData

Parse incoming clc_acdata message from client (file hashes + cvar values)
and forward to AC server as ACC_CLIENTDATA
===============
*/
void SV_ParseACData(void)
{
    int num_files, num_cvars;

    if (!sv_client) {
        return;
    }

    num_files = MSG_ReadLong();
    num_cvars = MSG_ReadLong();

    // Sanity checks
    if (num_files < 0 || num_files > 256 || num_cvars < 0 || num_cvars > 256) {
        Com_WPrintf("ACData: Invalid counts from %s: %d files, %d cvars\n",
                     sv_client->name, num_files, num_cvars);
        return;
    }

    Com_DPrintf("ACData: Received %d files, %d cvars from %s\n",
                num_files, num_cvars, sv_client->name);

#if USE_AC_SERVER
    AC_EnforceClientCvars(sv_client, num_files, num_cvars);
    AC_ForwardACData(sv_client, num_files, num_cvars);
#endif
}

/*
================
SV_ParseProcessData

Parse incoming clc_processdata message from client (running processes + modules)
and forward to AC server as ACC_PROCESSDATA
================
*/
void SV_ParseProcessData(void)
{
    int num_processes;
    byte flags;

    if (!sv_client) {
        return;
    }

    flags = MSG_ReadByte();
    num_processes = MSG_ReadLong();

    if (num_processes < 0 || num_processes > 1024) {
        Com_WPrintf("ProcessData: Invalid process count from %s: %d\n",
                     sv_client->name, num_processes);
        return;
    }

#if USE_AC_SERVER
    int data_size = SZ_Remaining(&msg_read);

    Com_DPrintf("ProcessData: Received %d processes from %s (flags=0x%02x, %d bytes)\n",
                num_processes, sv_client->name, flags, data_size);

    byte *data = MSG_ReadData(data_size);
    AC_ForwardProcessData(sv_client, flags, num_processes, data, data_size);
#endif
}

/*
================
SV_ParseCvarChange

Parse incoming clc_cvarchange message from client.

Wire format: [uint32 count]{[uint8 name_len][name][uint8 value_len][value]}

For each change: instantly enforce the cvar rules server-side, then
forward the change to the AC server for tamper tracking.
================
*/
void SV_ParseCvarChange(void)
{
    int count, i;
    char name[64];
    char value[256];
    byte name_len, val_len;

    if (!sv_client) {
        return;
    }

    count = MSG_ReadLong();

    if (count < 0 || count > 256) {
        Com_WPrintf("CvarChange: Invalid count from %s: %d\n",
                    sv_client->name, count);
        return;
    }

    Com_DPrintf("CvarChange: Received %d cvar changes from %s\n",
                count, sv_client->name);

    for (i = 0; i < count; i++) {
        if (msg_read.readcount + 1 > msg_read.cursize) {
            break;
        }
        name_len = MSG_ReadByte();
        if (name_len >= sizeof(name) || msg_read.readcount + name_len > msg_read.cursize) {
            break;
        }
        memcpy(name, msg_read.data + msg_read.readcount, name_len);
        name[name_len] = 0;
        msg_read.readcount += name_len;

        if (msg_read.readcount + 1 > msg_read.cursize) {
            break;
        }
        val_len = MSG_ReadByte();
        if (msg_read.readcount + val_len > msg_read.cursize) {
            break;
        }
        memcpy(value, msg_read.data + msg_read.readcount, val_len);
        value[val_len] = 0;
        msg_read.readcount += val_len;

#if USE_AC_SERVER
        AC_EnforceCvarChange(sv_client, name, value);
        AC_ForwardCvarChange(sv_client, name, value);
#endif
    }
}

/*
=================
SV_ParseSpikeModel

Parse incoming clc_acspike message from client.

Wire format: [uint32 count]{[uint8 path_len][path]}

For each spiked model: forward the violation to the AC server, which kicks
and records the player.
=================
*/
void SV_ParseSpikeModel(void)
{
    int count, i;
    char path[MAX_QPATH];
    byte path_len;

    if (!sv_client) {
        return;
    }

    count = MSG_ReadLong();

    if (count < 0 || count > MAX_MODELS) {
        Com_WPrintf("SpikeModel: Invalid count from %s: %d\n",
                    sv_client->name, count);
        return;
    }

    for (i = 0; i < count; i++) {
        if (msg_read.readcount + 1 > msg_read.cursize) {
            break;
        }
        path_len = MSG_ReadByte();
        if (path_len >= sizeof(path) || msg_read.readcount + path_len > msg_read.cursize) {
            break;
        }
        memcpy(path, msg_read.data + msg_read.readcount, path_len);
        path[path_len] = 0;
        msg_read.readcount += path_len;

        Com_DPrintf("SpikeModel: %s reported spiked model %s\n",
                    sv_client->name, path);

#if USE_AC_SERVER
        AC_ForwardSpikedModel(sv_client, path);
#endif
    }
}



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
Protocol: [clc_screenshot][short width][short height][long jpeg_size][jpeg_data...]
===============
*/
void SV_ParseScreenshot(void)
{
    int width, height, jpeg_size;
    byte *jpeg_data;

    if (!sv_client) {
        return;
    }

    // Read dimensions
    width = MSG_ReadShort();
    height = MSG_ReadShort();
    jpeg_size = MSG_ReadLong();

    // Validate size
    if (jpeg_size < 100 || jpeg_size > 32000) {
        Com_WPrintf("Screenshot: Invalid size %d from %s\n", jpeg_size, sv_client->name);
        SV_DropClient(sv_client, "invalid screenshot size");
        return;
    }

    if (width < 1 || height < 1 || width > 1920 || height > 1080) {
        Com_WPrintf("Screenshot: Invalid dimensions %dx%d from %s\n", width, height, sv_client->name);
        SV_DropClient(sv_client, "invalid screenshot dimensions");
        return;
    }

    // Read JPEG data (MSG_ReadData returns pointer to read buffer)
    jpeg_data = MSG_ReadData(jpeg_size);
    if (!jpeg_data) {
        Com_WPrintf("Screenshot: Failed to read data from %s\n", sv_client->name);
        return;
    }

    Com_DPrintf("Screenshot: Received %dx%d (%d bytes) from %s\n",
                width, height, jpeg_size, sv_client->name);

#if USE_AC_SERVER
    // Forward to anticheat server if connected
    AC_ForwardScreenshot(sv_client, width, height, jpeg_data, jpeg_size);
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
SV_ParseACData

Parse incoming clc_acdata message from client (file hashes + cvar values)
and forward to AC server as ACC_CLIENTDATA
===============
*/
void SV_ParseACData(void)
{
    int num_files, num_cvars;
    int i;

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



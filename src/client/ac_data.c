/*
Copyright (C) 2024 Anticheat Screenshots

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

//
// Client-side anticheat data: parse check list from server,
// compute file hashes, read cvars, send response back.
//

#include "client.h"
#include "ac_process.h"

#define AC_MAX_FILES  256
#define AC_MAX_CVARS  256
#define AC_MAX_PATH   256
#define AC_MAX_VALUE  256
#define AC_SHA1_SIZE  20

// Minimal SHA1 implementation
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} sha1_ctx_t;

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t a, b, c, d, e, t, w[80];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24)
             | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] << 8)
             | ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (w[i] << 1) | (w[i] >> 31);
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)
            t = ((b & c) | (~b & d)) + 0x5A827999;
        else if (i < 40)
            t = (b ^ c ^ d) + 0x6ED9EBA1;
        else if (i < 60)
            t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC;
        else
            t = (b ^ c ^ d) + 0xCA62C1D6;

        t += ((a << 5) | (a >> 27)) + e + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i, idx;

    idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    for (i = 0; i < len; i++) {
        ctx->buffer[idx++] = data[i];
        if (idx == 64) {
            sha1_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[AC_SHA1_SIZE])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    int i;

    sha1_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 0x3F) != 56) {
        sha1_update(ctx, &pad, 1);
    }

    // Write bit count big-endian
    for (i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (i * 8));
        sha1_update(ctx, &b, 1);
    }

    for (i = 0; i < 5; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

// SHA1 cache to avoid re-reading the same unchanged files on every recheck.
// Fast-path probes: just get the current on-disk size without reading content.
typedef struct {
    char     path[AC_MAX_PATH];
    int64_t  file_size;
    uint8_t  sha1[AC_SHA1_SIZE];
    bool     valid;
} ac_sha1_cache_entry_t;

static ac_sha1_cache_entry_t ac_sha1_cache[AC_MAX_FILES];
static int                   ac_cache_count;
static int                   ac_recheck_count; // periodic rechecks since last handshake

// SHA1 helper: hash a file
static bool sha1_file(const char *path, uint8_t digest[AC_SHA1_SIZE])
{
    byte *data;
    int len;
    sha1_ctx_t ctx;

    len = FS_LoadFile(path, (void **)&data);
    if (!data) {
        return false;
    }

    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);

    FS_FreeFile(data);
    return true;
}

// SHA1 helper with cache. The fast path only asks the filesystem for the
// current size of the file (no read, no hash) and returns a cached digest if
// the size is unchanged. force_full_rehash bypasses the cache entirely: this
// defeats an attacker restoring the original size+mtime after swapping a file,
// by periodically re-hashing everything unconditionally.
static bool sha1_file_cached(const char *path, uint8_t digest[AC_SHA1_SIZE],
                             bool force_full_rehash)
{
    int len;
    int i;

    if (!force_full_rehash) {
        len = FS_LoadFileEx(path, NULL, 0, TAG_FILESYSTEM); // size-only probe
        if (len >= 0) {
            // cache hit: same on-disk size, reuse the previous digest
            for (i = 0; i < ac_cache_count; i++) {
                if (ac_sha1_cache[i].valid &&
                    !strcmp(ac_sha1_cache[i].path, path) &&
                    ac_sha1_cache[i].file_size == len) {
                    memcpy(digest, ac_sha1_cache[i].sha1, AC_SHA1_SIZE);
                    return true;
                }
            }
        }
    } else {
        len = FS_LoadFileEx(path, NULL, 0, TAG_FILESYSTEM);
    }

    // cache miss / file changed / forced: read and hash from disk
    if (!sha1_file(path, digest)) {
        return false;
    }

    // mirror the result into the cache (insertion-order eviction)
    for (i = 0; i < ac_cache_count; i++) {
        if (!strcmp(ac_sha1_cache[i].path, path)) {
            break;
        }
    }
    if (i == ac_cache_count) {
        if (ac_cache_count < AC_MAX_FILES) {
            ac_cache_count++;
        } else {
            i = ac_cache_count - 1; // overwrite the oldest cached slot
        }
    }
    Q_strlcpy(ac_sha1_cache[i].path, path, sizeof(ac_sha1_cache[i].path));
    ac_sha1_cache[i].file_size = len;
    memcpy(ac_sha1_cache[i].sha1, digest, AC_SHA1_SIZE);
    ac_sha1_cache[i].valid = true;

    return true;
}

// Parse check list received from server
typedef struct {
    char     path[AC_MAX_PATH];
    uint8_t  expected_hash[AC_SHA1_SIZE];
    uint8_t  flags;
} ac_file_check_t;

typedef struct {
    char     name[AC_MAX_PATH];
    uint8_t  op;
    int      num_values;
    char     values[4][AC_MAX_VALUE];
    char     def[AC_MAX_VALUE];
} ac_cvar_check_t;

static ac_file_check_t  ac_files[AC_MAX_FILES];
static ac_cvar_check_t  ac_cvars[AC_MAX_CVARS];
static int              ac_num_files;
static int              ac_num_cvars;
static bool             ac_data_ready;

// Persistently true between handshake and disconnect, marks that the client
// holds a watchlist for the current game server and should report cvar changes.
static bool             ac_active;

// Real-time cvar change queue (watched cvars modified mid-game)
#define AC_MAX_QUEUED  32

typedef struct {
    char name[AC_MAX_PATH];
    char value[AC_MAX_VALUE];
} ac_cvar_change_t;

static ac_cvar_change_t ac_change_queue[AC_MAX_QUEUED];
static int              ac_change_count;

static void CL_ACData_SendResponse(void);
static void CL_ACData_WriteResponse(bool force_full_rehash);

/*
===============
CL_ParseACData

Parse svc_acdata message from server.
Protocol: [svc_acdata][uint32 num_files][uint32 num_cvars]
  per file: [20 bytes expected_hash][1 byte flags][uint8 path_len][path_bytes]
  per cvar: [uint8 name_len][name_bytes][1 byte op][1 byte num_values][values...][default]
===============
*/
void CL_ParseACData(void)
{
    int num_files, num_cvars;
    int i, j;

    num_files = MSG_ReadLong();
    num_cvars = MSG_ReadLong();

    if (num_files < 0 || num_files > AC_MAX_FILES || num_cvars < 0 || num_cvars > AC_MAX_CVARS) {
        Com_DPrintf("ACData: Invalid counts: %d files, %d cvars\n", num_files, num_cvars);
        return;
    }

    ac_num_files = num_files;
    ac_num_cvars = num_cvars;

    // Parse file entries
    for (i = 0; i < num_files; i++) {
        byte path_len;
        byte *data;

        data = MSG_ReadData(AC_SHA1_SIZE);
        if (data) memcpy(ac_files[i].expected_hash, data, AC_SHA1_SIZE);
        ac_files[i].flags = MSG_ReadByte();

        // Read path: if path_len is 0, reuse previous path
        path_len = MSG_ReadByte();
        if (path_len == 0 && i > 0) {
            strncpy(ac_files[i].path, ac_files[i-1].path, AC_MAX_PATH - 1);
            ac_files[i].path[AC_MAX_PATH - 1] = 0;
        } else if (path_len > 0) {
            data = MSG_ReadData(path_len);
            if (data) memcpy(ac_files[i].path, data, path_len);
            ac_files[i].path[path_len] = 0;
        } else {
            ac_files[i].path[0] = 0;
        }

        Com_DPrintf("ACData: File[%d]: %s (flags=%d)\n", i, ac_files[i].path, ac_files[i].flags);
    }

    // Parse cvar entries
    for (i = 0; i < num_cvars; i++) {
        byte name_len, val_len, def_len;
        byte *data;

        name_len = MSG_ReadByte();
        data = MSG_ReadData(name_len);
        if (data) memcpy(ac_cvars[i].name, data, name_len);
        ac_cvars[i].name[name_len] = 0;

        ac_cvars[i].op = MSG_ReadByte();
        ac_cvars[i].num_values = MSG_ReadByte();

        for (j = 0; j < ac_cvars[i].num_values && j < 4; j++) {
            val_len = MSG_ReadByte();
            data = MSG_ReadData(val_len);
            if (data) memcpy(ac_cvars[i].values[j], data, val_len);
            ac_cvars[i].values[j][val_len] = 0;
        }

        def_len = MSG_ReadByte();
        data = MSG_ReadData(def_len);
        if (data) memcpy(ac_cvars[i].def, data, def_len);
        ac_cvars[i].def[def_len] = 0;

        Com_DPrintf("ACData: Cvar[%d]: %s op=%d values=%d def=%s\n",
                    i, ac_cvars[i].name, ac_cvars[i].op, ac_cvars[i].num_values, ac_cvars[i].def);
    }

    bool first = !ac_active;

    ac_data_ready = true;
    ac_active = true;
    ac_change_count = 0;
    if (first) {
        ac_recheck_count = 0;
        Com_Printf("ACData: Received %d files, %d cvars from server\n", num_files, num_cvars);

        // console is visible while connecting, tell the player what is happening
        Com_Printf("^2Loading Anticheat, please wait...\n");
    } else {
        Com_DPrintf("ACData: Revalidating %d files, %d cvars\n", num_files, num_cvars);
    }

    // Immediately send response (hashes everything on first handshake,
    // populating the cache; force=true has no extra cost here)
    CL_ACData_SendResponse();
}

/*
===============
CL_ACData_WriteResponse

Compute file hashes (cached unless forced) and read cvars, then send
clc_acdata back to server.
===============
*/
static void CL_ACData_WriteResponse(bool force_full_rehash)
{
    int i;
    uint8_t hash[AC_SHA1_SIZE];
    bool verbose = ac_recheck_count == 0;

    MSG_WriteByte(clc_acdata);
    MSG_WriteLong(ac_num_files);
    MSG_WriteLong(ac_num_cvars);

    // Write file hashes (computed client-side)
    for (i = 0; i < ac_num_files; i++) {
        if (sha1_file_cached(ac_files[i].path, hash, force_full_rehash)) {
            MSG_WriteData(hash, AC_SHA1_SIZE);
            MSG_WriteByte(strlen(ac_files[i].path));
            MSG_WriteData(ac_files[i].path, strlen(ac_files[i].path));
            if (verbose) {
                Com_Printf("ACData: File[%d] OK: %s hash=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    i, ac_files[i].path,
                    hash[0], hash[1], hash[2], hash[3], hash[4],
                    hash[5], hash[6], hash[7], hash[8], hash[9],
                    hash[10], hash[11], hash[12], hash[13], hash[14],
                    hash[15], hash[16], hash[17], hash[18], hash[19]);
            } else {
                Com_DPrintf("ACData: File[%d] OK: %s hash=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    i, ac_files[i].path,
                    hash[0], hash[1], hash[2], hash[3], hash[4],
                    hash[5], hash[6], hash[7], hash[8], hash[9],
                    hash[10], hash[11], hash[12], hash[13], hash[14],
                    hash[15], hash[16], hash[17], hash[18], hash[19]);
            }
        } else {
            // File not found — send zero hash
            memset(hash, 0, AC_SHA1_SIZE);
            MSG_WriteData(hash, AC_SHA1_SIZE);
            MSG_WriteByte(strlen(ac_files[i].path));
            MSG_WriteData(ac_files[i].path, strlen(ac_files[i].path));
            Com_Printf("ACData: File[%d] NOT FOUND: %s\n", i, ac_files[i].path);
        }
    }

    // Write cvar values
    for (i = 0; i < ac_num_cvars; i++) {
        const char *actual_value;

        // Read the actual current value of the cvar (not the default)
        actual_value = Cvar_VariableString(ac_cvars[i].name);

        MSG_WriteByte(strlen(ac_cvars[i].name));
        MSG_WriteData(ac_cvars[i].name, strlen(ac_cvars[i].name));
        MSG_WriteByte(strlen(actual_value));
        MSG_WriteData(actual_value, strlen(actual_value));
    }

    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
    SZ_Clear(&msg_write);
}

/*
===============
CL_ACData_SendResponse

Initial response to svc_acdata (handshake or periodic recheck).
Every third recheck forces a full re-hash regardless of the size cache,
defeating an attacker who restores size+mtime after swapping a file.
===============
*/
void CL_ACData_SendResponse(void)
{
    bool force_full_rehash = ac_recheck_count > 0 && ac_recheck_count % 3 == 0;

    if (!ac_data_ready) {
        return;
    }

    CL_ACData_WriteResponse(force_full_rehash);
    ac_recheck_count++;

    ac_data_ready = false;
}

/*
===============
CL_ACData_Revalidate

Send clc_acdata immediately with a forced full re-hash. Called right after
the renderer reloads all models (vid_restart / fs_restart / map change),
so a model replaced on disk mid-game is reported to the server the moment
it is re-registered.
===============
*/
void CL_ACData_Revalidate(void)
{
    if (!ac_active || ac_num_files <= 0) {
        return;
    }

    if (cls.state < ca_connected) {
        return;
    }

    CL_ACData_WriteResponse(true);
}

/*
===============
CL_ACData_ResetCache

Invalidate all cached SHA1 digests so the next response re-reads every
protected file from disk.  Called on level change (CL_Changing_f) so that
an attacker who swapped a file during the previous level — preserving
its size — cannot reuse a stale cache entry to fool the server.
===============
*/
void CL_ACData_ResetCache(void)
{
    int i;

    for (i = 0; i < ac_cache_count; i++) {
        ac_sha1_cache[i].valid = false;
        ac_sha1_cache[i].path[0] = 0;
    }
    ac_cache_count = 0;
}

/*
===============
CL_ACData_Init

Initialize AC data module
===============
*/
void CL_ACData_Init(void)
{
    int i;

    ac_num_files = 0;
    ac_num_cvars = 0;
    ac_data_ready = false;
    ac_active = false;
    ac_change_count = 0;
    ac_recheck_count = 0;
    ac_cache_count = 0;
    for (i = 0; i < AC_MAX_QUEUED; i++) {
        ac_change_queue[i].name[0] = 0;
        ac_change_queue[i].value[0] = 0;
    }
    for (i = 0; i < AC_MAX_FILES; i++) {
        ac_sha1_cache[i].valid = false;
        ac_sha1_cache[i].path[0] = 0;
    }

    // hook into the cvar system to watch for real-time changes
    cvar_changed_notify = CL_AC_CvarChanged;
}

/*
===============
CL_ACData_Shutdown

Cleanup AC data module
===============
*/
void CL_ACData_Shutdown(void)
{
    ac_data_ready = false;
    ac_active = false;
    ac_change_count = 0;

    cvar_changed_notify = NULL;
}

/*
===============
CL_AC_CvarChanged

Called by the cvar system whenever any cvar value changes.
Queue the change if the cvar is on the server watchlist so it can be
sent to the server (clc_cvarchange) for real-time re-validation.
===============
*/
void CL_AC_CvarChanged(const char *name, const char *value)
{
    int i;

    if (!ac_active) {
        return; // no active AC session
    }

    // only report cvars the server cares about
    for (i = 0; i < ac_num_cvars; i++) {
        if (!strcmp(ac_cvars[i].name, name)) {
            break;
        }
    }
    if (i == ac_num_cvars) {
        return;
    }

    // update in place if the same cvar is already queued
    for (i = 0; i < ac_change_count; i++) {
        if (!strcmp(ac_change_queue[i].name, name)) {
            Q_strlcpy(ac_change_queue[i].value, value, sizeof(ac_change_queue[i].value));
            return;
        }
    }

    if (ac_change_count >= AC_MAX_QUEUED) {
        Com_DPrintf("ACData: cvar change queue full, dropping %s\n", name);
        return;
    }

    Q_strlcpy(ac_change_queue[ac_change_count].name, name, sizeof(ac_change_queue[ac_change_count].name));
    Q_strlcpy(ac_change_queue[ac_change_count].value, value, sizeof(ac_change_queue[ac_change_count].value));
    ac_change_count++;

    Com_DPrintf("ACData: queued cvar change: %s=%s\n", name, value);
}

/*
===============
CL_AC_FlushCvarChanges

Send all queued cvar changes to the server as clc_cvarchange.
Format: [clc_cvarchange][uint32 count]{[uint8 name_len][name][uint8 value_len][value]}
===============
*/
void CL_AC_FlushCvarChanges(void)
{
    int i;

    if (!ac_active) {
        return;
    }

    if (ac_change_count <= 0) {
        return;
    }

    if (cls.state < ca_connected) {
        return; // not connected to a game server
    }

    MSG_WriteByte(clc_cvarchange);
    MSG_WriteLong(ac_change_count);

    for (i = 0; i < ac_change_count; i++) {
        MSG_WriteByte(strlen(ac_change_queue[i].name));
        MSG_WriteData(ac_change_queue[i].name, strlen(ac_change_queue[i].name));
        MSG_WriteByte(strlen(ac_change_queue[i].value));
        MSG_WriteData(ac_change_queue[i].value, strlen(ac_change_queue[i].value));
    }

    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
    SZ_Clear(&msg_write);

    Com_DPrintf("ACData: sent %d cvar change%s\n", ac_change_count,
                ac_change_count == 1 ? "" : "s");

    ac_change_count = 0;
}

/*
================
CL_AC_FlushSpikedModels

Drain any pending spiked-model rejections reported by the renderer and send
them to the server as clc_acspike. Player and weapon models whose geometry
exceeds the anticheat threshold are reported right away so the AC server can
kick and record the violation.

Format: [clc_acspike][uint32 count]{[uint8 len][path]}
================
*/
void CL_AC_FlushSpikedModels(void)
{
    char paths[16][MAX_QPATH];
    int i, count = 0;

    if (!ac_active) {
        return;
    }

    if (cls.state < ca_connected) {
        return; // not connected to a game server
    }

    while (count < 16 && MOD_GetSpikedModel(paths[count], sizeof(paths[count]))) {
        Com_Printf("^1ACData: spiked model rejected: %s\n", paths[count]);
        count++;
    }

    if (!count) {
        return;
    }

    MSG_WriteByte(clc_acspike);
    MSG_WriteLong(count);

    for (i = 0; i < count; i++) {
        MSG_WriteByte(strlen(paths[i]));
        MSG_WriteData(paths[i], strlen(paths[i]));
    }

    Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
    SZ_Clear(&msg_write);

    Com_DPrintf("ACData: sent %d spiked model%s\n", count, count == 1 ? "" : "s");
}

/*
=================
CL_AC_RequestProcessCheck

Mark that a process/module snapshot should be sent on the next frame.
Called from event hooks (map change, model reload).
=================
*/
static bool ac_process_check_pending;

void CL_AC_RequestProcessCheck(void)
{
    ac_process_check_pending = true;
}

/*
=================
CL_AC_FlushProcessCheck

Polled each frame from CL_SendCmd.  If any models were (re)loaded from
disk since the last poll (map load, vid_restart, model swap attempt),
automatically mark a pending process check.  When pending and anticheat
is active, run the process/module enumeration and send the snapshot.
=================
*/
void CL_AC_FlushProcessCheck(void)
{
    // auto-request on model reload (covers map load + vid_restart)
    if (MOD_GetModelsReloaded())
        ac_process_check_pending = true;

    if (!ac_active)
        return;

    if (cls.state < ca_connected)
        return;

    if (!ac_process_check_pending)
        return;

    ac_process_check_pending = false;
    CL_AC_ProcessCheckNow();
}

/*
Copyright (C) 2025 Anticheat Process Detection

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

//
// Client-side anticheat process detection.
// Enumerates running processes and loaded modules, sends snapshot to server.
// Triggered by server via stufftext "cl_ac_process_check".
//

#include "client.h"
#include "ac_process.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif

#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#include <string.h>
#endif

#define AC_MAX_PROCESSES 1024
#define AC_MAX_MODULES   1024
#define AC_MAX_NAME      256
#define AC_MAX_PATH      256
#define AC_SHA1_SIZE     20
#define AC_SHA1_CACHE_SIZE 512

// Process data batch flags (wire format)
#define AC_PD_TRUNCATED  0x01   // snapshot incomplete (more processes/modules than stored)
#define AC_PD_FINAL      0x02   // last batch of this snapshot

// Maximum bytes per batch message (stay well under 32 KiB msg_write buffer)
#define AC_BATCH_MAX     16000

// SHA1 and file hashing only needed for process detection on Win/Linux
#if defined(_WIN32) || defined(__linux__)

// Minimal SHA1 (same as ac_data.c)
typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} ac_sha1_ctx_t;

static void ac_sha1_transform(uint32_t state[5], const uint8_t block[64])
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

static void ac_sha1_init(ac_sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

static void ac_sha1_update(ac_sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i, idx;

    idx = (size_t)(ctx->count & 0x3F);
    ctx->count += len;

    for (i = 0; i < len; i++) {
        ctx->buffer[idx++] = data[i];
        if (idx == 64) {
            ac_sha1_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void ac_sha1_final(ac_sha1_ctx_t *ctx, uint8_t digest[AC_SHA1_SIZE])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad = 0x80;
    int i;

    ac_sha1_update(ctx, &pad, 1);
    pad = 0;
    while ((ctx->count & 0x3F) != 56) {
        ac_sha1_update(ctx, &pad, 1);
    }

    for (i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (i * 8));
        ac_sha1_update(ctx, &b, 1);
    }

    for (i = 0; i < 5; i++) {
        digest[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

typedef struct {
    char path[AC_MAX_PATH];
    int64_t mtime_sec;
    int32_t mtime_nsec;
    int64_t file_size;
    uint8_t sha1[AC_SHA1_SIZE];
    int valid;
} ac_sha1_cache_entry_t;

static ac_sha1_cache_entry_t ac_sha1_cache[AC_SHA1_CACHE_SIZE];

static void ac_sha1_file(const char *path, uint8_t hash[AC_SHA1_SIZE])
{
    FILE *f;
    ac_sha1_ctx_t ctx;
    uint8_t buf[65536];
    size_t n;

    memset(hash, 0, AC_SHA1_SIZE);
    f = fopen(path, "rb");
    if (!f)
        return;

    ac_sha1_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ac_sha1_update(&ctx, buf, n);
    }
    ac_sha1_final(&ctx, hash);
    fclose(f);
}

#ifdef _WIN32
#include <sys/stat.h>
static void ac_sha1_file_cached(const char *path, uint8_t hash[AC_SHA1_SIZE])
{
    struct _stat st;
    int i, oldest;
    int64_t mtime_sec;
    int32_t mtime_nsec;

    if (_stat(path, &st) == 0) {
        mtime_sec = st.st_mtime;
        mtime_nsec = 0;
    } else {
        // Can't stat, just hash directly
        ac_sha1_file(path, hash);
        return;
    }

    // Search cache
    oldest = 0;
    for (i = 0; i < AC_SHA1_CACHE_SIZE; i++) {
        if (!ac_sha1_cache[i].valid) {
            oldest = i;
            break;
        }
        if (strcmp(ac_sha1_cache[i].path, path) == 0) {
            if (ac_sha1_cache[i].mtime_sec == mtime_sec &&
                ac_sha1_cache[i].file_size == st.st_size) {
                memcpy(hash, ac_sha1_cache[i].sha1, AC_SHA1_SIZE);
                return;
            }
            // File changed, re-hash and update cache entry
            ac_sha1_file(path, hash);
            memcpy(ac_sha1_cache[i].sha1, hash, AC_SHA1_SIZE);
            ac_sha1_cache[i].mtime_sec = mtime_sec;
            ac_sha1_cache[i].mtime_nsec = mtime_nsec;
            ac_sha1_cache[i].file_size = st.st_size;
            return;
        }
        // Track oldest for LRU eviction
        // (simple: just use insertion order, evict when full)
    }

    // Not found in cache, hash and store
    ac_sha1_file(path, hash);

    if (i < AC_SHA1_CACHE_SIZE) {
        oldest = i;
    }
    // Evict oldest entry
    Q_strlcpy(ac_sha1_cache[oldest].path, path, sizeof(ac_sha1_cache[oldest].path));
    memcpy(ac_sha1_cache[oldest].sha1, hash, AC_SHA1_SIZE);
    ac_sha1_cache[oldest].mtime_sec = mtime_sec;
    ac_sha1_cache[oldest].mtime_nsec = mtime_nsec;
    ac_sha1_cache[oldest].file_size = st.st_size;
    ac_sha1_cache[oldest].valid = 1;
}
#elif defined(__linux__)
#include <sys/stat.h>
static void ac_sha1_file_cached(const char *path, uint8_t hash[AC_SHA1_SIZE])
{
    struct stat st;
    int i;

    if (stat(path, &st) == 0) {
        for (i = 0; i < AC_SHA1_CACHE_SIZE; i++) {
            if (!ac_sha1_cache[i].valid) {
                break;
            }
            if (strcmp(ac_sha1_cache[i].path, path) == 0) {
                if (ac_sha1_cache[i].mtime_sec == st.st_mtime &&
                    ac_sha1_cache[i].file_size == st.st_size) {
                    memcpy(hash, ac_sha1_cache[i].sha1, AC_SHA1_SIZE);
                    return;
                }
                ac_sha1_file(path, hash);
                memcpy(ac_sha1_cache[i].sha1, hash, AC_SHA1_SIZE);
                ac_sha1_cache[i].mtime_sec = st.st_mtime;
                ac_sha1_cache[i].mtime_nsec = (int32_t)0;
                ac_sha1_cache[i].file_size = st.st_size;
                return;
            }
        }
        ac_sha1_file(path, hash);
        if (i >= AC_SHA1_CACHE_SIZE) i = 0;
        Q_strlcpy(ac_sha1_cache[i].path, path, sizeof(ac_sha1_cache[i].path));
        memcpy(ac_sha1_cache[i].sha1, hash, AC_SHA1_SIZE);
        ac_sha1_cache[i].mtime_sec = st.st_mtime;
        ac_sha1_cache[i].mtime_nsec = 0;
        ac_sha1_cache[i].file_size = st.st_size;
        ac_sha1_cache[i].valid = 1;
    } else {
        ac_sha1_file(path, hash);
    }
}
#endif /* _WIN32 / __linux__ */
#endif /* _WIN32 || __linux__ */

// Process entry structure
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    char name[AC_MAX_NAME];
} ac_process_entry_t;

// Module entry structure
typedef struct {
    char name[AC_MAX_NAME];
    char path[AC_MAX_PATH];
    uint8_t sha1[AC_SHA1_SIZE];
} ac_module_entry_t;

static ac_process_entry_t ac_processes[AC_MAX_PROCESSES];
static ac_module_entry_t  ac_modules[AC_MAX_MODULES];
static int ac_num_processes;
static int ac_num_modules;
static int ac_total_processes;   // real count including those not stored
static int ac_total_modules;
static qboolean ac_truncated_processes;
static qboolean ac_truncated_modules;

#ifdef _WIN32
/*
==============
ac_enumerate_processes_win32

Enumerate running processes using CreateToolhelp32Snapshot
==============
*/
static void ac_enumerate_processes_win32(void)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD my_pid;

    ac_num_processes = 0;
    ac_total_processes = 0;
    ac_truncated_processes = false;
    my_pid = GetCurrentProcessId();

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            // Skip our own process
            if (pe.th32ProcessID == my_pid)
                continue;

            ac_total_processes++;

            if (ac_num_processes >= AC_MAX_PROCESSES) {
                ac_truncated_processes = true;
                continue;
            }

            ac_processes[ac_num_processes].pid = pe.th32ProcessID;
            ac_processes[ac_num_processes].parent_pid = pe.th32ParentProcessID;
            Q_strlcpy(ac_processes[ac_num_processes].name, pe.szExeFile,
                      sizeof(ac_processes[ac_num_processes].name));
            ac_num_processes++;
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
}

/*
==============
ac_enumerate_modules_win32

Enumerate loaded modules using EnumProcessModules
==============
*/
static void ac_enumerate_modules_win32(void)
{
    HANDLE proc;
    HMODULE mods[AC_MAX_MODULES];
    DWORD needed;
    int i;

    ac_num_modules = 0;
    ac_total_modules = 0;
    ac_truncated_modules = false;

    proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                       FALSE, GetCurrentProcessId());
    if (!proc)
        return;

    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        int count = needed / sizeof(HMODULE);

        for (i = 0; i < count; i++) {
            char mod_name[AC_MAX_PATH];
            char mod_path[AC_MAX_PATH];

            ac_total_modules++;

            if (GetModuleFileNameExA(proc, mods[i], mod_path, sizeof(mod_path))) {
                const char *slash = strrchr(mod_path, '\\');
                if (!slash) slash = strrchr(mod_path, '/');
                Q_strlcpy(mod_name, slash ? slash + 1 : mod_path, sizeof(mod_name));
            } else {
                Q_strlcpy(mod_name, "unknown", sizeof(mod_name));
                mod_path[0] = 0;
            }

            if (ac_num_modules >= AC_MAX_MODULES) {
                ac_truncated_modules = true;
                continue;
            }

            Q_strlcpy(ac_modules[ac_num_modules].name, mod_name,
                      sizeof(ac_modules[ac_num_modules].name));
            Q_strlcpy(ac_modules[ac_num_modules].path, mod_path,
                      sizeof(ac_modules[ac_num_modules].path));
            ac_sha1_file_cached(mod_path, ac_modules[ac_num_modules].sha1);
            ac_num_modules++;
        }
    }

    CloseHandle(proc);
}
#endif /* _WIN32 */

#ifdef __linux__
/*
==============
ac_enumerate_processes_linux

Enumerate processes from /proc
==============
*/
static void ac_enumerate_processes_linux(void)
{
    DIR *dir;
    struct dirent *ent;
    FILE *f;
    char path[256];
    char line[1024];
    int my_pid;

    ac_num_processes = 0;
    ac_total_processes = 0;
    ac_truncated_processes = false;
    my_pid = getpid();

    dir = opendir("/proc");
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;

        int pid = atoi(ent->d_name);
        if (pid == my_pid)
            continue;

        ac_total_processes++;

        if (ac_num_processes >= AC_MAX_PROCESSES) {
            ac_truncated_processes = true;
            continue;
        }

        Q_snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                if (len > 0 && line[len-1] == '\n')
                    line[len-1] = 0;

                ac_processes[ac_num_processes].pid = pid;
                ac_processes[ac_num_processes].parent_pid = 0;
                Q_strlcpy(ac_processes[ac_num_processes].name, line,
                          sizeof(ac_processes[ac_num_processes].name));
                ac_num_processes++;
            }
            fclose(f);
        }
    }

    closedir(dir);
}

/*
==============
ac_enumerate_modules_linux

Read /proc/self/maps to find loaded libraries
==============
*/
static void ac_enumerate_modules_linux(void)
{
    FILE *f;
    char line[1024];
    char last_path[AC_MAX_PATH] = {0};

    ac_num_modules = 0;
    ac_total_modules = 0;
    ac_truncated_modules = false;

    f = fopen("/proc/self/maps", "r");
    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;

        // Parse: addr-addr perms offset dev inode [path]
        char *tok = strtok(line, " ");
        char *perms = NULL;
        char *filepath = NULL;
        int field = 0;

        tok = strtok(line, " ");
        field = 0;
        while (tok) {
            switch (field) {
            case 1: perms = tok; break;
            default:
                if (tok[0] == '/')
                    filepath = tok;
                break;
            }
            field++;
            tok = strtok(NULL, " ");
        }

        if (!filepath || !perms)
            continue;

        if (!strchr(perms, 'x'))
            continue;

        if (!strcmp(filepath, last_path))
            continue;
        Q_strlcpy(last_path, filepath, sizeof(last_path));

        ac_total_modules++;

        if (ac_num_modules >= AC_MAX_MODULES) {
            ac_truncated_modules = true;
            continue;
        }

        const char *slash = strrchr(filepath, '/');
        Q_strlcpy(ac_modules[ac_num_modules].name,
                  slash ? slash + 1 : filepath,
                  sizeof(ac_modules[ac_num_modules].name));
        Q_strlcpy(ac_modules[ac_num_modules].path, filepath,
                  sizeof(ac_modules[ac_num_modules].path));
        ac_sha1_file_cached(filepath, ac_modules[ac_num_modules].sha1);
        ac_num_modules++;
    }

    fclose(f);
}
#endif /* __linux__ */

/*
==============
CL_AC_ProcessCheckNow

Run the process/module enumeration and send snapshot to server.
Gated on having an active netchan. Called directly by the flush path
on model-reload / map-change events, and also by the command handler
when the server stuffs "cl_ac_process_check".
==============
*/
void CL_AC_ProcessCheckNow(void)
{
    int i, j;
    int proc_idx, mod_idx;
    byte flags;

    if (!cls.netchan.remote_address.type)
        return;

#ifdef _WIN32
    ac_enumerate_processes_win32();
    ac_enumerate_modules_win32();
#elif defined(__linux__)
    ac_enumerate_processes_linux();
    ac_enumerate_modules_linux();
#else
    ac_num_processes = 0;
    ac_num_modules = 0;
    ac_total_processes = 0;
    ac_total_modules = 0;
    ac_truncated_processes = false;
    ac_truncated_modules = false;
#endif

    Com_DPrintf("ProcessCheck: Found %d/%d processes, %d/%d modules\n",
                ac_num_processes, ac_total_processes,
                ac_num_modules, ac_total_modules);

    // Truncated flag if either side had more entries than we could store
    flags = 0;
    if (ac_truncated_processes || ac_truncated_modules)
        flags |= AC_PD_TRUNCATED;

    proc_idx = 0;
    mod_idx = 0;

    // Send in batches of ≤ AC_BATCH_MAX bytes each
    while (proc_idx < ac_num_processes || mod_idx < ac_num_modules) {
        int batch_procs, batch_mods;
        int bytes_used;
        byte batch_flags;

        // Pre-compute how many entries fit in one batch
        // Header: clc(1) + flags(1) + num_procs(4) + num_mods(4) = 10 bytes
        bytes_used = 10;
        batch_procs = 0;
        batch_mods = 0;

        // Count processes that fit
        for (i = proc_idx; i < ac_num_processes; i++) {
            int entry_size = 4 + 4 + 1 + (int)strlen(ac_processes[i].name);
            if (bytes_used + entry_size > AC_BATCH_MAX && batch_procs > 0)
                break;
            bytes_used += entry_size;
            batch_procs++;
        }

        // Count modules that fit with remaining budget
        for (i = mod_idx; i < ac_num_modules; i++) {
            int entry_size = 1 + (int)strlen(ac_modules[i].name)
                           + 1 + (int)strlen(ac_modules[i].path)
                           + AC_SHA1_SIZE;
            if (bytes_used + entry_size > AC_BATCH_MAX && batch_mods > 0)
                break;
            bytes_used += entry_size;
            batch_mods++;
        }

        // Final batch if nothing remains after this one
        batch_flags = flags;
        if (proc_idx + batch_procs >= ac_num_processes &&
            mod_idx + batch_mods >= ac_num_modules)
            batch_flags |= AC_PD_FINAL;

        // Write the batch
        MSG_WriteByte(clc_processdata);
        MSG_WriteByte(batch_flags);
        MSG_WriteLong(batch_procs);

        for (j = 0; j < batch_procs; j++) {
            size_t namelen = strlen(ac_processes[proc_idx].name);
            MSG_WriteLong(ac_processes[proc_idx].pid);
            MSG_WriteLong(ac_processes[proc_idx].parent_pid);
            MSG_WriteByte((byte)namelen);
            MSG_WriteData(ac_processes[proc_idx].name, namelen);
            proc_idx++;
        }

        MSG_WriteLong(batch_mods);

        for (j = 0; j < batch_mods; j++) {
            size_t namelen = strlen(ac_modules[mod_idx].name);
            size_t pathlen = strlen(ac_modules[mod_idx].path);
            MSG_WriteByte((byte)namelen);
            MSG_WriteData(ac_modules[mod_idx].name, namelen);
            MSG_WriteByte((byte)pathlen);
            MSG_WriteData(ac_modules[mod_idx].path, pathlen);
            MSG_WriteData(ac_modules[mod_idx].sha1, AC_SHA1_SIZE);
            mod_idx++;
        }

        // Abort on overflow (never send a corrupt message)
        if (msg_write.overflowed) {
            Com_WPrintf("ProcessCheck: message overflow, aborting batch\n");
            SZ_Clear(&msg_write);
            break;
        }

        Netchan_Transmit(&cls.netchan, msg_write.cursize, msg_write.data, 3);
        SZ_Clear(&msg_write);
    }

    Com_DPrintf("ProcessCheck: Sent %d/%d processes, %d/%d modules (truncated=%d)\n",
                proc_idx, ac_total_processes,
                mod_idx, ac_total_modules,
                (flags & AC_PD_TRUNCATED) ? 1 : 0);
}

/*
==============
CL_AC_ProcessCheck_f

Handler for server stufftext "cl_ac_process_check".
==============
*/
static void CL_AC_ProcessCheck_f(void)
{
    CL_AC_ProcessCheckNow();
}

/*
==============
CL_AC_ProcessInit

Register the cl_ac_process_check command
==============
*/
void CL_AC_ProcessInit(void)
{
    Cmd_AddCommand("cl_ac_process_check", CL_AC_ProcessCheck_f);
}

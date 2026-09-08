#include "client.h"
#include "discord_game_sdk.h"

extern cvar_t *fs_game;

static struct IDiscordCore* discord_core = NULL;
static struct IDiscordActivityManager* discord_activity_manager = NULL;
static qboolean discord_initialized = qfalse;

// Forward declarations for Discord SDK C API (not in extern "C" block in header)
static void Discord_CreateParamsSetDefault(struct DiscordCreateParams* params) {
    memset(params, 0, sizeof(struct DiscordCreateParams));
    params->application_version = DISCORD_APPLICATION_MANAGER_VERSION;
    params->user_version = DISCORD_USER_MANAGER_VERSION;
    params->image_version = DISCORD_IMAGE_MANAGER_VERSION;
    params->activity_version = DISCORD_ACTIVITY_MANAGER_VERSION;
    params->relationship_version = DISCORD_RELATIONSHIP_MANAGER_VERSION;
    params->lobby_version = DISCORD_LOBBY_MANAGER_VERSION;
    params->network_version = DISCORD_NETWORK_MANAGER_VERSION;
    params->overlay_version = DISCORD_OVERLAY_MANAGER_VERSION;
    params->storage_version = DISCORD_STORAGE_MANAGER_VERSION;
    params->store_version = DISCORD_STORE_MANAGER_VERSION;
    params->voice_version = DISCORD_VOICE_MANAGER_VERSION;
    params->achievement_version = DISCORD_ACHIEVEMENT_MANAGER_VERSION;
}

// Declare DiscordCreate with correct calling convention (DISCORD_API is empty on x64)
enum EDiscordResult DISCORD_API DiscordCreate(DiscordVersion version, struct DiscordCreateParams* params, struct IDiscordCore** result);

// Forward declarations
static const char* Discord_GetModDisplayName(const char* gamedir);

static void Discord_ActivityCallback(void* callback_data, enum EDiscordResult result) {
    (void)callback_data;
    if (result != DiscordResult_Ok) {
        Com_DPrintf("Discord activity update failed: %d\n", result);
    }
}

static void Discord_ClearCallback(void* callback_data, enum EDiscordResult result) {
    (void)callback_data;
    if (result != DiscordResult_Ok) {
        Com_DPrintf("Discord clear activity failed: %d\n", result);
    }
}

void Discord_Init(void) {
    if (discord_initialized) return;

    cvar_t *discord_client_id_cvar = Cvar_Get("discord_client_id", "1539464653564809277", CVAR_ARCHIVE);
    int64_t client_id = (int64_t)strtoll(discord_client_id_cvar->string, NULL, 10);

    if (client_id == 0) {
        return;
    }

    struct DiscordCreateParams params;
    Discord_CreateParamsSetDefault(&params);
    params.client_id = client_id;
    params.flags = DiscordCreateFlags_NoRequireDiscord;

    enum EDiscordResult result = DiscordCreate(DISCORD_VERSION, &params, &discord_core);
    if (result != DiscordResult_Ok) {
        Com_Printf("Discord SDK init failed: %d\n", result);
        return;
    }

    discord_core->get_activity_manager(discord_core, &discord_activity_manager);

    discord_initialized = qtrue;
    Com_Printf("Discord Rich Presence initialized (client_id: %lld)\n", (long long)client_id);
}

void Discord_Shutdown(void) {
    if (!discord_initialized) return;

    if (discord_core) {
        discord_core->destroy(discord_core);
        discord_core = NULL;
    }
    discord_activity_manager = NULL;
    discord_initialized = qfalse;
    Com_Printf("Discord Rich Presence shutdown\n");
}

void Discord_RunCallbacks(void) {
    if (!discord_initialized || !discord_core) return;

    enum EDiscordResult result = discord_core->run_callbacks(discord_core);
    if (result == DiscordResult_NotRunning) {
        Com_DPrintf("Discord not running, Rich Presence disabled\n");
        Discord_Shutdown();
    } else if (result != DiscordResult_Ok) {
        Com_DPrintf("Discord run_callbacks error: %d\n", result);
    }
}

void Discord_UpdatePresenceMapMod(const char* mapname, const char* gamedir, int64_t start_timestamp) {
    if (!discord_initialized || !discord_activity_manager) return;

    const char* mod_display = Discord_GetModDisplayName(gamedir);

    struct DiscordActivity activity;
    memset(&activity, 0, sizeof(activity));

    activity.type = DiscordActivityType_Playing;
    activity.instance = 1;

    // State = Mod name (línea principal)
    if (mod_display && mod_display[0]) {
        Q_strncpyz(activity.state, mod_display, sizeof(activity.state));
    }

    // Details = Map name (línea secundaria)
    if (mapname && mapname[0]) {
        Q_strncpyz(activity.details, mapname, sizeof(activity.details));
    }

    if (start_timestamp > 0) {
        activity.timestamps.start = start_timestamp;
    }

    activity.instance = 1;

    discord_activity_manager->update_activity(discord_activity_manager, &activity, NULL, Discord_ActivityCallback);
}

void Discord_ClearActivity(void) {
    if (!discord_initialized || !discord_activity_manager) return;

    discord_activity_manager->clear_activity(discord_activity_manager, NULL, Discord_ClearCallback);
}

const char* Discord_GetModDisplayName(const char* gamedir) {
    if (!gamedir || !gamedir[0]) return "Quake 2";
    if (!strcmp(gamedir, "dday")) return "Dday Normandy";
    if (!strcmp(gamedir, "rogue")) return "Ground Zero";
    if (!strcmp(gamedir, "xatrix")) return "The Reckoning";
    if (!strcmp(gamedir, "zaero")) return "Zaero";
    if (!strcmp(gamedir, "ctf")) return "CTF";
    if (!strcmp(gamedir, "action")) return "Action Quake 2";
    if (!strcmp(gamedir, "aq2")) return "Action Quake 2";
    return gamedir;
}

qboolean Discord_IsInitialized(void) {
    return discord_initialized;
}
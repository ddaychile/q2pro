#include "core.h"
#include "activity_manager.h"
#include "types.h"

#include <cstring>

extern "C" {

struct DiscordCoreWrapper {
    discord::Core* core;
    discord::ActivityManager* activity_manager;
    bool initialized;
    int64_t start_timestamp;
};

static DiscordCoreWrapper g_discord = {nullptr, nullptr, false, 0};

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

int Discord_Init(int64_t client_id) {
    if (g_discord.initialized) return 1;
    if (client_id == 0) return 0;

    discord::Result result = discord::Core::Create(client_id, static_cast<uint64_t>(discord::CreateFlags::NoRequireDiscord), &g_discord.core);
    
    if (result != discord::Result::Ok || !g_discord.core) {
        return 0;
    }

    g_discord.activity_manager = &g_discord.core->ActivityManager();
    g_discord.initialized = true;
    g_discord.start_timestamp = 0;

    return 1;
}

void Discord_Shutdown(void) {
    if (!g_discord.initialized) return;

    if (g_discord.core) {
        delete g_discord.core;
        g_discord.core = nullptr;
    }
    g_discord.activity_manager = nullptr;
    g_discord.initialized = false;
}

void Discord_RunCallbacks(void) {
    if (!g_discord.initialized || !g_discord.core) return;

    discord::Result result = g_discord.core->RunCallbacks();
    if (result == discord::Result::NotRunning) {
        Discord_Shutdown();
    }
}

void Discord_UpdatePresenceMapMod(const char* mapname, const char* gamedir, int64_t start_timestamp) {
    if (!g_discord.initialized || !g_discord.activity_manager) return;

    const char* mod_display = Discord_GetModDisplayName(gamedir);

    discord::Activity activity = {};
    activity.SetType(discord::ActivityType::Playing);
    activity.SetInstance(true);

    if (mod_display && mod_display[0]) {
        activity.SetState(mod_display);
    }

    if (mapname && mapname[0]) {
        activity.SetDetails(mapname);
    }

    if (start_timestamp > 0) {
        activity.GetTimestamps().SetStart(start_timestamp);
    }

    g_discord.activity_manager->UpdateActivity(activity, [](discord::Result result) {
        // Callback
    });
}

void Discord_ClearActivity(void) {
    if (!g_discord.initialized || !g_discord.activity_manager) return;

    g_discord.activity_manager->ClearActivity([](discord::Result result) {
        // Callback
    });
}

int Discord_IsInitialized(void) {
    return g_discord.initialized ? 1 : 0;
}

} // extern "C"
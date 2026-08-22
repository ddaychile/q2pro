#ifndef __DISCORD_H__
#define __DISCORD_H__

#ifdef __cplusplus
extern "C" {
#endif

int Discord_Init(int64_t client_id);
void Discord_Shutdown(void);
void Discord_RunCallbacks(void);
void Discord_UpdatePresenceMapMod(const char* mapname, const char* gamedir, int64_t start_timestamp);
void Discord_ClearActivity(void);
int Discord_IsInitialized(void);

#ifdef __cplusplus
}
#endif

#endif
/*
 * NOTE(notscared) Precache miss logging - logs assets that couldn't be found
 * to help identify missing files in the game data.
 */

#ifndef PRECACHE_LOG_H
#define PRECACHE_LOG_H

void PrecacheLog_Init(void);
void PrecacheLog_Shutdown(void);

// Log a missing asset. Type is "image", "model", or "sound".
// Only logs each unique path once per session.
void PrecacheLog_Miss(const char *type, const char *path);

#endif // PRECACHE_LOG_H

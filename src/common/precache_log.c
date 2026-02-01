/*
 * NOTE(notscared) Precache miss logging - logs assets that couldn't be found
 * to help identify missing files in the game data.
 *
 * This runs unconditionally (no cvar) - in a perfect world, the log file
 * won't be written because there are no misses.
 */

#include "shared/shared.h"
#include "common/common.h"
#include "common/files.h"
#include "common/hash_map.h"

static qhandle_t precache_logfile;
static hash_map_t *logged_paths;  // tracks already-logged paths to avoid duplicates

void PrecacheLog_Init(void)
{
    char buffer[MAX_OSPATH];

    // Create hash map to track logged paths (avoid duplicates)
    // Use string hash and comparison functions for char* keys
    logged_paths = HashMap_Create(char *, bool, &HashStr, &HashStrCmp);

    // Open log file - always create fresh each session
    precache_logfile = FS_EasyOpenFile(buffer, sizeof(buffer),
        FS_MODE_WRITE | FS_FLAG_TEXT,
        "logs/", "precache-miss", ".log");

    if (precache_logfile) {
        Com_DPrintf("Precache miss logging to %s\n", buffer);
    }
}

void PrecacheLog_Shutdown(void)
{
    if (logged_paths) {
        // Free all the strdup'd keys
        uint32_t size = HashMap_Size(logged_paths);
        for (uint32_t i = 0; i < size; i++) {
            char **key = HashMap_GetKey(char *, logged_paths, i);
            if (key && *key) {
                Z_Free(*key);
            }
        }
        HashMap_Destroy(logged_paths);
        logged_paths = NULL;
    }

    if (precache_logfile) {
        FS_CloseFile(precache_logfile);
        precache_logfile = 0;
    }
}

void PrecacheLog_Miss(const char *type, const char *path)
{
    if (!precache_logfile || !logged_paths)
        return;

    // Skip empty paths
    if (!path || !*path)
        return;

    // Check if we've already logged this path
    if (HashMap_Lookup(bool, logged_paths, &path))
        return;

    // Mark as logged (need to copy since hash map stores pointer)
    char *path_copy = Z_CopyString(path);
    bool logged = true;
    HashMap_Insert(logged_paths, &path_copy, &logged);

    // Write to log file
    FS_FPrintf(precache_logfile, "%s: %s\n", type, path);

    // Flush immediately so we don't lose data on crash
    FS_Flush(precache_logfile);
}

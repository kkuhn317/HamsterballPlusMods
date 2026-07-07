/* GhostSaverUtils.cpp — Utility functions for ghost saving/loading.
 * Race name lookup, filename conversion, file I/O, game state checks.
 */
#include "GhostSaver.h"

 /* Get App pointer from global */
static DWORD get_app(void) {
    if (IsBadReadPtr((void*)GLOBAL_APP_PTR, 4)) return 0;
    DWORD app = *(DWORD*)GLOBAL_APP_PTR;
    if (!app || app < 0x10000) return 0;
    return app;
}

/* Check if we're in an active Time Trial race */
static int is_time_trial_active(void) {
    DWORD app = get_app();
    if (!app) return 0;
    if (IsBadReadPtr((void*)(app + APP_220_PROFILE), 4)) return 0;
    DWORD profile = *(DWORD*)(app + APP_220_PROFILE);
    if (!profile || profile < 0x10000) return 0;
    if (IsBadReadPtr((void*)(profile + 0x11), 1)) return 0;
    if (*(BYTE*)(profile + 0x11) == 0) return 0;
    if (IsBadReadPtr((void*)(app + APP_234_PARTY_MODE), 1)) return 0;
    if (*(BYTE*)(app + APP_234_PARTY_MODE) != 0) return 0;
    return 1;
}

/* Pre-call TT check: can't read profile yet, use party mode as heuristic */
static int is_time_trial_precheck(void) {
    DWORD app = get_app();
    if (!app) return 0;
    if (IsBadReadPtr((void*)(app + APP_234_PARTY_MODE), 1)) return 0;
    if (*(BYTE*)(app + APP_234_PARTY_MODE) != 0) return 0;
    return 1;
}

/* Read race name from the recording BTT at App+0x90C */
static int get_race_name(char* out, int outLen) {
    out[0] = '\0';
    DWORD app = get_app();
    if (!app) return 0;
    if (IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4)) return 0;
    DWORD btt = *(DWORD*)(app + APP_90C_RECORDING);
    if (!btt || btt < 0x10000) return 0;
    if (IsBadReadPtr((void*)(btt + BTT_NAME), 1)) return 0;
    char* name = (char*)(btt + BTT_NAME);
    if (name[0] < 0x20 || name[0] > 0x7E) return 0;
    for (int i = 0; i < 64 && name[i]; i++) {
        if (name[i] < 0x20 || name[i] > 0x7E) { name[i] = '\0'; break; }
    }
    strncpy(out, name, outLen - 1);
    out[outLen - 1] = '\0';
    return 1;
}

/* Walk the race name table at 0x4F7080 until NULL/invalid pointer.
 * Vanilla game has 16 entries; tournament mods may extend it. */
static int get_race_name_table_count(void) {
    DWORD* nameTable = (DWORD*)RACE_NAME_TABLE;
    int i;
    for (i = 0; i < 64; i++) {
        if (IsBadReadPtr(nameTable + i, 4)) return i;
        DWORD namePtr = nameTable[i];
        if (!namePtr || namePtr < 0x400000) return i;
        if (IsBadReadPtr((void*)namePtr, 2)) return i;
        char c = *(char*)namePtr;
        if (c < 0x20 || c > 0x7E) return i;
    }
    return i;
}

static int get_race_name_by_index(DWORD race_index, char* out, int outLen) {
    int tableCount = get_race_name_table_count();
    if ((int)race_index >= tableCount) return 0;
    DWORD* nameTable = (DWORD*)RACE_NAME_TABLE;
    char* name = (char*)nameTable[race_index];
    if (!name || (DWORD)name < 0x400000) return 0;
    if (IsBadReadPtr(name, 2)) return 0;
    if (name[0] < 0x20 || name[0] > 0x7E) return 0;
    strncpy(out, name, outLen - 1);
    out[outLen - 1] = '\0';
    return 1;
}

/* Convert race name to ghost filename.
 * "Warm-Up Race" -> "Warm-Up.ghost"
 * Title-case every word, strip " RACE" suffix, sanitize invalid chars. */
static void race_name_to_filename(const char* raceName, char* out, int outLen) {
    char base[128];
    strncpy(base, raceName, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    /* Strip " RACE" suffix (case-insensitive) */
    int len = (int)strlen(base);
    if (len >= 5 && _stricmp(base + len - 5, " RACE") == 0)
        base[len - 5] = '\0';

    /* Title-case: first letter of each word uppercase, rest lowercase */
    int newWord = 1;
    for (int i = 0; base[i]; i++) {
        char c = base[i];
        if (c == ' ' || c == '-' || c == '_')
            newWord = 1;
        else if (newWord) {
            if (c >= 'a' && c <= 'z') base[i] = c - 32;
            newWord = 0;
        }
        else {
            if (c >= 'A' && c <= 'Z') base[i] = c + 32;
        }
    }

    /* Sanitize invalid Windows filename chars */
    for (int i = 0; base[i]; i++) {
        char c = base[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            base[i] = '_';
    }

    snprintf(out, outLen, "%s%s.ghost", g_ghostDir, base);
}

/* Get saved time for a race from its .ghost file (NO_TIME if not found) */
static int get_saved_time(const char* raceName) {
    char path[MAX_PATH];
    race_name_to_filename(raceName, path, sizeof(path));

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NO_TIME;

    int result = NO_TIME;
    DWORD magic, version, time;
    DWORD bytesRead;

    if (ReadFile(h, &magic, 4, &bytesRead, NULL) && magic == GHOST_MAGIC &&
        ReadFile(h, &version, 4, &bytesRead, NULL) && version == GHOST_VERSION &&
        ReadFile(h, &time, 4, &bytesRead, NULL)) {
        result = (int)time;
    }

    CloseHandle(h);
    return result;
}

/* Save ghost to a .tmp file then atomically rename.
 * Checks each WriteFile for short writes to prevent corruption. */
static void save_ghost_for_race(const char* raceName, int time,
    DWORD(*snaps)[10], int count) {
    char path[MAX_PATH];
    race_name_to_filename(raceName, path, sizeof(path));

    char tmpPath[MAX_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

    HANDLE h = CreateFileA(tmpPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        log_fmt("ERROR: cannot create %s", tmpPath);
        return;
    }

    DWORD written;
    int ok = 1;
    DWORD magic = GHOST_MAGIC;
    DWORD version = GHOST_VERSION;
    DWORD frameCount = (DWORD)count;

    if (!WriteFile(h, &magic, 4, &written, NULL) || written != 4) ok = 0;
    if (ok && (!WriteFile(h, &version, 4, &written, NULL) || written != 4)) ok = 0;
    if (ok && (!WriteFile(h, (DWORD*)&time, 4, &written, NULL) || written != 4)) ok = 0;
    if (ok && (!WriteFile(h, &frameCount, 4, &written, NULL) || written != 4)) ok = 0;

    if (ok && count > 0) {
        DWORD totalBytes = (DWORD)count * SNAP_BYTES;
        if (!WriteFile(h, snaps, totalBytes, &written, NULL) || written != totalBytes) {
            log_fmt("ERROR: short write — expected %d bytes, got %d", totalBytes, written);
            ok = 0;
        }
    }

    if (ok) FlushFileBuffers(h);
    CloseHandle(h);

    if (ok) {
        if (MoveFileExA(tmpPath, path, MOVEFILE_REPLACE_EXISTING)) {
            log_fmt("Saved %s (%d frames, time=%d)", path, count, time);
        }
        else {
            log_fmt("ERROR: MoveFileEx failed (err=%d)", GetLastError());
            DeleteFileA(tmpPath);
        }
    }
    else {
        log_fmt("ERROR: write failed for %s — keeping existing ghost", path);
        DeleteFileA(tmpPath);
    }
}

/* Initialize Ghosts/ directory in the game's working directory */
static void init_ghost_dir(void) {
    char dir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, dir);
    snprintf(g_ghostDir, MAX_PATH, "%s\\Ghosts\\", dir);
    CreateDirectoryA(g_ghostDir, NULL);
}

/* ── Dynamic snapshot buffer ── */

static void snaps_reserve(int needed) {
    if (needed <= g_rawCapacity) return;
    int newCap = g_rawCapacity ? g_rawCapacity : 5000;
    while (newCap < needed) newCap *= 2;
    DWORD(*newBuf)[SNAP_DWORDS] = (DWORD(*)[SNAP_DWORDS])
        realloc(g_rawSnaps, newCap * SNAP_BYTES);
    if (!newBuf) {
        log_fmt("ERROR: realloc failed for %d snaps", newCap);
        return;
    }
    g_rawSnaps = newBuf;
    g_rawCapacity = newCap;
}

static void snaps_reset(void) {
    if (g_rawSnaps) {
        free(g_rawSnaps);
        g_rawSnaps = NULL;
    }
    g_rawCount = 0;
    g_rawCapacity = 0;
}

/* Clean up the dummy recording BTT if it's still at App+0x90C.
 * Double-free guard: only destroy if App+0x90C still points to our dummy. */
static void cleanup_dummy_btt(DWORD app) {
    if (!g_dummyRecording || g_dummyRecording < 0x10000) return;

    DWORD curr90C = 0;
    if (!IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4))
        curr90C = *(DWORD*)(app + APP_90C_RECORDING);

    if (curr90C == g_dummyRecording) {
        if (!IsBadReadPtr((void*)g_dummyRecording, 4)) {
            DWORD vt = *(DWORD*)g_dummyRecording;
            if (vt == BTT_VTABLE) {
                CallMethod<void>(RVA_BTT_DTOR, (void*)g_dummyRecording, (DWORD)1);
                log_fmt("Cleaned up dummy BTT at 0x%X", g_dummyRecording);
            }
            else {
                Call<void>(RVA_GAME_FREE, (void*)g_dummyRecording);
                log_fmt("Cleaned up dummy BTT via game_free (bad vtable)");
            }
        }
        *(DWORD*)(app + APP_90C_RECORDING) = 0;
    }
    g_dummyRecording = 0;
}
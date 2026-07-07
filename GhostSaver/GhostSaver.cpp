/* GhostSaver.cpp — Persistent Ghost Data for Time Trial Mode (HB+ API)
 *
 * Saves/loads ghost ball recordings to/from .ghost files in the Ghosts/
 * directory, so Time Trial ghosts survive game restarts.
 *
 * Converted from ghost_saver.c v25.4 (bass.dll proxy) to HB+ API.
 * Key simplification: no background thread, no CRITICAL_SECTION,
 * no inline asm — everything runs on the main thread via callbacks.
 *
 * All v25.4 functionality preserved:
 * - Pre-inject ghost BTT at App+0x910 before Board_ctor
 * - Dummy recording BTT at App+0x90C (NO_TIME protector)
 * - Post-hook old BTT destruction
 * - Goal flag 0->1 transition detection
 * - Game's own BTT recording read at goal time
 * - Per-race .ghost files + Previous_Run.ghost
 * - Time comparison (only save if faster)
 * - Dynamic snapshot buffer (realloc growth)
 * - Atomic save (.tmp + MoveFileEx)
 * - Race name table dynamic extent
 * - Filename sanitization + title-case conversion
 */
#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

 /* ═══════════════════════════════════════════════════════════════════════════
  * Constants
  * ═══════════════════════════════════════════════════════════════════════════ */

#define GLOBAL_APP_PTR     0x005341E0
#define BTT_SIZE           0x528
#define BTT_BEST_TIME      0x524
#define BTT_NAME           0x424
#define SNAP_SIZE          0x28
#define NO_TIME            9999999
#define SNAP_DWORDS        10
#define SNAP_BYTES         40
#define BTT_VTABLE         0x004D262C

#define APP_90C_RECORDING  0x90C
#define APP_910_PLAYBACK    0x910
#define APP_5D6_GOAL_FLAG   0x5D6
#define APP_234_PARTY_MODE  0x234
#define APP_220_PROFILE     0x220

  /* RVA offsets for Call<>/CallMethod<> (absolute - 0x400000) */
#define RVA_BTT_CTOR       0x27660
#define RVA_BTT_DTOR       0x278C0
#define RVA_ALIST_APPEND   0x53780
#define RVA_OPERATOR_NEW   0xBA57B
#define RVA_GAME_FREE      0xBA74D

/* Absolute addresses */
#define ADDR_APP_START_PRACTICE  0x00428C50
#define RACE_NAME_TABLE          0x004F7080

/* Ghost binary file format */
#define GHOST_MAGIC    0x47485347
#define GHOST_VERSION  1

/* ═══════════════════════════════════════════════════════════════════════════
 * Global state
 * ═══════════════════════════════════════════════════════════════════════════ */

static IModAPI* g_api = nullptr;
static char g_currentRaceName[128] = "";
static char g_hookRaceName[128] = "";
static int g_recording = 0;
static int g_raceFinished = 0;
static int g_prevGoalFlag = 0;
static DWORD g_prevRecording = 0;
static DWORD g_savedOldPlayback = 0;
static DWORD g_dummyRecording = 0;
static char g_ghostDir[MAX_PATH] = "";
static bool g_enabled = true;

/* Dynamic snapshot buffer */
static DWORD(*g_rawSnaps)[SNAP_DWORDS] = NULL;
static int g_rawCount = 0;
static int g_rawCapacity = 0;

/* Hook typedef */
typedef void(__fastcall* AppStartPracticeRace_t)(void* app, void* edx, DWORD race_index);
static AppStartPracticeRace_t orig_AppStartPracticeRace = nullptr;

/* Logging (disabled by default) */
#define LOGGING_ENABLED 0

static void log_msg(const char* msg) {
#if LOGGING_ENABLED
    if (!g_ghostDir[0]) return;
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%s..\\ghost_saver_log.txt", g_ghostDir);
    HANDLE h = CreateFileA(logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
        WriteFile(h, "\r\n", 2, &w, NULL);
        CloseHandle(h);
    }
#endif
}

static void log_fmt(const char* fmt, ...) {
#if LOGGING_ENABLED
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log_msg(buf);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility functions
 * ═══════════════════════════════════════════════════════════════════════════ */

static DWORD get_app(void) {
    if (IsBadReadPtr((void*)GLOBAL_APP_PTR, 4)) return 0;
    DWORD app = *(DWORD*)GLOBAL_APP_PTR;
    if (!app || app < 0x10000) return 0;
    return app;
}

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

static int is_time_trial_precheck(void) {
    DWORD app = get_app();
    if (!app) return 0;
    if (IsBadReadPtr((void*)(app + APP_234_PARTY_MODE), 1)) return 0;
    if (*(BYTE*)(app + APP_234_PARTY_MODE) != 0) return 0;
    return 1;
}

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

static void race_name_to_filename(const char* raceName, char* out, int outLen) {
    char base[128];
    strncpy(base, raceName, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';

    int len = (int)strlen(base);
    if (len >= 5 && _stricmp(base + len - 5, " RACE") == 0)
        base[len - 5] = '\0';

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

    for (int i = 0; base[i]; i++) {
        char c = base[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            base[i] = '_';
    }

    snprintf(out, outLen, "%s%s.ghost", g_ghostDir, base);
}

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

static void init_ghost_dir(void) {
    char dir[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, dir);
    snprintf(g_ghostDir, MAX_PATH, "%s\\Ghosts\\", dir);
    CreateDirectoryA(g_ghostDir, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dynamic snapshot buffer
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Dummy BTT cleanup
 * ═══════════════════════════════════════════════════════════════════════════ */

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

/* ═══════════════════════════════════════════════════════════════════════════
 * Ghost injection (called from hook)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void inject_saved_ghost(const char* raceName) {
    DWORD app = get_app();
    if (!app) return;

    char path[MAX_PATH];
    race_name_to_filename(raceName, path, sizeof(path));

    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_fmt("No ghost file for '%s'", raceName);
        return;
    }

    int savedTime = NO_TIME;
    DWORD(*savedSnaps)[10] = NULL;
    int savedCount = 0;

    DWORD magic, version, time, frameCount;
    DWORD bytesRead;

    if (ReadFile(hf, &magic, 4, &bytesRead, NULL) && magic == GHOST_MAGIC &&
        ReadFile(hf, &version, 4, &bytesRead, NULL) && version == GHOST_VERSION &&
        ReadFile(hf, &time, 4, &bytesRead, NULL) &&
        ReadFile(hf, &frameCount, 4, &bytesRead, NULL) &&
        frameCount > 0 && frameCount < 200000) {

        savedTime = (int)time;
        savedSnaps = (DWORD(*)[10])malloc(frameCount * 10 * sizeof(DWORD));
        if (savedSnaps) {
            DWORD totalBytes = frameCount * SNAP_BYTES;
            if (ReadFile(hf, savedSnaps, totalBytes, &bytesRead, NULL) &&
                bytesRead == totalBytes) {
                savedCount = (int)frameCount;
            }
            else {
                log_fmt("ERROR: short read for ghost");
                free(savedSnaps);
                savedSnaps = NULL;
            }
        }
    }
    else {
        log_fmt("ERROR: bad ghost file header for '%s'", raceName);
    }
    CloseHandle(hf);

    if (!savedSnaps || savedCount == 0) {
        if (savedSnaps) free(savedSnaps);
        return;
    }

    log_fmt("Loading ghost: '%s' time=%d frames=%d", raceName, savedTime, savedCount);

    void* btt = Call<void*>(RVA_OPERATOR_NEW, (SIZE_T)BTT_SIZE);
    if (!btt) { free(savedSnaps); return; }
    log_fmt("BTT allocated at %p", btt);

    CallMethod<void>(RVA_BTT_CTOR, btt);

    DWORD vtable = *(DWORD*)btt;
    if (vtable != BTT_VTABLE) {
        log_fmt("ERROR: BTT ctor failed — vtable=0x%X", vtable);
        free(savedSnaps);
        Call<void>(RVA_GAME_FREE, btt);
        return;
    }

    *(DWORD*)((char*)btt + BTT_BEST_TIME) = savedTime;

    char* bttName = (char*)((char*)btt + BTT_NAME);
    strncpy(bttName, raceName, 127);
    bttName[127] = '\0';

    DWORD* alist = (DWORD*)((char*)btt + 4);
    for (int i = 0; i < savedCount; i++) {
        DWORD* snap = (DWORD*)Call<void*>(RVA_OPERATOR_NEW, (SIZE_T)SNAP_SIZE);
        if (!snap) continue;
        memcpy(snap, savedSnaps[i], SNAP_BYTES);
        CallMethod<void>(RVA_ALIST_APPEND, alist, snap);
    }
    log_fmt("Appended %d snapshots", savedCount);

    *(int*)((char*)btt + 0x41C) = 0;

    *(DWORD*)(app + APP_910_PLAYBACK) = (DWORD)btt;
    free(savedSnaps);
    log_fmt("Ghost injected into App+0x910 (btt=%p)", btt);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * App_StartPracticeRace hook
 * ═══════════════════════════════════════════════════════════════════════════ */

static void __fastcall hook_AppStartPracticeRace(void* app_ptr, void* edx, DWORD race_index) {
    DWORD app = (DWORD)app_ptr;
    log_fmt("HOOK: App_StartPracticeRace(race_index=%d)", race_index);

    /* Clean up stale dummy from previous race */
    if (g_dummyRecording && g_dummyRecording > 0x10000) {
        DWORD curr90C = 0;
        if (!IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4))
            curr90C = *(DWORD*)(app + APP_90C_RECORDING);
        if (curr90C == g_dummyRecording) {
            if (!IsBadReadPtr((void*)g_dummyRecording, 4)) {
                DWORD vt = *(DWORD*)g_dummyRecording;
                if (vt == BTT_VTABLE)
                    CallMethod<void>(RVA_BTT_DTOR, (void*)g_dummyRecording, (DWORD)1);
                else
                    Call<void>(RVA_GAME_FREE, (void*)g_dummyRecording);
            }
            *(DWORD*)(app + APP_90C_RECORDING) = 0;
        }
        g_dummyRecording = 0;
    }

    g_savedOldPlayback = 0;

    if (is_time_trial_precheck()) {
        char raceName[128] = "";
        if (get_race_name_by_index(race_index, raceName, sizeof(raceName)) && raceName[0]) {
            log_fmt("HOOK: pre-inject for race '%s'", raceName);
            strncpy(g_hookRaceName, raceName, sizeof(g_hookRaceName) - 1);
            g_hookRaceName[sizeof(g_hookRaceName) - 1] = '\0';

            int savedTime = get_saved_time(raceName);
            if (savedTime != NO_TIME) {
                if (!IsBadReadPtr((void*)(app + APP_910_PLAYBACK), 4)) {
                    DWORD existing = *(DWORD*)(app + APP_910_PLAYBACK);
                    if (existing && existing > 0x10000) {
                        g_savedOldPlayback = existing;
                    }
                    inject_saved_ghost(raceName);

                    DWORD newPlayback = 0;
                    if (!IsBadReadPtr((void*)(app + APP_910_PLAYBACK), 4))
                        newPlayback = *(DWORD*)(app + APP_910_PLAYBACK);

                    int injectFailed = 0;
                    if (g_savedOldPlayback && newPlayback == g_savedOldPlayback) {
                        log_msg("inject failed — leaving App+0x910 unchanged");
                        g_savedOldPlayback = 0;
                        injectFailed = 1;
                    }

                    if (!injectFailed && newPlayback && newPlayback > 0x10000 &&
                        !IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4)) {
                        DWORD recording = *(DWORD*)(app + APP_90C_RECORDING);
                        if (recording && recording > 0x10000 &&
                            !IsBadReadPtr((void*)(recording + BTT_BEST_TIME), 4)) {
                            int oldTime = *(int*)((char*)recording + BTT_BEST_TIME);
                            if (oldTime != NO_TIME) {
                                log_fmt("Neutralizing old recording time %d -> NO_TIME", oldTime);
                                *(int*)((char*)recording + BTT_BEST_TIME) = NO_TIME;
                            }
                        }
                        if (!recording || recording < 0x10000) {
                            log_msg("Pre-creating dummy recording BTT");
                            void* dummyRec = Call<void*>(RVA_OPERATOR_NEW, (SIZE_T)BTT_SIZE);
                            if (dummyRec) {
                                CallMethod<void>(RVA_BTT_CTOR, dummyRec);
                                DWORD vt = *(DWORD*)dummyRec;
                                if (vt == BTT_VTABLE) {
                                    *(DWORD*)((char*)dummyRec + BTT_BEST_TIME) = NO_TIME;
                                    *(DWORD*)(app + APP_90C_RECORDING) = (DWORD)dummyRec;
                                    g_dummyRecording = (DWORD)dummyRec;
                                    log_fmt("Dummy recording BTT at 0x%X", (DWORD)dummyRec);
                                }
                                else {
                                    log_fmt("ERROR: dummy BTT ctor vtable=0x%X", vt);
                                    Call<void>(RVA_GAME_FREE, dummyRec);
                                }
                            }
                        }
                    }
                }
            }
            else {
                log_fmt("No saved ghost for '%s', clearing playback", raceName);
                if (!IsBadReadPtr((void*)(app + APP_910_PLAYBACK), 4)) {
                    DWORD existing = *(DWORD*)(app + APP_910_PLAYBACK);
                    if (existing && existing > 0x10000)
                        g_savedOldPlayback = existing;
                    *(DWORD*)(app + APP_910_PLAYBACK) = 0;
                }
            }
        }
    }

    /* Call original */
    orig_AppStartPracticeRace(app_ptr, edx, race_index);

    /* Destroy old App+0x910 BTT we replaced */
    if (g_savedOldPlayback && g_savedOldPlayback > 0x10000) {
        if (!IsBadReadPtr((void*)g_savedOldPlayback, 4)) {
            DWORD vt = *(DWORD*)g_savedOldPlayback;
            if (vt == BTT_VTABLE) {
                log_fmt("Destroying old playback BTT at 0x%X", g_savedOldPlayback);
                CallMethod<void>(RVA_BTT_DTOR, (void*)g_savedOldPlayback, (DWORD)1);
            }
            else {
                log_fmt("WARNING: old playback BTT vtable=0x%X — skipping", vt);
            }
        }
        g_savedOldPlayback = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Race state monitoring (called from onGameUpdate)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void check_race_state(void) {
    DWORD app = get_app();
    if (!app) return;

    int tt = is_time_trial_active();
    if (!tt) {
        if (g_recording) {
            log_fmt("Left Time Trial mode (was recording %d frames)", g_rawCount);
            g_recording = 0;
            g_raceFinished = 0;
            snaps_reset();
            g_prevGoalFlag = 0;
            g_currentRaceName[0] = '\0';
            g_hookRaceName[0] = '\0';
        }
        cleanup_dummy_btt(app);
        if (!IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4))
            g_prevRecording = *(DWORD*)(app + APP_90C_RECORDING);
        return;
    }

    DWORD currRecording = 0;
    if (!IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4))
        currRecording = *(DWORD*)(app + APP_90C_RECORDING);

    if (currRecording != g_prevRecording && currRecording && currRecording > 0x10000) {
        g_prevRecording = currRecording;

        char raceName[128];
        if (g_hookRaceName[0]) {
            strncpy(raceName, g_hookRaceName, sizeof(raceName) - 1);
            raceName[sizeof(raceName) - 1] = '\0';
        }
        else {
            get_race_name(raceName, sizeof(raceName));
        }

        if (raceName[0]) {
            strncpy(g_currentRaceName, raceName, sizeof(g_currentRaceName) - 1);
            g_currentRaceName[sizeof(g_currentRaceName) - 1] = '\0';
            snaps_reset();
            g_recording = 1;
            g_raceFinished = 0;
            if (!IsBadReadPtr((void*)(app + APP_5D6_GOAL_FLAG), 1))
                g_prevGoalFlag = *(BYTE*)(app + APP_5D6_GOAL_FLAG);
            else
                g_prevGoalFlag = 0;
            log_fmt("RACE START: '%s' (BTT=0x%X)", raceName, currRecording);
        }
        else {
            log_fmt("Race detected but name not ready, will retry");
            g_prevRecording = 0;
        }
    }

    if (g_recording && !g_raceFinished) {
        if (!IsBadReadPtr((void*)(app + APP_5D6_GOAL_FLAG), 1)) {
            BYTE goalFlag = *(BYTE*)(app + APP_5D6_GOAL_FLAG);
            if (goalFlag && !g_prevGoalFlag) {
                g_raceFinished = 1;

                int finishTime = NO_TIME;
                DWORD btt = 0;
                if (!IsBadReadPtr((void*)(app + APP_90C_RECORDING), 4)) {
                    btt = *(DWORD*)(app + APP_90C_RECORDING);
                    if (btt && btt > 0x10000 &&
                        !IsBadReadPtr((void*)(btt + BTT_BEST_TIME), 4))
                        finishTime = *(DWORD*)(btt + BTT_BEST_TIME);
                }
                log_fmt("GOAL! finishTime=%d", finishTime);

                if (finishTime != NO_TIME && btt && g_currentRaceName[0]) {
                    if (!IsBadReadPtr((void*)(btt + 8), 4)) {
                        DWORD count = *(DWORD*)(btt + 8);
                        if (!IsBadReadPtr((void*)(btt + 0x410), 4)) {
                            DWORD* data = *(DWORD**)(btt + 0x410);
                            if (count > 0 && count < 200000 && data &&
                                (DWORD)data > 0x10000 &&
                                !IsBadReadPtr(data, count * 4)) {
                                log_fmt("Reading %d frames from game recording", count);
                                snaps_reserve((int)count);
                                if (g_rawSnaps) {
                                    g_rawCount = 0;
                                    for (int i = 0; i < (int)count; i++) {
                                        DWORD* snap = (DWORD*)data[i];
                                        if (snap && (DWORD)snap > 0x10000 &&
                                            !IsBadReadPtr(snap, SNAP_BYTES)) {
                                            memcpy(g_rawSnaps[g_rawCount], snap, SNAP_BYTES);
                                            g_rawCount++;
                                        }
                                    }
                                    log_fmt("Read %d snapshots", g_rawCount);
                                }
                            }
                        }
                    }

                    if (g_rawCount > 0) {
                        save_ghost_for_race("Previous_Run", finishTime,
                            g_rawSnaps, g_rawCount);

                        int existingTime = get_saved_time(g_currentRaceName);
                        if (existingTime == NO_TIME) {
                            log_fmt("No existing ghost — saving");
                            save_ghost_for_race(g_currentRaceName, finishTime,
                                g_rawSnaps, g_rawCount);
                        }
                        else if (finishTime < existingTime) {
                            log_fmt("New time %d < saved %d — overwriting",
                                finishTime, existingTime);
                            save_ghost_for_race(g_currentRaceName, finishTime,
                                g_rawSnaps, g_rawCount);
                        }
                        else {
                            log_fmt("New time %d >= saved %d — discarding",
                                finishTime, existingTime);
                        }
                    }
                    else {
                        log_msg("0 snapshots — likely stale goal flag, resetting");
                        g_raceFinished = 0;
                    }
                }
            }
            g_prevGoalFlag = goalFlag;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Mod class
 * ═══════════════════════════════════════════════════════════════════════════ */

class GhostSaverMod : public HamsterballAPI {
public:
    const char* GetModName() override { return "Ghost Saver"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        g_api = modApi;

        CustomButton btn("GHOST_SAVER", "Save Ghosts");
        btn.defaultState = true;
        modApi->CreateToggleButton(btn, this);

        init_ghost_dir();

        modApi->RegisterCustomHook(ADDR_APP_START_PRACTICE,
            (void*)hook_AppStartPracticeRace,
            (void**)&orig_AppStartPracticeRace);

        log_msg("=== Ghost Saver Mod (HB+) Initialized ===");
    }

    void onButtonToggle(const char* buttonId, bool newState) override {
        if (strcmp(buttonId, "GHOST_SAVER") == 0) {
            g_enabled = newState;
        }
    }

    void onGameUpdate() override {
        if (!g_enabled || !g_api) return;
        check_race_state();
    }

    void onSceneEnd() override {
        g_recording = 0;
        g_raceFinished = 0;
        g_prevGoalFlag = 0;
        g_currentRaceName[0] = '\0';
        g_hookRaceName[0] = '\0';
    }

    void onLevelStart() override {
        DWORD app = get_app();
        if (app && !IsBadReadPtr((void*)(app + APP_5D6_GOAL_FLAG), 1))
            g_prevGoalFlag = *(BYTE*)(app + APP_5D6_GOAL_FLAG);
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new GhostSaverMod();
}
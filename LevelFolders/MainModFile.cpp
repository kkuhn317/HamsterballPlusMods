// plus_level_folders_MSVC.cpp — Level Folders (HB+ v2.1, MSVC) v01x
//
// Per-level music folders named like the base MESHWORLD file:
//   Levels/Level1/music.wav   (Warm-Up = Level1, Beginner = LevelCascade, ...)
// File priority inside a folder: music.wav > music.ogg > music.mp3 > music.mo3.
// Missing folder/file = vanilla Music.mo3 plays untouched. Use .wav: it plays
// through the mod's own waveOut output (own RIFF parse, per-song volume), so
// it works everywhere. .ogg/.mp3 go through MCI (needs a working driver);
// .mo3 through BASS_MusicLoad/PlayEx.
//
// How it works:
//   Audio_PlayMusic (0x46A310) looks up the track NAME and plays it.
//   Audio_PlayMusicAtSpeed (0x46A440) calls it internally, so one hook covers
//   normal + speed variants + retries.
//   Entry bytes at 0x46A310: 53 55 8B E9 56 (5 bytes) -> JMP cave.
//   The cave calls music_play_handler(channelObj, trackName):
//     armed level file exists -> stop game handle, play ours, skip original.
//     Else run original.
//   The race song fires before onLevelStart, so the handler also replays the
//   saved song at level start (same-board check). Other songs (goal/menu)
//   release the slot and play vanilla.
//   Volume = BASS_CONFIG_GVOL_MUSIC x channel fade, mirrored to our output,
//   so custom music follows the game's music slider + fade-in like vanilla.
//   BASS is reached via the loader's own exports (GetModuleHandleA("bass.dll")).
//   Logging is console-only (ShowConsole=1); no log file is ever created.
//
// Build (Visual Studio, 32-bit — the game is x86):
//   New Project -> Dynamic-Link Library (DLL), add this file + HamsterballAPI.h,
//   platform Win32/x86, Release. Copy the built DLL into the game's Mods\
//   folder under its own name (e.g. plus_level_folders.dll). NEVER rename it
//   to bass.dll — the game root bass.dll must stay the HB+ proxy.

#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PLF_VERSION "v01x"
#define PLF_TOGGLE_ID "PLF_ENABLED"

#define PLAY_MUSIC_ADDR 0x46A310
#define PLAY_MUSIC_CONT 0x46A315
static const unsigned char kExpBytes[5] = { 0x53, 0x55, 0x8B, 0xE9, 0x56 };

// BASS function pointers (resolved from the loader's bass.dll at runtime)
typedef DWORD(__stdcall* BASS_MusicLoad_t)(int, void*, DWORD, DWORD, DWORD, DWORD);
typedef int(__stdcall* BASS_MusicPlayEx_t)(DWORD, DWORD, int, DWORD);
// Streams come in two ABIs (EAT-verified): pre-2.3 5-param @20 with DWORD
// offset/length, 2.3+ 7-param @28 with QWORD offset/length. Calling the
// wrong one corrupts the stack, so detect via the decorated exports.
typedef DWORD(__stdcall* BASS_StreamCreateFile20_t)(int, void*, DWORD, DWORD, DWORD);
typedef DWORD(__stdcall* BASS_StreamCreateFile28_t)(int, void*, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef int(__stdcall* BASS_ChannelPlay_t)(DWORD, int);
typedef int(__stdcall* BASS_ChannelResume_t)(DWORD);
// System MCI (winmm) for mp3/ogg: era BASS builds have no usable stream
// play fn, and their stream calls proved crash-prone. Resolved dynamically
// so no link dependency is added.
typedef DWORD(__stdcall* MCI_SendString_t)(const char*, char*, unsigned, void*);
static MCI_SendString_t pMciSend = NULL;
static int mci_open = 0;
// mciGetErrorStringA: turns numeric MCI errors into readable text.
typedef int(__stdcall* MCI_GetErrStr_t)(unsigned, char*, unsigned);
static MCI_GetErrStr_t pMciErrStr = NULL;
// PlaySoundA (winmm): last-resort .wav loop (no volume control).
typedef int(__stdcall* SND_PlaySound_t)(const char*, void*, unsigned);
static SND_PlaySound_t pPlaySound = NULL;
static int snd_open = 0;
// BASS_ChannelGetAttributes: read the game's music volume so custom
// output can mirror it.
typedef int(__stdcall* BASS_ChannelGetAttr_t)(DWORD, DWORD*, DWORD*, int*);
static BASS_ChannelGetAttr_t pChanGetAttr = NULL;
typedef DWORD(__stdcall* BASS_GetVol_t)(void);
static BASS_GetVol_t pGetVol = NULL;
typedef DWORD(__stdcall* BASS_GetConfig_t)(DWORD);
static BASS_GetConfig_t pGetConfig = NULL;
typedef DWORD(__stdcall* BASS_ChanGetDev_t)(DWORD);
typedef int(__stdcall* BASS_SetDev_t)(DWORD);
typedef DWORD(__stdcall* BASS_GetDev_t)(void);
static BASS_ChanGetDev_t pChanGetDev = NULL;
static BASS_SetDev_t pSetDev = NULL;
static BASS_GetDev_t pGetDev = NULL;
static int g_musicDev = -1;
// Volume model: global BASS volume (user's music setting) x channel fade.
// Matches vanilla music behavior exactly.
static int g_volG = 100, g_volC = 100, g_volBoth = 100;
static int g_volLogged = 0;
typedef int(__stdcall* BASS_ChannelStop_t)(DWORD);
typedef int(__stdcall* BASS_MusicFree_t)(DWORD);
typedef int(__stdcall* BASS_StreamFree_t)(DWORD);
typedef int(__stdcall* BASS_ChannelSetAttr_t)(DWORD, DWORD, int, int);
typedef int(__stdcall* BASS_ErrorGetCode_t)(void);

static BASS_MusicLoad_t        pMusicLoad = NULL;
static BASS_MusicPlayEx_t      pMusicPlayEx = NULL;
static BASS_StreamCreateFile20_t pStreamCreate20 = NULL;
static BASS_StreamCreateFile28_t pStreamCreate28 = NULL;
static BASS_ChannelPlay_t      pChannelPlay = NULL;
static BASS_ChannelResume_t    pChannelResume = NULL;
static BASS_ChannelStop_t      pChannelStop = NULL;
static BASS_MusicFree_t        pMusicFree = NULL;
static BASS_StreamFree_t       pStreamFree = NULL;
static BASS_ChannelSetAttr_t   pChanAttr = NULL;
static BASS_ErrorGetCode_t     pErrorGetCode = NULL;

// Board (Scene) name -> Levels/<Base> folder. Bases match RaceFiles.txt
// (Race 1=level1 ... 15=levelimpossible) and LevelData.txt MeshPath.
struct BoardMap { const char* board; const char* folder; };
static const BoardMap kBoards[] = {
    { "Board (Warm-Up)",     "Level1" },
    { "Board (Beginner)",    "LevelCascade" },
    { "Board (Intermediate)","Level2" },
    { "Board (Dizzy)",       "Level3" },
    { "Board (Tower)",       "Level4" },
    { "Board (Up)",          "LevelUp" },
    { "Board (Dark)",        "LevelDark" },
    { "Board (Expert)",      "Level5" },
    { "Board (Odd)",         "Level6" },
    { "Board (Toob)",        "Level8" },
    { "Board (Wobbly)",      "Level7" },
    { "Board (Glass)",       "LevelGlass" },
    { "Board (Sky)",         "Level9" },
    { "Board (Master)",      "Level10" },
    { "Board (Impossible)",  "LevelImpossible" },
};

// Armed state for the current level. kind: 0 = none, 1 = stream, 2 = mo3.
static char  g_exeDir[MAX_PATH] = "";
static char  g_armedPath[MAX_PATH] = "";
static char  g_armedFolder[64] = "";
static int   g_armedKind = 0;
static char  g_armedBoard[64] = "";
static char  g_armedSong[64] = "";
static DWORD g_customMusic = 0;   // active custom handle, 0 = none
static int   g_customKind = 0;    // kind of g_customMusic (1/2)
static bool  g_hookInstalled = false;
static BYTE* g_cave = NULL;

static void plf_log(const char* msg) {
    DWORD written = 0;
    // Console only: no log file is ever created.
    HANDLE hc = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!hc || hc == INVALID_HANDLE_VALUE) return;
    if (!WriteFile(hc, msg, (DWORD)strlen(msg), &written, NULL)) return;
    WriteFile(hc, "\r\n", 2, &written, NULL);
}

static void split_dir(char* path) {
    char* last = NULL, * p = path;
    while (*p) { if (*p == '\\' || *p == '/') last = p; p++; }
    if (last) *(last + 1) = '\0';
}

static int file_exists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

static void build_log_path(void) {
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        strncpy(g_exeDir, exePath, MAX_PATH - 1);
        split_dir(g_exeDir);
    }
}

static const char* board_to_folder(const char* board) {
    size_t i;
    if (!board) return NULL;
    for (i = 0; i < sizeof(kBoards) / sizeof(kBoards[0]); i++)
        if (strcmp(board, kBoards[i].board) == 0) return kBoards[i].folder;
    return NULL;
}

// Probe Levels/<folder>/ for music.wav/ogg/mp3/mo3. Returns kind 0/4/1/2.
// music.wav first: own waveOut output (volume-aware), then MCI, then BASS.
static int probe_folder(const char* folder, char* outPath) {
    static const char* files[] = { "music.wav", "music.ogg", "music.mp3", "music.mo3" };
    static const int kinds[] = { 4, 1, 1, 2 };
    int i;
    for (i = 0; i < 4; i++) {
        snprintf(outPath, MAX_PATH, "%sLevels\\%s\\%s", g_exeDir, folder, files[i]);
        if (file_exists(outPath)) return kinds[i];
    }
    return 0;
}

// ---- waveOut WAV player: own RIFF parse, per-handle volume ----
struct WV_WFX { WORD tag, ch; DWORD rate, avg; WORD align, bits, extra; };
struct WV_HDR { char* data; DWORD len, rec, user, flags, loops; void* next; DWORD rsv; };
typedef void(__stdcall* WV_CB_t)(void*, unsigned, DWORD, DWORD, DWORD);
typedef int(__stdcall* WO_Open_t)(void**, unsigned, void*, DWORD, DWORD, DWORD);
typedef int(__stdcall* WO_PH_t)(void*, void*, unsigned);
typedef int(__stdcall* WO_Write_t)(void*, void*, unsigned);
typedef int(__stdcall* WO_Reset_t)(void*);
typedef int(__stdcall* WO_Close_t)(void*);
typedef int(__stdcall* WO_Vol_t)(void*, DWORD);
static HMODULE g_winmm = NULL;
static WO_Open_t pWO_Open = NULL;
static WO_PH_t pWO_Prep = NULL, pWO_Unprep = NULL;
static WO_Write_t pWO_Write = NULL;
static WO_Reset_t pWO_Reset = NULL;
static WO_Close_t pWO_Close = NULL;
static WO_Vol_t pWO_Vol = NULL;
static void* wv_h = NULL;
static WV_HDR wv_hdr;
static char* wv_buf = NULL;
static int wv_open = 0;
#define WOM_DONE_MSG 0x3BD
#define WV_CALLBACK_FN 0x30000
#define WV_MAPPER 0xFFFFFFFFu

static void wave_vol() {
    int comb;
    DWORD wv;
    unsigned err;
    char buf[128];
    if (!wv_open || !pWO_Vol || !wv_h) return;
    comb = (g_volG * g_volC + 50) / 100;
    if (comb < 0) comb = 0;
    if (comb > 100) comb = 100;
    wv = (DWORD)comb * 65535u / 100u;
    err = (unsigned)pWO_Vol(wv_h, wv | (wv << 16));
    if (err) {
        snprintf(buf, sizeof(buf), "WAV VOLFAIL err=%u", err);
        plf_log(buf);
    }
}

static void __stdcall wv_cb(void* h, unsigned msg, DWORD i0, DWORD i1, DWORD i2) {
    (void)i0; (void)i1; (void)i2;
    if (msg == WOM_DONE_MSG && wv_open && pWO_Write)
        pWO_Write(h ? h : wv_h, &wv_hdr, 32);
}

static void wv_resolve() {
    if (!g_winmm) {
        g_winmm = GetModuleHandleA("winmm.dll");
        if (!g_winmm) g_winmm = LoadLibraryA("winmm.dll");
    }
    if (!g_winmm) return;
    if (!pWO_Open) pWO_Open = (WO_Open_t)GetProcAddress(g_winmm, "waveOutOpen");
    if (!pWO_Prep) pWO_Prep = (WO_PH_t)GetProcAddress(g_winmm, "waveOutPrepareHeader");
    if (!pWO_Unprep) pWO_Unprep = (WO_PH_t)GetProcAddress(g_winmm, "waveOutUnprepareHeader");
    if (!pWO_Write) pWO_Write = (WO_Write_t)GetProcAddress(g_winmm, "waveOutWrite");
    if (!pWO_Reset) pWO_Reset = (WO_Reset_t)GetProcAddress(g_winmm, "waveOutReset");
    if (!pWO_Close) pWO_Close = (WO_Close_t)GetProcAddress(g_winmm, "waveOutClose");
    if (!pWO_Vol) pWO_Vol = (WO_Vol_t)GetProcAddress(g_winmm, "waveOutSetVolume");
}

static void wave_stop() {
    if (!wv_open) return;
    wv_open = 0;
    if (pWO_Reset && wv_h) pWO_Reset(wv_h);
    if (pWO_Unprep && wv_h) pWO_Unprep(wv_h, &wv_hdr, 32);
    if (pWO_Close && wv_h) pWO_Close(wv_h);
    wv_h = NULL;
    if (wv_buf) { free(wv_buf); wv_buf = NULL; }
}

static DWORD wv_get32(const char* p) {
    return (DWORD)((unsigned char)p[0] | ((unsigned char)p[1] << 8) |
        ((unsigned char)p[2] << 16) | ((unsigned char)p[3] << 24));
}

static WORD wv_get16(const char* p) {
    return (WORD)((unsigned char)p[0] | ((unsigned char)p[1] << 8));
}

static int wave_play(const char* path) {
    char* b = NULL, buf[128];
    DWORD len = 0, off, dataOff = 0, dataLen = 0;
    WORD tag = 0, ch = 0, bits = 0;
    DWORD rate = 0;
    WV_WFX wfx;
    int i, rc;
    HANDLE h;
    DWORD sz, rd = 0;
    wave_stop();
    wv_resolve();
    if (!pWO_Open || !pWO_Prep || !pWO_Write || !pWO_Reset || !pWO_Unprep || !pWO_Close) {
        plf_log("WAV no waveout");
        return 0;
    }
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { plf_log("WAV FAIL read"); return 0; }
    sz = GetFileSize(h, NULL);
    if (sz == 0xFFFFFFFF || sz < 44 || sz > 67108864) { CloseHandle(h); plf_log("WAV FAIL size"); return 0; }
    b = (char*)malloc(sz);
    if (!b) { CloseHandle(h); return 0; }
    if (!ReadFile(h, b, sz, &rd, NULL) || rd != sz) { free(b); CloseHandle(h); plf_log("WAV FAIL read"); return 0; }
    CloseHandle(h);
    len = sz;
    if (b[0] != 'R' || b[1] != 'I' || b[2] != 'F' || b[3] != 'F' ||
        b[8] != 'W' || b[9] != 'A' || b[10] != 'V' || b[11] != 'E') {
        free(b); plf_log("WAV FAIL riff"); return 0;
    }
    off = 12;
    while (off + 8 <= len) {
        DWORD cid = wv_get32(b + off), clen = wv_get32(b + off + 4);
        if (cid == 0x20746D66) {
            if (clen < 16 || off + 24 > len) break;
            tag = wv_get16(b + off + 8);
            ch = wv_get16(b + off + 10);
            rate = wv_get32(b + off + 12);
            bits = wv_get16(b + off + 22);
            if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2 ||
                rate < 8000 || rate > 192000) {
                free(b); plf_log("WAV FAIL fmt"); return 0;
            }
            wfx.tag = 1; wfx.ch = ch; wfx.rate = rate; wfx.bits = bits;
            wfx.align = (WORD)(ch * bits / 8);
            wfx.avg = rate * wfx.align;
            wfx.extra = 0;
        }
        else if (cid == 0x61746164) {
            dataOff = off + 8;
            dataLen = clen;
            if (dataOff + dataLen > len) dataLen = len - dataOff;
        }
        off += 8 + clen + (clen & 1);
        if (off >= len) break;
    }
    if (!tag || !dataOff || !dataLen) { free(b); plf_log("WAV FAIL chunks"); return 0; }
    for (i = 0; i < 8; i++) ((DWORD*)&wv_hdr)[i] = 0;
    wv_hdr.data = b + dataOff;
    wv_hdr.len = dataLen;
    rc = pWO_Open(&wv_h, WV_MAPPER, &wfx, (DWORD)wv_cb, 0, WV_CALLBACK_FN);
    if (rc) {
        free(b);
        snprintf(buf, sizeof(buf), "WAV FAIL open err=%d", rc);
        plf_log(buf);
        return 0;
    }
    if (pWO_Prep(wv_h, &wv_hdr, 32)) {
        pWO_Close(wv_h); wv_h = NULL; free(b); plf_log("WAV FAIL prep"); return 0;
    }
    wv_buf = b;
    wv_open = 1;
    rc = pWO_Write(wv_h, &wv_hdr, 32);
    if (rc) {
        wave_stop();
        snprintf(buf, sizeof(buf), "WAV FAIL write err=%d", rc);
        plf_log(buf);
        return 0;
    }
    wave_vol();
    snprintf(buf, sizeof(buf), "WAV play ch=%d rate=%d bits=%d",
        (int)wfx.ch, (int)wfx.rate, (int)wfx.bits);
    plf_log(buf);
    return 1;
}

static void yield_custom(const char* why) {
    char buf[256];
    if (!g_customMusic && !mci_open && !snd_open && !wv_open) return;
    if (g_customKind == 5) {
        wave_stop();
        g_customMusic = 0;
        g_customKind = 0;
        snprintf(buf, sizeof(buf), "YIELD %s (wav stopped)", why);
        plf_log(buf);
        return;
    }
    if (g_customKind == 4) {
        if (pPlaySound) pPlaySound(NULL, NULL, 0);
        snd_open = 0;
        g_customMusic = 0;
        g_customKind = 0;
        snprintf(buf, sizeof(buf), "YIELD %s (wav stopped)", why);
        plf_log(buf);
        return;
    }
    if (g_customKind == 3) {
        if (pMciSend) {
            pMciSend("stop PLFBGM", NULL, 0, NULL);
            pMciSend("close PLFBGM", NULL, 0, NULL);
        }
        mci_open = 0;
        g_customMusic = 0;
        g_customKind = 0;
        snprintf(buf, sizeof(buf), "YIELD %s (mci stopped)", why);
        plf_log(buf);
        return;
    }
    if (pChannelStop && g_customMusic) pChannelStop(g_customMusic);
    if (g_customKind == 2) { if (pMusicFree) pMusicFree(g_customMusic); }
    else { if (pStreamFree) pStreamFree(g_customMusic); }
    g_customMusic = 0;
    g_customKind = 0;
    snprintf(buf, sizeof(buf), "YIELD %s (custom stopped)", why);
    plf_log(buf);
}

// MCI playback for mp3/ogg via system winmm (no BASS stream calls:
// era BASS builds export Resume but not Play, and their stream ABI
// proved crash-prone).
static unsigned mci_cmd(const char* cmd) {
    if (!pMciSend) return 1;
    return (unsigned)pMciSend(cmd, NULL, 0, NULL);
}

// Human-readable MCI error text (static buffer, log immediately).
static const char* mci_err_text(unsigned err) {
    static char etxt[128];
    int i;
    if (!pMciErrStr) return "no-text";
    for (i = 0; i < 127; i++) etxt[i] = 0;
    if (!pMciErrStr(err, etxt, 127)) return "no-text";
    etxt[127] = '\0';
    if (!etxt[0]) return "empty-text";
    return etxt;
}

// Loop state: waveaudio has no repeat flag, so onGameUpdate polls
// "status PLFBGM mode" and re-plays from 0 when it stops.
static int mci_loop = 0;
static int mci_frames = 0;

static void mci_yield() {
    if (!mci_open) return;
    mci_cmd("stop PLFBGM");
    mci_cmd("close PLFBGM");
    mci_open = 0;
    mci_loop = 0;
    plf_log("MCI stopped");
}

static int mci_try_open(const char* path, const char* type) {
    char cmd[MAX_PATH + 64], buf[256];
    unsigned err;
    snprintf(cmd, sizeof(cmd), "open \"%s\" type %s alias PLFBGM", path, type);
    plf_log("MCI open attempt...");
    err = pMciSend ? (unsigned)pMciSend(cmd, NULL, 0, NULL) : 1;
    if (err) {
        snprintf(buf, sizeof(buf), "MCI FAIL open-%s err=%u (%s)",
            type, err, mci_err_text(err));
        plf_log(buf);
    }
    else {
        plf_log("MCI open ok");
    }
    return err ? 0 : 1;
}

// Mirror the game's music volume onto open MCI output (MCI 0-1000).
static void mci_apply_vol() {
    char cmd[64], buf[128];
    unsigned err;
    int v;
    if (!mci_open) return;
    v = g_volBoth * 10;
    if (v < 0) v = 0;
    if (v > 1000) v = 1000;
    snprintf(cmd, sizeof(cmd), "setaudio PLFBGM volume to %d", v);
    err = mci_cmd(cmd);
    if (err) {
        snprintf(buf, sizeof(buf), "MCI VOLFAIL err=%u (%s)",
            err, mci_err_text(err));
        plf_log(buf);
    }
}

// Combined volume (global x channel): refresh caches, push to outputs.
// The global is read on the game's own BASS device: threads default to a
// different device whose volume is always 100 (that cost us v01r).
static void sync_vol_all(DWORD ch) {
    DWORD gv = 100, cv = 100, freq = 0;
    int pan = 0;
    char buf[64];
    if (pChanGetAttr && ch) {
        if (pChanGetAttr(ch, &freq, &cv, &pan)) { if (cv > 100) cv = 100; }
        else if (!g_volLogged) { g_volLogged = 1; plf_log("VOL query failed"); }
        if (pChanGetDev) {
            DWORD d = pChanGetDev(ch);
            if (d >= 1 && d <= 8 && (int)d != g_musicDev) {
                g_musicDev = (int)d;
                snprintf(buf, sizeof(buf), "VOLDEV %d", g_musicDev);
                plf_log(buf);
            }
        }
    }
    // Music setting = BASS_CONFIG_GVOL_MUSIC (option 6): the game writes
    // the slider there via BASS_SetConfig (binary-verified). Device-global
    // volume is always 100; only a fallback.
    if (pGetConfig) {
        gv = pGetConfig(6);
        if (gv > 100) gv = 100;
    }
    else if (pGetVol) {
        if (pSetDev && pGetDev && g_musicDev >= 1) {
            DWORD back = pGetDev();
            pSetDev((DWORD)g_musicDev);
            gv = pGetVol();
            pSetDev(back);
        }
        else {
            gv = pGetVol();
        }
        if (gv > 100) gv = 100;
    }
    if ((int)gv == g_volG && (int)cv == g_volC) return;
    {
        int pg = g_volG;
        g_volG = (int)gv;
        g_volC = (int)cv;
        g_volBoth = (g_volG * g_volC + 50) / 100;
        if (g_volG != pg) {
            snprintf(buf, sizeof(buf), "VOLG %d", g_volG);
            plf_log(buf);
        }
    }
    mci_apply_vol();
    wave_vol();
}

// PlaySound .wav loop: SND_FILENAME|SND_ASYNC|SND_LOOP. The game handle
// is stopped first so outputs never mix. Stop = PlaySound(NULL).
static int snd_play(const char* path) {
    int rc;
    if (pPlaySound) pPlaySound(NULL, NULL, 0);
    snd_open = 0;
    if (!pPlaySound) return 0;
    rc = pPlaySound(path, NULL, 0x20001 | 0x8 | 0x2);
    if (!rc) {
        plf_log("SND FAIL playsound");
        return 0;
    }
    snd_open = 1;
    return 1;
}

static int mci_play(const char* path, const char* type) {
    char buf[128];
    unsigned err;
    int wave;
    mci_yield();
    // Single explicit-type attempt only: retrying through another device
    // re-enters a broken driver and crashed v01k.
    if (!mci_try_open(path, type)) return 0;
    mci_apply_vol();
    mci_loop = 0;
    wave = (type[0] == 'w');
    if (wave) {
        // waveaudio rejects "repeat": plain play + poll-loop restart.
        err = mci_cmd("play PLFBGM");
        if (!err) {
            mci_loop = 1;
            plf_log("MCI loop via poll");
        }
    }
    else {
        err = mci_cmd("play PLFBGM repeat");
        if (err) {
            snprintf(buf, sizeof(buf), "MCI FAIL play-repeat err=%u (%s)",
                err, mci_err_text(err));
            plf_log(buf);
            err = mci_cmd("play PLFBGM");
            if (!err) {
                mci_loop = 1;
                plf_log("MCI loop via poll");
            }
        }
    }
    if (err) {
        snprintf(buf, sizeof(buf), "MCI FAIL play err=%u (%s)",
            err, mci_err_text(err));
        plf_log(buf);
        mci_cmd("close PLFBGM");
        return 0;
    }
    mci_open = 1;
    return 1;
}

// Per-frame poll (called from onGameUpdate): restart looped MCI output
// that reached its end. Throttled to every 4th frame.
static void mci_poll() {
    char rs[32];
    int i;
    if (!mci_open || !mci_loop || !pMciSend) return;
    if (++mci_frames < 4) return;
    mci_frames = 0;
    for (i = 0; i < 31; i++) rs[i] = 0;
    if (pMciSend("status PLFBGM mode", rs, 31, NULL)) return;
    if (rs[0] == 's' && rs[1] == 't' && rs[2] == 'o')
        mci_cmd("play PLFBGM from 0");
}

static int g_volFrames = 0;
static void sync_vol_all(DWORD ch);
static DWORD g_lastGameH;

// Periodic re-read of the game music volume: songs start at 0 and fade
// up, and settings can change mid-song. Only while custom is active.
static void vol_poll() {
    if (!mci_open && !g_customMusic && !wv_open) return;
    if (!pGetVol && !pChanGetAttr) return;
    if (++g_volFrames < 30) return;
    g_volFrames = 0;
    sync_vol_all(g_lastGameH);
}

// Last PlayMusic triple + board, for late-arm recovery. Recovery replays
// only when the song fired for the board being started.
static DWORD g_lastChanObj = 0;
static char g_lastTrack[64] = "";
static char g_lastBoard[64] = "";

// Handler called from the cave as handler(channelObj, trackName).
// Returns 1 if custom music started (skip original), 0 to run original.
static int __cdecl music_play_handler(DWORD channelObj, const char* trackName);

class LevelFoldersMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
public:
    const char* GetModName() override { return "Level Folders"; }
    const char* GetAuthorName() override { return "BookwormKevin"; }
    const char* GetContributors() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    bool enabled() {
        if (!api) return true;
        return api->GetButtonState(PLF_TOGGLE_ID);
    }

    void resolve_bass() {
        HMODULE hBass = GetModuleHandleA("bass.dll");
        HMODULE hReal = NULL;
        if (!hBass) hBass = GetModuleHandleA("BASS.dll");
        // The HB+ proxy only forwards a subset: stream fns often live
        // in bass_real.dll only. Try the proxy first, then bass_real.
        hReal = GetModuleHandleA("bass_real.dll");
        if (!hReal) hReal = GetModuleHandleA("BASS_REAL.dll");
        if (!hReal) hReal = LoadLibraryA("bass_real.dll");
#define RESOLVE(var, name) \
        if (!var) var = (decltype(var))GetProcAddress(hBass ? hBass : NULL, name); \
        if (!var && hReal) var = (decltype(var))GetProcAddress(hReal, name)
        RESOLVE(pMusicLoad, "BASS_MusicLoad");
        RESOLVE(pMusicPlayEx, "BASS_MusicPlayEx");
        // Stream ABI: decorated exports disambiguate (@20 old, @28 new).
        // Plain-name-only builds: old family if ChannelSetAttributes@16
        // marker exists, else modern @28.
        if (!pStreamCreate20 && !pStreamCreate28) {
            if (hBass) pStreamCreate20 =
                (BASS_StreamCreateFile20_t)GetProcAddress(hBass, "_BASS_StreamCreateFile@20");
            if (hReal && !pStreamCreate20) pStreamCreate20 =
                (BASS_StreamCreateFile20_t)GetProcAddress(hReal, "_BASS_StreamCreateFile@20");
            if (hBass) pStreamCreate28 =
                (BASS_StreamCreateFile28_t)GetProcAddress(hBass, "_BASS_StreamCreateFile@28");
            if (hReal && !pStreamCreate28) pStreamCreate28 =
                (BASS_StreamCreateFile28_t)GetProcAddress(hReal, "_BASS_StreamCreateFile@28");
        }
        if (!pStreamCreate20 && !pStreamCreate28) {
            FARPROC plain = hBass ? GetProcAddress(hBass, "BASS_StreamCreateFile") : NULL;
            if (!plain && hReal) plain = GetProcAddress(hReal, "BASS_StreamCreateFile");
            if (plain) {
                FARPROC marker = hBass ? GetProcAddress(hBass, "_BASS_ChannelSetAttributes@16") : NULL;
                if (!marker && hReal) marker = GetProcAddress(hReal, "_BASS_ChannelSetAttributes@16");
                if (marker) pStreamCreate20 = (BASS_StreamCreateFile20_t)plain;
                else pStreamCreate28 = (BASS_StreamCreateFile28_t)plain;
            }
        }
        RESOLVE(pChannelPlay, "BASS_ChannelPlay");
        if (!pChannelResume) {
            if (hBass) pChannelResume =
                (BASS_ChannelResume_t)GetProcAddress(hBass, "BASS_ChannelResume");
            if (!pChannelResume && hReal) pChannelResume =
                (BASS_ChannelResume_t)GetProcAddress(hReal, "BASS_ChannelResume");
            if (!pChannelResume && hReal) pChannelResume =
                (BASS_ChannelResume_t)GetProcAddress(hReal, "_BASS_ChannelResume@4");
            if (!pChannelResume && hBass) pChannelResume =
                (BASS_ChannelResume_t)GetProcAddress(hBass, "_BASS_ChannelResume@4");
        }
        RESOLVE(pChannelStop, "BASS_ChannelStop");
        RESOLVE(pMusicFree, "BASS_MusicFree");
        RESOLVE(pStreamFree, "BASS_StreamFree");
        RESOLVE(pChanAttr, "BASS_ChannelSetAttributes");
        RESOLVE(pChanGetAttr, "BASS_ChannelGetAttributes");
        RESOLVE(pErrorGetCode, "BASS_ErrorGetCode");
#undef RESOLVE
        if (!pChanGetAttr) {
            if (hReal) pChanGetAttr =
                (BASS_ChannelGetAttr_t)GetProcAddress(hReal, "_BASS_ChannelGetAttributes@16");
            if (!pChanGetAttr && hBass) pChanGetAttr =
                (BASS_ChannelGetAttr_t)GetProcAddress(hBass, "_BASS_ChannelGetAttributes@16");
        }
        if (!pGetVol) {
            if (hBass) pGetVol =
                (BASS_GetVol_t)GetProcAddress(hBass, "BASS_GetVolume");
            if (!pGetVol && hReal) pGetVol =
                (BASS_GetVol_t)GetProcAddress(hReal, "BASS_GetVolume");
            if (!pGetVol && hReal) pGetVol =
                (BASS_GetVol_t)GetProcAddress(hReal, "_BASS_GetVolume@0");
            if (!pGetVol && hBass) pGetVol =
                (BASS_GetVol_t)GetProcAddress(hBass, "_BASS_GetVolume@0");
        }
        if (!pGetConfig) {
            if (hBass) pGetConfig =
                (BASS_GetConfig_t)GetProcAddress(hBass, "BASS_GetConfig");
            if (!pGetConfig && hReal) pGetConfig =
                (BASS_GetConfig_t)GetProcAddress(hReal, "BASS_GetConfig");
            if (!pGetConfig && hReal) pGetConfig =
                (BASS_GetConfig_t)GetProcAddress(hReal, "_BASS_GetConfig@4");
            if (!pGetConfig && hBass) pGetConfig =
                (BASS_GetConfig_t)GetProcAddress(hBass, "_BASS_GetConfig@4");
        }
        if (!pChanGetDev) {
            if (hBass) pChanGetDev =
                (BASS_ChanGetDev_t)GetProcAddress(hBass, "BASS_ChannelGetDevice");
            if (!pChanGetDev && hReal) pChanGetDev =
                (BASS_ChanGetDev_t)GetProcAddress(hReal, "BASS_ChannelGetDevice");
            if (!pChanGetDev && hReal) pChanGetDev =
                (BASS_ChanGetDev_t)GetProcAddress(hReal, "_BASS_ChannelGetDevice@4");
            if (!pChanGetDev && hBass) pChanGetDev =
                (BASS_ChanGetDev_t)GetProcAddress(hBass, "_BASS_ChannelGetDevice@4");
        }
        if (!pSetDev) {
            if (hBass) pSetDev =
                (BASS_SetDev_t)GetProcAddress(hBass, "BASS_SetDevice");
            if (!pSetDev && hReal) pSetDev =
                (BASS_SetDev_t)GetProcAddress(hReal, "BASS_SetDevice");
            if (!pSetDev && hReal) pSetDev =
                (BASS_SetDev_t)GetProcAddress(hReal, "_BASS_SetDevice@4");
            if (!pSetDev && hBass) pSetDev =
                (BASS_SetDev_t)GetProcAddress(hBass, "_BASS_SetDevice@4");
        }
        if (!pGetDev) {
            if (hBass) pGetDev =
                (BASS_GetDev_t)GetProcAddress(hBass, "BASS_GetDevice");
            if (!pGetDev && hReal) pGetDev =
                (BASS_GetDev_t)GetProcAddress(hReal, "BASS_GetDevice");
            if (!pGetDev && hReal) pGetDev =
                (BASS_GetDev_t)GetProcAddress(hReal, "_BASS_GetDevice@0");
            if (!pGetDev && hBass) pGetDev =
                (BASS_GetDev_t)GetProcAddress(hBass, "_BASS_GetDevice@0");
        }
        // winmm for MCI mp3/ogg (dynamic, no link dependency).
        if (!pMusicLoad) pMusicLoad = (BASS_MusicLoad_t)(hReal ? GetProcAddress(hReal, "_BASS_MusicLoad@24") : NULL);
        if (!pMusicLoad && hBass) pMusicLoad = (BASS_MusicLoad_t)GetProcAddress(hBass, "_BASS_MusicLoad@24");
        if (!pMusicPlayEx) pMusicPlayEx = (BASS_MusicPlayEx_t)(hReal ? GetProcAddress(hReal, "_BASS_MusicPlayEx@16") : NULL);
        if (!pMusicPlayEx && hBass) pMusicPlayEx = (BASS_MusicPlayEx_t)GetProcAddress(hBass, "_BASS_MusicPlayEx@16");
        if (!pChannelPlay && hReal) pChannelPlay = (BASS_ChannelPlay_t)GetProcAddress(hReal, "_BASS_ChannelPlay@8");
        if (!pChannelPlay && hBass) pChannelPlay = (BASS_ChannelPlay_t)GetProcAddress(hBass, "_BASS_ChannelPlay@8");
        if (!pChannelStop && hReal) pChannelStop = (BASS_ChannelStop_t)GetProcAddress(hReal, "_BASS_ChannelStop@4");
        if (!pStreamFree && hReal) pStreamFree = (BASS_StreamFree_t)GetProcAddress(hReal, "_BASS_StreamFree@4");
    }

    void install_hook();
    const char* current_board();

    bool streams_ready() {
        return (pStreamCreate20 || pStreamCreate28) &&
            (pChannelPlay || pChannelResume);
    }
    void log_bass_state(const char* when) {
        char buf[160];
        HMODULE hB = GetModuleHandleA("bass.dll");
        HMODULE hR = GetModuleHandleA("bass_real.dll");
        snprintf(buf, sizeof(buf),
            "BASS [%s] proxy=%d real=%d music=%d playex=%d stream20=%d stream28=%d chplay=%d resume=%d stop=%d",
            when, hB ? 1 : 0, hR ? 1 : 0,
            pMusicLoad ? 1 : 0, pMusicPlayEx ? 1 : 0,
            pStreamCreate20 ? 1 : 0, pStreamCreate28 ? 1 : 0,
            pChannelPlay ? 1 : 0, pChannelResume ? 1 : 0, pChannelStop ? 1 : 0);
        plf_log(buf);
    }

    void Initialize(IModAPI* modApi) override {
        char buf[256];
        api = modApi;
        build_log_path();
        snprintf(buf, sizeof(buf), "=== Level Folders %s (HB+) Started ===", PLF_VERSION);
        plf_log(buf);

        CustomButton btn(PLF_TOGGLE_ID, "Level Folders Music");
        btn.defaultState = true;
        btn.trueText = "ON";
        btn.falseText = "OFF";
        api->CreateToggleButton(btn, this);

        resolve_bass();
        if (!pMciSend) {
            HMODULE hw = GetModuleHandleA("winmm.dll");
            if (!hw) hw = LoadLibraryA("winmm.dll");
            if (hw) {
                pMciSend =
                    (MCI_SendString_t)GetProcAddress(hw, "mciSendStringA");
                pMciErrStr =
                    (MCI_GetErrStr_t)GetProcAddress(hw, "mciGetErrorStringA");
                pPlaySound =
                    (SND_PlaySound_t)GetProcAddress(hw, "PlaySoundA");
            }
        }
        log_bass_state("init");
        if (pMciSend) plf_log("MCI ready (mp3/ogg on)");
        else plf_log("MCI missing, mp3/ogg disabled");
        if (pPlaySound) plf_log("SND ready (wav on)");
        else plf_log("SND missing, wav disabled");
        plf_log(pChanGetAttr ? "VOLREAD on" : "VOLREAD off");
        plf_log(pGetVol ? "VOLGREAD on" : "VOLGREAD off");
        plf_log((pChanGetDev && pSetDev && pGetDev) ? "VOLDEVREAD on" : "VOLDEVREAD off");
        if (!pMusicLoad || !pMusicPlayEx) plf_log("BASS music fns missing, mo3 disabled");
        if ((!pMusicLoad || !pMusicPlayEx) && !pMciSend)
            plf_log("no playback fns, mod inert");
        install_hook();
    }

    void onLevelStart() override {
        plf_log("onLevelStart fired");
        const char* board = current_board();
        const char* folder;
        char buf[256], path[MAX_PATH];
        int kind;
        { // bass_real may map after our Initialize — retry once per level.
            bool had = streams_ready();
            resolve_bass();
            if (!had && streams_ready()) log_bass_state("level");
        }
        if (!board) { plf_log("LEVEL start (no scene yet)"); return; }
        folder = board_to_folder(board);
        if (!folder) {
            snprintf(buf, sizeof(buf), "LEVEL [%s] unmapped (custom level?), vanilla music", board);
            plf_log(buf);
            g_armedKind = 0;
            g_armedPath[0] = '\0';
            g_armedFolder[0] = '\0';
            g_armedBoard[0] = '\0';
            g_armedSong[0] = '\0';
            return;
        }
        kind = probe_folder(folder, path);
        if (kind == 0) {
            snprintf(buf, sizeof(buf), "LEVEL [%s] folder Levels\\%s\\, no music file, vanilla", board, folder);
            plf_log(buf);
            g_armedKind = 0;
            g_armedPath[0] = '\0';
            g_armedFolder[0] = '\0';
            g_armedBoard[0] = '\0';
            g_armedSong[0] = '\0';
        }
        else {
            strncpy(g_armedPath, path, MAX_PATH - 1);
            strncpy(g_armedFolder, folder, sizeof(g_armedFolder) - 1);
            strncpy(g_armedBoard, board, sizeof(g_armedBoard) - 1);
            // Candidate song: the most recent PlayMusic seeds the slot, so a
            // later jingle/menu song disarms instead of hijacking it. An empty
            // candidate falls back to first-played adoption (below).
            strncpy(g_armedSong, g_lastTrack, sizeof(g_armedSong) - 1);
            g_armedKind = kind;
            snprintf(buf, sizeof(buf), "LEVEL [%s] armed Levels\\%s\\ (%s)", board, folder,
                kind == 2 ? "mo3" : kind == 4 ? "wav" : "stream");
            plf_log(buf);
            snprintf(buf, sizeof(buf), "ARM path=%s", g_armedPath);
            plf_log(buf);
            // Race song fires before onLevelStart: replay the saved
            // PlayMusic now. Blocked only when it provably belongs to
            // another board; an unknown board means transition, i.e. the
            // incoming race's song.
            if (g_lastChanObj && g_lastTrack[0] &&
                (!g_lastBoard[0] || strcmp(g_lastBoard, board) == 0)) {
                plf_log("late-arm recovery");
                music_play_handler(g_lastChanObj, g_lastTrack);
            }
        }
    }

    void onSceneEnd() override {
        // Vanilla keeps Music.mo3 playing across light restarts (no PlayMusic
        // fires), so keep custom playing too. The next PlayMusic yields it.
        // Only the folder arming is cleared — menus fall back to vanilla.
        g_armedKind = 0;
        g_armedPath[0] = '\0';
        g_armedFolder[0] = '\0';
        g_armedBoard[0] = '\0';
        g_armedSong[0] = '\0';
        plf_log("SCENE end (arming cleared, custom keeps playing)");
    }

    void onGameUpdate() override { mci_poll(); vol_poll(); }

    void onButtonToggle(const char* buttonId, bool newState) override {
        char buf[128];
        if (!buttonId || strcmp(buttonId, PLF_TOGGLE_ID) != 0) return;
        snprintf(buf, sizeof(buf), "TOGGLE %s", newState ? "ON" : "OFF");
        plf_log(buf);
        if (!newState) yield_custom("toggle-off");
    }
};

static LevelFoldersMod* g_mod = NULL;

const char* LevelFoldersMod::current_board() {
    Scene* sc;
    const char* name;
    if (!api) return NULL;
    sc = api->GetScene();
    if (!sc) return NULL;
    if (IsBadReadPtr(sc, 4)) return NULL;
    // Scene.name is char* at +0x868 (binary-verified static_assert).
    name = *(const char**)((const char*)sc + 0x868);
    if (!name || IsBadReadPtr((void*)name, 8)) return NULL;
    return name;
}

static int __cdecl music_play_handler(DWORD channelObj, const char* trackName) {
    char name[64], buf[256];
    size_t n = 0;
    DWORD gameH = 0, nh = 0;

    if (!trackName || IsBadReadPtr((void*)trackName, 8)) return 0;
    if (channelObj < 0x10000 || IsBadReadPtr((void*)(channelObj + 8), 4)) return 0;
    for (; n < sizeof(name) - 1; n++) {
        char c;
        if (IsBadReadPtr((void*)(trackName + n), 1)) break;
        c = trackName[n];
        name[n] = c;
        if (!c) break;
    }
    name[n] = '\0';
    if (!name[0]) return 0;

    // Remember for late-arm recovery (race song fires before onLevelStart).
    {
        size_t k;
        const char* lb;
        g_lastChanObj = channelObj;
        g_lastGameH = *(DWORD*)(channelObj + 8);
        for (k = 0; k < sizeof(g_lastTrack) - 1 && name[k]; k++)
            g_lastTrack[k] = name[k];
        g_lastTrack[k] = '\0';
        lb = (g_mod ? g_mod->current_board() : NULL);
        if (lb) strncpy(g_lastBoard, lb, sizeof(g_lastBoard) - 1);
        else g_lastBoard[0] = '\0';
    }

    // Scene changed since arming (e.g. quit to menu): stale arming must
    // not hijack the new scene's music.
    if (g_armedKind && g_mod) {
        const char* cb = g_mod->current_board();
        if (!cb || !g_armedBoard[0] || strcmp(cb, g_armedBoard) != 0) {
            yield_custom(name);
            g_armedKind = 0;
            g_armedPath[0] = '\0';
            g_armedFolder[0] = '\0';
            g_armedBoard[0] = '\0';
            g_armedSong[0] = '\0';
            snprintf(buf, sizeof(buf), "DISARM (%s) scene changed -> original", name);
            plf_log(buf);
            return 0;
        }
        // Only the armed song is substituted: goal jingles, menu themes
        // and any other song play vanilla (and release the slot).
        if (g_armedSong[0] && strcmp(name, g_armedSong) != 0) {
            yield_custom(name);
            g_armedKind = 0;
            g_armedPath[0] = '\0';
            g_armedFolder[0] = '\0';
            g_armedBoard[0] = '\0';
            g_armedSong[0] = '\0';
            snprintf(buf, sizeof(buf), "DISARM (%s) other song -> original", name);
            plf_log(buf);
            return 0;
        }
    }

    // Disabled -> vanilla. Still yield any custom first (same as a switch).
    if (g_mod && !g_mod->enabled()) {
        yield_custom(name);
        snprintf(buf, sizeof(buf), "YIELD (%s) disabled -> original", name);
        plf_log(buf);
        return 0;
    }

    // Every new song yields the old custom first (vanilla always switches).
    if (g_customMusic) yield_custom(name);

    if (g_armedKind == 0 || !g_armedPath[0]) {
        snprintf(buf, sizeof(buf), "YIELD (%s) no file -> original", name);
        plf_log(buf);
        return 0;
    }
    if (!file_exists(g_armedPath)) {
        snprintf(buf, sizeof(buf), "YIELD (%s) file gone -> original", name);
        plf_log(buf);
        return 0;
    }

    gameH = *(DWORD*)(channelObj + 8); // HMUSIC of Music.mo3
    if (pChannelStop && gameH) pChannelStop(gameH);
    sync_vol_all(gameH);

    if (g_armedKind == 4) {
        // .wav: waveOut first (per-handle volume), then MCI, then SND.
        if (wave_play(g_armedPath)) {
            nh = 1;
            g_customKind = 5;
        }
        else if (mci_play(g_armedPath, "waveaudio")) {
            nh = 1;
            g_customKind = 3;
        }
        else if (snd_play(g_armedPath)) {
            nh = 1;
            g_customKind = 4;
        }
        else {
            snprintf(buf, sizeof(buf), "YIELD (%s) wav failed -> original", name);
            plf_log(buf);
            return 0;
        }
    }
    else if (g_armedKind == 2) {
        if (!pMusicLoad || !pMusicPlayEx) return 0;
        nh = pMusicLoad(0, g_armedPath, 0, 0, 4, 0);
        if (!nh) {
            snprintf(buf, sizeof(buf), "FAIL load (%s) mo3", name);
            plf_log(buf);
            return 0; // original restarts the game handle
        }
        if (pChanAttr) pChanAttr(nh, (DWORD)-1, 100, -101);
        if (!pMusicPlayEx(nh, 0, 1, 1)) {
            int err = pErrorGetCode ? pErrorGetCode() : -1;
            if (pMusicFree) pMusicFree(nh);
            snprintf(buf, sizeof(buf), "PLAY FAIL rc=0 err=%d (%s)", err, name);
            plf_log(buf);
            return 0; // original instead of silence
        }
        g_customKind = 2;
    }
    else {
        // mp3/ogg via system MCI (winmm): no BASS stream calls at all.
        // The game's own handle is stopped first so outputs never mix.
        if (!pMciSend) {
            snprintf(buf, sizeof(buf), "YIELD (%s) no mci -> original", name);
            plf_log(buf);
            return 0;
        }
        if (!mci_play(g_armedPath, "mpegvideo")) {
            snprintf(buf, sizeof(buf), "YIELD (%s) mci failed -> original", name);
            plf_log(buf);
            return 0;
        }
        nh = 1;
        g_customKind = 3;
    }
    g_customMusic = nh;
    // First substituted song defines the slot; others disarm (above).
    if (!g_armedSong[0]) strncpy(g_armedSong, name, sizeof(g_armedSong) - 1);
    snprintf(buf, sizeof(buf), "PLAY custom (%s) folder=%s via=%s", name,
        g_armedFolder[0] ? g_armedFolder : "?",
        g_customKind == 2 ? "mo3" : g_customKind == 3 ? "mci" :
        g_customKind == 5 ? "wav" : "snd");
    plf_log(buf);
    return 1;
}

void LevelFoldersMod::install_hook() {
    DWORD target = PLAY_MUSIC_ADDR, old_prot;
    BYTE* c;
    int pos = 0;
    if (g_hookInstalled && g_cave) {
        if (*(BYTE*)target == 0xE9 &&
            *(DWORD*)(target + 1) == (DWORD)g_cave - (target + 5))
            return; // already ours
    }
    if (IsBadReadPtr((void*)target, 5)) { plf_log("hook: exe not mapped, skip"); return; }
    if (memcmp((void*)target, kExpBytes, 5) != 0) {
        plf_log("hook: entry bytes mismatch, NOT patching");
        return;
    }
    c = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!c) { plf_log("hook: VirtualAlloc failed"); return; }
    g_cave = c;

    c[pos++] = 0x60;                                              // PUSHAD
    c[pos++] = 0x8B; c[pos++] = 0x44; c[pos++] = 0x24; c[pos++] = 0x24; // MOV EAX,[ESP+36] trackName
    c[pos++] = 0x50;                                              // PUSH EAX
    c[pos++] = 0x8B; c[pos++] = 0x44; c[pos++] = 0x24; c[pos++] = 0x1C; // MOV EAX,[ESP+28] channelObj
    c[pos++] = 0x50;                                              // PUSH EAX
    c[pos++] = 0xE8;                                              // CALL handler
    {
        DWORD rel = (DWORD)&music_play_handler - (DWORD)(c + pos + 4);
        memcpy(c + pos, &rel, 4); pos += 4;
    }
    c[pos++] = 0x83; c[pos++] = 0xC4; c[pos++] = 0x08;             // ADD ESP,8
    c[pos++] = 0x85; c[pos++] = 0xC0;                             // TEST EAX,EAX
    c[pos++] = 0x74; c[pos++] = 0x04;                             // JZ +4
    c[pos++] = 0x61;                                              // POPAD
    c[pos++] = 0xC2; c[pos++] = 0x08; c[pos++] = 0x00;             // RET 8 (skip original)
    c[pos++] = 0x61;                                              // POPAD
    c[pos++] = 0x53;                                              // PUSH EBX (stolen bytes)
    c[pos++] = 0x55;                                              // PUSH EBP
    c[pos++] = 0x8B; c[pos++] = 0xE9;                             // MOV EBP,ECX
    c[pos++] = 0x56;                                              // PUSH ESI
    c[pos++] = 0xE9;                                              // JMP PLAY_MUSIC_CONT
    {
        DWORD rel = (DWORD)PLAY_MUSIC_CONT - (DWORD)(c + pos + 4);
        memcpy(c + pos, &rel, 4); pos += 4;
    }

    VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old_prot);
    *(BYTE*)target = 0xE9;
    *(DWORD*)(target + 1) = (DWORD)c - (target + 5);
    VirtualProtect((void*)target, 5, old_prot, &old_prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);
    g_hookInstalled = true;
    plf_log("hook installed at Audio_PlayMusic");
}

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    if (!g_mod) g_mod = new LevelFoldersMod();
    return g_mod;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hMod);
    return TRUE;
}

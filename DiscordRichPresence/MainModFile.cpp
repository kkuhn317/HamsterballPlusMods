#define _CRT_SECURE_NO_WARNINGS
#include "HamsterballAPI.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char* APP_ID = "1522782863135608884";
static const char* LOG_FILE = "discord_rpc_log.txt";

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static bool g_connected = false;

struct SceneMapping {
    const char* sceneName;
    const char* displayName;
    const char* imageKey;
};

static SceneMapping RACE_MAP[] = {
    {"Board (Warm-Up)",      "Warm-Up Race",       "tourney-beginner"},
    {"Board (Beginner)",     "Beginner Race",      "tourney-cascade"},
    {"Board (Intermediate)", "Intermediate Race",   "tourney-intermediate"},
    {"Board (Dizzy)",        "Dizzy Race",          "tourney-dizzy"},
    {"Board (Tower)",        "Tower Race",          "tourney-tower"},
    {"Board (Up)",           "Up Race",             "tourney-up"},
    {"Board (Dark)",         "Neon Race",           "tourney-neon"},
    {"Board (Expert)",       "Expert Race",         "tourney-expert"},
    {"Board (Odd)",          "Odd Race",            "tourney-odd"},
    {"Board (Toob)",         "Toob Race",           "tourney-toob"},
    {"Board (Wobbly)",       "Wobbly Race",         "tourney-wobbly"},
    {"Board (Glass)",        "Glass Race",          "tourney-glass"},
    {"Board (Sky)",          "Sky Race",            "tourney-sky"},
    {"Board (Master)",       "Master Race",         "tourney-master"},
    {"Board (Impossible)",   "Impossible Race",     "tourney-impossible"},
    {NULL, NULL, NULL}
};

static SceneMapping ARENA_MAP[] = {
    {"RumbleBoard (Warmup Arena)",       "Warm-Up Arena",      NULL},
    {"RumbleBoard (Beginner Arena)",     "Beginner Arena",     NULL},
    {"RumbleBoard (Intermediate Arena)", "Intermediate Arena", NULL},
    {"RumbleBoard (Dizzy Arena)",        "Dizzy Arena",        NULL},
    {"RumbleBoard (Tower Arena)",        "Tower Arena",        NULL},
    {"RumbleBoard (Up Arena)",           "Up Arena",           NULL},
    {"RumbleBoard (Neon Arena)",         "Neon Arena",         NULL},
    {"RumbleBoard (Expert Arena)",       "Expert Arena",       NULL},
    {"RumbleBoard (Odd Arena)",          "Odd Arena",          NULL},
    {"RumbleBoard (Toob Arena)",         "Toob Arena",         NULL},
    {"RumbleBoard (Wobbly Arena)",       "Wobbly Arena",       NULL},
    {"RumbleBoard (Sky Arena)",          "Sky Arena",          NULL},
    {NULL, NULL, NULL}
};

static const SceneMapping* LookupScene(const char* name, SceneMapping* table) {
    if (!name) return NULL;
    for (int i = 0; table[i].sceneName; i++) {
        if (strcmp(name, table[i].sceneName) == 0) return &table[i];
    }
    return NULL;
}

struct GameState {
    bool inLevel;
    bool isArena;
    bool isPartyRace;
    const char* displayName;
    const char* imageKey;
    time_t levelStartTime;
};

static GameState g_currentState = {};
static GameState g_lastSentState = {};
static time_t g_lastSendTime = 0;
static bool g_forceUpdate = false;
static bool g_rpcEnabled = true;
static bool g_running = true;

static void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) return;
    char dir[512];
    GetCurrentDirectoryA(512, dir);
    char path[768];
    snprintf(path, sizeof(path), "%s\\%s", dir, LOG_FILE);
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") == 0 && f) {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_s(&tmv, &now);
        char ts[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(f, "[%s] ", ts);
        fputs(buf, f);
        if (len > 0 && buf[len - 1] != '\n') fputc('\n', f);
        fclose(f);
    }
    OutputDebugStringA(buf);
}

static bool SendFrame(DWORD opcode, const char* json) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    DWORD len = (DWORD)strlen(json);
    size_t totalLen = 8 + len;
    char* buf = (char*)malloc(totalLen);
    if (!buf) return false;
    memcpy(buf, &opcode, 4);
    memcpy(buf + 4, &len, 4);
    memcpy(buf + 8, json, len);
    DWORD written;
    BOOL ok = WriteFile(g_pipe, buf, (DWORD)totalLen, &written, NULL);
    free(buf);
    if (!ok || written != totalLen) {
        Log("SendFrame FAILED (opcode=%lu, written=%lu, expected=%zu)", opcode, written, totalLen);
        return false;
    }
    return true;
}

static bool ReadFrame(char* outBuf, int bufSize) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    char header[8];
    DWORD bytesRead = 0;
    if (!ReadFile(g_pipe, header, 8, &bytesRead, NULL) || bytesRead != 8) {
        Log("ReadFrame: failed to read header (got %lu bytes)", bytesRead);
        return false;
    }
    DWORD opcode, length;
    memcpy(&opcode, header, 4);
    memcpy(&length, header + 4, 4);
    if (length > (DWORD)(bufSize - 1)) length = bufSize - 1;
    DWORD read = 0;
    if (!ReadFile(g_pipe, outBuf, length, &read, NULL) || read != length) {
        Log("ReadFrame: failed to read body (got %lu/%lu)", read, length);
        return false;
    }
    outBuf[read] = '\0';
    return true;
}

static bool Connect() {
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            Log("Connected to pipe: %s", path);
            char json[256];
            snprintf(json, sizeof(json), "{\"v\":1,\"client_id\":\"%s\"}", APP_ID);
            Log("Sending handshake: %s", json);
            if (SendFrame(0, json)) {
                char response[4096];
                if (ReadFrame(response, sizeof(response))) {
                    Log("Handshake response: %s", response);
                    if (strstr(response, "\"code\"") && !strstr(response, "\"code\":0")) {
                        Log("Handshake ERROR — check app_id is correct");
                        CloseHandle(g_pipe);
                        g_pipe = INVALID_HANDLE_VALUE;
                        continue;
                    }
                    g_connected = true;
                    g_forceUpdate = true;
                    Log("Connected to Discord successfully!");
                    return true;
                }
                else {
                    Log("Handshake: no response, assuming connected anyway");
                    g_connected = true;
                    g_forceUpdate = true;
                    return true;
                }
            }
            else {
                Log("Handshake send failed");
                CloseHandle(g_pipe);
                g_pipe = INVALID_HANDLE_VALUE;
            }
        }
    }
    Log("Could not connect to any Discord IPC pipe (0-9). Is Discord running?");
    return false;
}

static void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
    g_connected = false;
}

static bool SendActivity(const char* state, const char* details, time_t startTime, const char* imageKey) {
    if (!g_connected) return false;
    char json[1024];
    const char* img = imageKey ? imageKey : "icon";
    if (startTime > 0) {
        snprintf(json, sizeof(json),
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{"
            "\"state\":\"%s\",\"details\":\"%s\","
            "\"timestamps\":{\"start\":%lld},"
            "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"Hamsterball\"}"
            "}},\"nonce\":\"hb-%lld\"}",
            (int)GetCurrentProcessId(), state, details,
            (long long)startTime, img, (long long)time(NULL));
    }
    else {
        snprintf(json, sizeof(json),
            "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{"
            "\"state\":\"%s\",\"details\":\"%s\","
            "\"assets\":{\"large_image\":\"%s\",\"large_text\":\"Hamsterball\"}"
            "}},\"nonce\":\"hb-%lld\"}",
            (int)GetCurrentProcessId(), state, details,
            img, (long long)time(NULL));
    }
    Log("Sending activity: state='%s' details='%s' image='%s' start=%lld", state, details, img, (long long)startTime);
    return SendFrame(1, json);
}

static bool ClearActivity() {
    if (!g_connected) return false;
    char json[256];
    snprintf(json, sizeof(json),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"hb-%lld\"}",
        (int)GetCurrentProcessId(), (long long)time(NULL));
    Log("Clearing activity");
    return SendFrame(1, json);
}

static void ReadGameState(IModAPI* api) {
    memset(&g_currentState, 0, sizeof(g_currentState));

    Scene* scene = api->GetScene();
    if (!scene) return;

    const char* sceneName = NULL;
    if (scene->name && !IsBadReadPtr(scene->name, 1)) {
        sceneName = scene->name;
    }
    if (!sceneName) return;

    const SceneMapping* race = LookupScene(sceneName, RACE_MAP);
    const SceneMapping* arena = LookupScene(sceneName, ARENA_MAP);

    if (race) {
        g_currentState.inLevel = true;
        g_currentState.isArena = false;
        g_currentState.displayName = race->displayName;
        g_currentState.imageKey = race->imageKey;
        int pc = 1;
        if (api->GetPlayer2()) pc++;
        if (api->GetPlayer3()) pc++;
        if (api->GetPlayer4()) pc++;
        g_currentState.isPartyRace = (pc > 1);
    }
    else if (arena) {
        g_currentState.inLevel = true;
        g_currentState.isArena = true;
        g_currentState.displayName = arena->displayName;
        g_currentState.imageKey = NULL;
    }
    else {
        g_currentState.inLevel = true;
        g_currentState.displayName = sceneName;
        g_currentState.imageKey = NULL;
    }
}

static bool StateChanged() {
    if (g_forceUpdate) return true;
    if (g_currentState.inLevel != g_lastSentState.inLevel) return true;
    if (g_currentState.isArena != g_lastSentState.isArena) return true;
    if (g_currentState.isPartyRace != g_lastSentState.isPartyRace) return true;
    if (g_currentState.displayName != g_lastSentState.displayName) return true;
    return false;
}

static void BuildPresence(char* outState, int stateLen, char* outDetails, int detailsLen,
    time_t* outStartTime, const char** outImage) {
    *outStartTime = 0;
    *outImage = NULL;

    if (!g_currentState.inLevel) {
        strncpy_s(outState, stateLen, "In Menu", _TRUNCATE);
        strncpy_s(outDetails, detailsLen, "Hamsterball", _TRUNCATE);
        return;
    }

    *outStartTime = g_currentState.levelStartTime;
    *outImage = g_currentState.imageKey;

    if (g_currentState.isArena) {
        strncpy_s(outState, stateLen, "Rodent Rumble", _TRUNCATE);
    }
    else if (g_currentState.isPartyRace) {
        strncpy_s(outState, stateLen, "Party Race", _TRUNCATE);
    }
    else {
        strncpy_s(outState, stateLen, "Single Race", _TRUNCATE);
    }

    strncpy_s(outDetails, detailsLen, g_currentState.displayName ? g_currentState.displayName : "Unknown", _TRUNCATE);
}

static DWORD WINAPI DiscordThread(LPVOID param) {
    IModAPI* api = (IModAPI*)param;

    Log("=== Discord Rich Presence mod starting ===");
    Log("App ID: %s", APP_ID);

    time_t lastReconnectAttempt = 0;
    bool firstConnect = true;

    while (g_running) {
        if (!g_rpcEnabled) {
            if (g_connected) {
                ClearActivity();
                Disconnect();
            }
            Sleep(2000);
            continue;
        }

        if (!g_connected) {
            time_t now = time(NULL);
            if (now - lastReconnectAttempt >= 15 || firstConnect) {
                firstConnect = false;
                lastReconnectAttempt = now;
                Connect();
            }
            Sleep(2000);
            continue;
        }

        ReadGameState(api);

        time_t now = time(NULL);
        bool shouldUpdate = StateChanged();

        if (shouldUpdate) {
            if (g_currentState.inLevel && !g_lastSentState.inLevel) {
                if (g_currentState.levelStartTime == 0) {
                    g_currentState.levelStartTime = now;
                }
            }

            if (!g_currentState.inLevel && g_lastSentState.inLevel) {
                g_forceUpdate = true;
            }
        }

        if (shouldUpdate && (now - g_lastSendTime >= 15 || g_lastSendTime == 0)) {
            char stateStr[128], detailsStr[128];
            time_t startTime = 0;
            const char* imageKey = NULL;
            BuildPresence(stateStr, sizeof(stateStr), detailsStr, sizeof(detailsStr), &startTime, &imageKey);

            SendActivity(stateStr, detailsStr, startTime, imageKey);

            g_lastSentState = g_currentState;
            g_lastSendTime = now;
            g_forceUpdate = false;
        }

        DWORD avail = 0;
        if (PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL)) {
            if (avail > 0) {
                char readBuf[4096];
                if (!ReadFrame(readBuf, sizeof(readBuf))) {
                    Log("Pipe read failed during poll — disconnecting");
                    Disconnect();
                }
            }
        }
        else {
            Log("PeekNamedPipe failed — Discord likely closed, disconnecting");
            Disconnect();
        }

        Sleep(1000);
    }

    if (g_connected) {
        ClearActivity();
    }
    Disconnect();
    return 0;
}

class DiscordRPCMod : public HamsterballAPI {
private:
    IModAPI* api = nullptr;
    HANDLE thread = NULL;
public:
    const char* GetModName() override { return "Discord Rich Presence"; }
    const char* GetAuthorName() override { return "Hamsterbot"; }
    int GetApiVersion() override { return HAMSTERBALL_API_VERSION; }

    void Initialize(IModAPI* modApi) override {
        api = modApi;
        CustomButton btn("discord_rpc", "Discord Rich Presence");
        btn.defaultState = true;
        modApi->CreateToggleButton(btn, this);
        g_running = true;
        thread = CreateThread(NULL, 0, DiscordThread, modApi, 0, NULL);
    }

    void onButtonToggle(const char* buttonId, bool newState) override {
        if (strcmp(buttonId, "discord_rpc") == 0) {
            g_rpcEnabled = newState;
            g_forceUpdate = true;
        }
    }

    void onLevelStart() override {
        g_currentState.levelStartTime = time(NULL);
        g_forceUpdate = true;
    }

    void onSceneEnd() override {
        g_forceUpdate = true;
    }
};

extern "C" __declspec(dllexport) HamsterballAPI* CreateModInstance() {
    return new DiscordRPCMod();
}
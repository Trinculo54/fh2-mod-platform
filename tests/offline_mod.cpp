// A mod for the platform test's online phases: made for offline play (FH2ModInfo.offline_only, API 9).
// It hooks a pointer of its own, so the phase where FH2MP goes online after it started ("online-late",
// platform 2.9.0) can see the platform stop it: once every mod has started, it calls through the
// pointer and tries enable_hooks again, and logs what happened. With FH2_TEST_OFFLINE_EDITS set
// ("online-late", "online-ready"), it also edits game text, adds SQL and replaces a file, so those
// phases can see the platform take them out when it's stopped, and refuse new ones. With
// FH2_TEST_OFFLINE_IDLE set ("online-clean"), it only registers: nothing of it has run when FH2MP says
// it's going online, so the platform stops it then; once the game is online, it tries to hook its
// pointer and replace its file, and logs what it was given.
#include "fh2mod.h"
#include <windows.h>

#include <cstdio>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;
static bool g_edits;

static int original_fn() { return 1; }
static int detour_fn() { return 2; }
alignas(8) static int (*volatile g_slot)() = original_fn;

static FH2Hook* hook_slot() {
    void* original = nullptr;
    return g_api->hook_pointer(g_mod, "OfflineMod's own pointer", reinterpret_cast<void**>(const_cast<int (**)()>(&g_slot)),
                               reinterpret_cast<void*>(detour_fn), &original);
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{sizeof info, "OfflineMod", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    info.description  = "Made for offline play.";
    info.offline_only = 1;
    FH2Mod* m = api->register_mod(&info);
    if (!m) return 1;   // the platform turned it away: the game is online
    g_api = api;
    g_mod = m;
    api->log(m, api->playing_online() ? "running online" : "running offline");
    if (GetEnvironmentVariableW(L"FH2_TEST_OFFLINE_IDLE", nullptr, 0) > 0) {
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            if (!g_api->wait_for_mods(20000)) return 0;
            for (int i = 0; i < 100 && !g_api->playing_online(); ++i) Sleep(100);   // FH2MP's going_online
            FH2Hook* h      = hook_slot();
            const int again = h ? g_api->enable_hooks(g_mod, h) : -1;
            char line[200];
            std::snprintf(line, sizeof line, "after start: online=%d, the %s ran, enable_hooks gave %d, set_file gave %d", g_api->playing_online(),
                          g_slot() == 2 ? "detour" : "original", again, g_api->set_file(g_mod, "UI/OfflineMod.txt", "late", 4));
            g_api->log(g_mod, line);
            return 0;
        }, nullptr, 0, nullptr);
        return 0;
    }
    FH2Hook* h = hook_slot();
    if (h) api->enable_hooks(m, h);
    g_edits = GetEnvironmentVariableW(L"FH2_TEST_OFFLINE_EDITS", nullptr, 0) > 0;
    if (g_edits) {
        api->set_string(m, "LoadingMessages.str", 0x6875, L"from OfflineMod");
        api->add_sql(m, "UPDATE AIPlayers SET XP = 4242 WHERE Id = 1;");
        api->set_file(m, "UI/OfflineMod.txt", "from OfflineMod", 15);
    }
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (!g_api->wait_for_mods(20000)) return 0;
        const int ran   = g_slot();
        const int again = g_api->enable_hooks(g_mod, nullptr);
        char line[200];
        int n = std::snprintf(line, sizeof line, "after start: online=%d, the %s ran, enable_hooks gave %d", g_api->playing_online(),
                              ran == 2 ? "detour" : "original", again);
        if (g_edits && g_api->playing_online())   // stopped by now: set_file is refused
            std::snprintf(line + n, sizeof line - n, ", set_file gave %d", g_api->set_file(g_mod, "UI/OfflineMod.txt", "again", 5));
        g_api->log(g_mod, line);
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

// A mod for the platform test's online phases: made for offline play (FH2ModInfo.offline_only, API 9).
// It hooks a pointer of its own, so the phase where FH2MP goes online after it started ("online-late",
// platform 2.9.0) can see the platform stop it: once every mod has started, it calls through the
// pointer and tries enable_hooks again, and logs what happened.
#include "fh2mod.h"
#include <windows.h>

#include <cstdio>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

static int original_fn() { return 1; }
static int detour_fn() { return 2; }
alignas(8) static int (*volatile g_slot)() = original_fn;

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{sizeof info, "OfflineMod", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    info.description  = "Made for offline play.";
    info.offline_only = 1;
    FH2Mod* m = api->register_mod(&info);
    if (!m) return 1;   // the platform turned it away: the game is online
    g_api = api;
    g_mod = m;
    api->log(m, api->playing_online() ? "running online" : "running offline");
    void* original = nullptr;
    FH2Hook* h = api->hook_pointer(m, "OfflineMod's own pointer", reinterpret_cast<void**>(const_cast<int (**)()>(&g_slot)),
                                   reinterpret_cast<void*>(detour_fn), &original);
    if (h) api->enable_hooks(m, h);
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (!g_api->wait_for_mods(20000)) return 0;
        const int ran   = g_slot();
        const int again = g_api->enable_hooks(g_mod, nullptr);
        char line[160];
        std::snprintf(line, sizeof line, "after start: online=%d, the %s ran, enable_hooks gave %d", g_api->playing_online(),
                      ran == 2 ? "detour" : "original", again);
        g_api->log(g_mod, line);
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

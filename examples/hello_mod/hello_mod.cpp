// HelloMod: the smallest useful FH2 mod, showing the platform API.
//
// Build it (this repo's CMake builds it as HelloMod.dll), drop it in
// fh2-mods next to azimuth.exe, and launch the game:
//   - fh2-mods\platform.log gets a few lines from it
//   - the "Fast Traveling to Destination" loading message gets a greeting
//   - once you press Play, it logs where the game was mapped
#include "fh2mod.h"

#include <windows.h>

#include <cstdio>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod          = nullptr;

DWORD WINAPI wait_for_game(LPVOID) {
    // Anything slow goes on your own thread, never in fh2mod_init.
    if (void* base = g_api->wait_for_game(0xFFFFFFFFu)) {
        char line[96];
        std::snprintf(line, sizeof line, "the game is mapped at %p", base);
        g_api->log(g_mod, line);
    }
    return 0;
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 1) return 1;
    g_api = api;

    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "HelloMod";
    info.version          = "1.0.0";
    info.github_repo      = nullptr; // "You/YourRepo" to get update notices
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    api->log(g_mod, "hello from HelloMod");

    // LoadingMessages.str, key 0x6875 (IDS_FastTravel): shown while fast
    // travelling. Edits made here, inside fh2mod_init, are written for you
    // before the game reads its strings.
    api->set_string(g_mod, "LoadingMessages.str", 0x6875, L"Fast Traveling to Destination (hello from HelloMod!)");

    if (HANDLE t = CreateThread(nullptr, 0, wait_for_game, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

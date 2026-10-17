// A mod for the platform test: notices, conflicting edits, a late edit.
#include "fh2mod.h"
#include <windows.h>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{sizeof info, "TestMod", "1.0.0", nullptr, "9.9.9", 0};
    g_mod = api->register_mod(&info);
    api->set_notice(g_mod, "Test notice line\n\nsecond line  ");
    api->replace_string(g_mod, "AIPlayers.str", L"Nocomply76", L"Buddy1");
    api->set_string(g_mod, "LoadingMessages.str", 0x6875, L"from TestMod");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(300);
        g_api->replace_string(g_mod, "AIPlayers.str", L"Super Final", L"Buddy2");
        g_api->apply_strings(g_mod);
        g_api->log(g_mod, "late edit applied");
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

// A mod for the platform test: notices, conflicting edits, a late edit,
// file overrides, database changes, an interface for AaaMod.
#include "fh2mod.h"
#include <windows.h>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

struct MathV1 {
    uint32_t size;
    int (*add)(int, int);
};
static const MathV1 g_math{sizeof(MathV1), [](int a, int b) { return a + b; }};

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{sizeof info, "TestMod", "1.0.0", nullptr, "9.9.9", 0};
    g_mod = api->register_mod(&info);
    api->set_notice(g_mod, "Test notice line\n\nsecond line  ");
    api->replace_string(g_mod, "AIPlayers.str", L"Nocomply76", L"Buddy1");
    api->set_string(g_mod, "LoadingMessages.str", 0x6875, L"from TestMod");

    const char x[] = "<xml/>";
    api->set_file(g_mod, "drivatarglobalcache\\DefaultCache.bin", x, sizeof x - 1); // AaaMod has it
    api->set_file(g_mod, "Stripped/StringTables/EN/Main.str", x, sizeof x - 1);   // refused
    api->set_file(g_mod, "UI/../../evil.txt", x, sizeof x - 1);                    // refused
    api->set_file(g_mod, "media/UI/keep.xml", x, sizeof x - 1);
    api->set_file(g_mod, "UI/handmade.xml", x, sizeof x - 1);
    api->set_file(g_mod, "UI/temp.xml", x, sizeof x - 1);
    api->add_sql(g_mod, "UPDATE AIPlayers SET XP = 12345 WHERE Id = 1; UPDATE AIPlayers SET XP = 777 WHERE Id = 2");
    api->publish_interface(g_mod, "TestMod.Math", 1, &g_math);

    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Sleep(300);
        g_api->replace_string(g_mod, "AIPlayers.str", L"Super Final", L"Buddy2");
        g_api->apply_strings(g_mod);
        g_api->clear_file(g_mod, "UI/temp.xml");
        g_api->log(g_mod, "late edit applied");
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

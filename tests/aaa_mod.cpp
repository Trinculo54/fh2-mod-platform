// A mod for the platform test that loads first (alphabetically): owns a
// file TestMod also wants, has SQL that fails, uses TestMod's interface,
// and crashes on request (the "crash" phase).
#include "fh2mod.h"
#include <windows.h>

#include <cstdio>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

struct MathV1 {
    uint32_t size;
    int (*add)(int, int);
};

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 2) return 1;
    g_api = api;
    FH2ModInfo info{sizeof info, "AaaMod", "0.5.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    g_mod = api->register_mod(&info);
    const char data[] = "from AaaMod";
    api->set_file(g_mod, "drivatarglobalcache/DefaultCache.bin", data, sizeof data - 1);
    api->add_sql(g_mod, "UPDATE NoSuchTable SET x = 1");
    if (api->wait_for_mods(0) == 0) api->log(g_mod, "wait_for_mods from init returned 0");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (!g_api->wait_for_mods(20000)) return 0;
        char line[160];
        if (auto* m = static_cast<const MathV1*>(g_api->get_interface("TestMod.Math", 1))) {
            std::snprintf(line, sizeof line, "TestMod.Math says %d", m->add(2, 3));
            g_api->log(g_mod, line);
        }
        if (!g_api->get_interface("TestMod.Math", 2)) g_api->log(g_mod, "no TestMod.Math v2");
        const char* v = g_api->mod_version("testmod");
        std::snprintf(line, sizeof line, "TestMod version %s", v ? v : "(none)");
        g_api->log(g_mod, line);
        if (GetFileAttributesW(L"crash.txt") != INVALID_FILE_ATTRIBUTES) {
            g_api->log(g_mod, "crashing on purpose");
            *static_cast<volatile int*>(nullptr) = 1;
        }
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

// A mod that loads other mods, for the platform test's "hosts" phases (platform 2.9.0): one DLL that registers
// HostMod, then MemberA (two settings and a link), then MemberB (made for offline play, one setting), all in
// its fh2mod_init, as a loader of scripted or .NET mods does. YOUR MODS lists each of them. With FH2_TEST_MEMBER_C set
// it registers MemberC too. Once fh2mod_init has returned, a thread registers MemberLate, which the list can't place.
#include "fh2mod.h"
#include <windows.h>

static const FH2ModApi* g_api;

static FH2Mod* reg(const char* name, const char* about, int offline_only) {
    FH2ModInfo info{sizeof info, name, "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    info.description  = about;
    info.offline_only = offline_only;
    return g_api->register_mod(&info);
}

// An On/Off row, or with `url` a link row.
static void row(FH2Mod* m, const char* key, const char* label, const char* about, const char* url) {
    FH2Setting s{sizeof s};
    s.key         = key;
    s.label       = label;
    s.description = about;
    s.value       = 1;
    s.kind        = url ? FH2_SETTING_LINK : FH2_SETTING_ON_OFF;
    s.url         = url;
    g_api->add_setting(m, &s);
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2Mod* host = reg("HostMod", "Loads other mods.", 0);
    if (!host) return 0;   // the platform turned it away: the game is online and it's set to stay off
    api->log(host, "running");
    if (FH2Mod* a = reg("MemberA", "The first mod HostMod loads.", 0)) {
        api->log(a, "running");
        row(a, "Alpha", "MemberA first", "A setting of MemberA's.", nullptr);
        row(a, "Beta", "MemberA second", "Another setting of MemberA's.", nullptr);
        row(a, "Site", "MemberA site", "A link of MemberA's.", "https://example.com/member-a");
    }
    if (FH2Mod* b = reg("MemberB", "The second mod HostMod loads, made for offline play.", 1)) {
        api->log(b, "running");
        row(b, "Gamma", "MemberB only", "A setting of MemberB's.", nullptr);
    }
    if (GetEnvironmentVariableW(L"FH2_TEST_MEMBER_C", nullptr, 0) > 0)
        if (FH2Mod* c = reg("MemberC", "A mod HostMod loads from the second start on.", 0)) api->log(c, "running");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (!g_api->wait_for_mods(20000)) return 0;
        if (FH2Mod* late = reg("MemberLate", "Registered after fh2mod_init.", 0)) g_api->log(late, "running");
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

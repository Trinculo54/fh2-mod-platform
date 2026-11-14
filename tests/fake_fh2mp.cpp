// A stand-in for FH2MP's DLL in the platform test's later online phases (platform 2.9.0). It registers
// as FH2MP from a DLL with another name (ZzFH2MP.dll, MpClient.dll), as a renamed FH2MP.dll would. In fh2mod_init it
// writes the session file the FH2MP launcher writes, where FH2MP looks for it (data\bin when that's
// there, else next to azimuth.exe), if it isn't there yet: a Play pressed while the game was starting.
// Once every mod has started, it asks FH2Platform.Online, as FH2MP would before it connects. With
// FH2_TEST_NO_SESSION set ("online-ready") it writes no session, so only that call makes the game online.
#include "fh2mod.h"
#include <windows.h>

#include <cstdio>
#include <string>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 9) return 1;
    g_api = api;
    FH2ModInfo info{sizeof info, "FH2MP", "0.0.1", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    info.description = "A stand-in for FH2MP in the platform test.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    std::wstring dir       = api->azimuth_dir;
    const std::wstring bin = dir + L"\\data\\bin";
    const DWORD a          = GetFileAttributesW(bin.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) dir = bin;
    const std::wstring session = dir + L"\\fh2mp-session.txt";
    if (GetFileAttributesW(session.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetEnvironmentVariableW(L"FH2_TEST_NO_SESSION", nullptr, 0) == 0) {
        HANDLE f = CreateFileW(session.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n = 0;
            WriteFile(f, "xuid=1\ntoken=test\n", 18, &n, nullptr);
            CloseHandle(f);
            api->log(g_mod, "wrote the session file in fh2mod_init");
        }
    }
    api->log(g_mod, api->playing_online() ? "in fh2mod_init: online" : "in fh2mod_init: offline");
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        if (!g_api->wait_for_mods(20000)) return 0;
        const auto* o = static_cast<const FH2PlatformOnline*>(g_api->get_interface(FH2_ONLINE_INTERFACE, 1));
        if (!o || o->size < sizeof *o) {
            g_api->log(g_mod, "no FH2Platform.Online");
            return 0;
        }
        char names[64], shorter[4], line[200];
        const int n = o->going_online(names, sizeof names);
        o->going_online(shorter, sizeof shorter);
        std::snprintf(line, sizeof line, "going_online: %d started (%s; cut short \"%s\"), playing_online %d", n, names,
                      shorter, o->playing_online());
        g_api->log(g_mod, line);
        return 0;
    }, nullptr, 0, nullptr);
    return 0;
}

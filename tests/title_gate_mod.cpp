// The title gate test's mod (tests/title_gate_test.cpp): leaves a file when its DLL is loaded and another when its
// fh2mod_init is done, which takes 300 ms so the test can tell a game that was held from one that wasn't.
#include "fh2mod.h"

#include <windows.h>

static void marker(const char* name) {
    HANDLE f = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, "ran", 3, &n, nullptr);
    CloseHandle(f);
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) marker("gate-dllmain.txt");
    return TRUE;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{};
    info.size    = sizeof info;
    info.name    = "GateMod";
    info.version = "1.0.0";
    FH2Mod* mod  = api->register_mod(&info);
    Sleep(300);
    marker("gate-init.txt");
    if (mod) api->log(mod, "started");
    return 0;
}

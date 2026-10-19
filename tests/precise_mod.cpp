// In-game check of API 7 (not shipped): instruction hooks on two game functions, logging what
// they see. Drop PreciseTest.dll into fh2-mods, play a minute, read fh2-mods\platform.log.
//   CWorldCollObjMan update (every frame): game+0x384C46 (mov r15, rcx; mov edx, [rcx+0x260]),
//     next to WreckingBall's function hook at the same function's start: rcx is the manager.
//   The freeroam HUD shown (Hud vtable +0x488): game+0xD38DF2 (mov byte [rcx+0x410], 1), after
//     mov rbx, rcx: rbx and rcx are the Hud page, whose vtable is game+0x2DEF9C8.
#include "fh2mod.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;
static char* g_base;
static std::atomic<uint64_t> g_frames{0};
static std::atomic<int> g_huds{0};

static void on_update(FH2Context* c, void*) {
    const uint64_t n = ++g_frames;
    if (n == 1 || n % 3000 == 0) {
        char b[160];
        std::snprintf(b, sizeof b, "update hook: %llu calls; manager %p; second argument %.4f",
                      static_cast<unsigned long long>(n), reinterpret_cast<void*>(c->rcx), c->xmm[1].f[0]);
        g_api->log(g_mod, b);
    }
}

static void on_hud(FH2Context* c, void*) {
    if (++g_huds > 20) return;
    const bool same   = c->rbx == c->rcx && c->rbx;
    const bool is_hud = same && *reinterpret_cast<char**>(c->rbx) == g_base + 0x2DEF9C8;
    char b[160];
    std::snprintf(b, sizeof b, "HUD hook: page %p, rbx %s rcx, vtable %s", reinterpret_cast<void*>(c->rbx),
                  same ? "==" : "!=", is_hud ? "is the Hud's (ok)" : "ISN'T the Hud's (FAIL)");
    g_api->log(g_mod, b);
}

static DWORD WINAPI start(LPVOID) {
    g_base = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_base) return 0;
    FH2Hook* u = g_api->hook_instruction(g_mod, "CWorldCollObjMan update", g_base + 0x384C46, &on_update, nullptr);
    FH2Hook* h = g_api->hook_instruction(g_mod, "Hud shown", g_base + 0xD38DF2, &on_hud, nullptr);
    g_api->log(g_mod, u && h ? "both instruction hooks made" : "an instruction hook wasn't made (FAIL)");
    if (u) g_api->enable_hooks(g_mod, u);
    if (h) g_api->enable_hooks(g_mod, h);
    return 0;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (!api || api->api_version < 7) return 1;
    g_api = api;
    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "PreciseTest";
    info.version          = "0.1.0";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.description      = "A test of the platform's instruction hooks. Not a real mod: remove it after testing.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    if (HANDLE t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

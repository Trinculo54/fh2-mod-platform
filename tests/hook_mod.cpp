// A mod for the hook test (tests/hook_test.cpp). It registers twice, as
// HookA and HookB, so hooks from two mods meet on the same code, and logs a
// "check ok"/"check FAIL" line for everything it tries. Game addresses are
// from docs/game-notes.md.
#include "fh2mod.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

static const FH2ModApi* g_api;
static FH2Mod* g_a;
static FH2Mod* g_b;
static int g_fails;
static std::string g_trace;

static void check(bool ok, const char* what) {
    char line[256];
    std::snprintf(line, sizeof line, "check %s %s", ok ? "ok  " : "FAIL", what);
    g_api->log(g_a, line);
    if (!ok) ++g_fails;
}
#define CHECK(x) check((x), #x)

// ---- code of our own to hook --------------------------------------------------------

extern "C" __attribute__((noinline)) int victim(int x) {
    g_trace += "V";
    volatile int y = x;
    return y * 2 + 1;
}

static int (*orig_a)(int);
static int (*orig_b)(int);
static int detour_a(int x) {
    g_trace += "A";
    if (x == 666) *static_cast<volatile int*>(nullptr) = 1; // the crash phase
    return orig_a(x);
}
static int detour_b(int x) {
    g_trace += "B";
    return orig_b(x);
}
static std::string run(int x = 1) {
    g_trace.clear();
    const int r = victim(x);
    if (r != x * 2 + 1) g_trace += "!";
    return g_trace;
}

// Where a jump through our stubs (jmp [rip+x]) and MinHook's relays ends up.
static const void* follow(const void* p) {
    for (int i = 0; i < 8 && p; ++i) {
        const auto* c = static_cast<const unsigned char*>(p);
        int32_t d     = 0;
        if (c[0] == 0xFF && c[1] == 0x25) {
            std::memcpy(&d, c + 2, 4);
            std::memcpy(&p, c + 6 + d, 8);
        } else if (c[0] == 0xE9) {
            std::memcpy(&d, c + 1, 4);
            p = c + 5 + d;
        } else {
            break;
        }
    }
    return p;
}

static const void* call_target(const void* site) {
    const auto* c = static_cast<const unsigned char*>(site);
    int32_t d     = 0;
    std::memcpy(&d, c + 1, 4);
    return c + 5 + d;
}

// "sub rsp,28h; call victim; add rsp,28h; ret", somewhere a rel32 reaches victim.
static unsigned char* make_caller() {
    const auto v = reinterpret_cast<uintptr_t>(&victim);
    for (uintptr_t a = (v & ~uintptr_t{0xFFFF}) + 0x10000000; a > v - 0x10000000; a -= 0x10000)
        if (auto* p = static_cast<unsigned char*>(
                VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {
            const int32_t rel = static_cast<int32_t>(v - reinterpret_cast<uintptr_t>(p + 9));
            const unsigned char code[] = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
            std::memcpy(p, code, sizeof code);
            std::memcpy(p + 5, &rel, 4);
            // A second one at +0x100, its call at +12, so the rel32 straddles a
            // 16-byte boundary and can't be swapped in one atomic write.
            const unsigned char code2[] = {0x48, 0x83, 0xEC, 0x28, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                           0x90, 0xE8, 0,    0,    0,    0,    0x48, 0x83, 0xC4, 0x28, 0xC3};
            const int32_t rel2 = static_cast<int32_t>(v - reinterpret_cast<uintptr_t>(p + 0x100 + 17));
            std::memcpy(p + 0x100, code2, sizeof code2);
            std::memcpy(p + 0x100 + 13, &rel2, 4);
            DWORD old = 0;
            VirtualProtect(p, 0x1000, PAGE_EXECUTE_READ, &old);
            return p;
        }
    return nullptr;
}
static int (*orig_call)(int);
static int call_detour(int x) {
    g_trace += "C";
    return orig_call(x);
}

static int dummy_detour() { return 0; }
static void* orig_dummy;
static void* orig_dummy2;
static void* orig_dummy3;

static DWORD WINAPI tests(LPVOID) {
    const bool crash = GetFileAttributesW(L"crash.txt") != INVALID_FILE_ATTRIBUTES;
    unsigned char before[16];
    std::memcpy(before, reinterpret_cast<void*>(&victim), sizeof before);

    // Our own code: hooks there don't wait for the game.
    FH2Hook* a = g_api->hook_function(g_a, "victim", reinterpret_cast<void*>(&victim),
                                      reinterpret_cast<void*>(&detour_a), reinterpret_cast<void**>(&orig_a));
    CHECK(a != nullptr);
    CHECK(run() == "V");          // made, but off
    CHECK(orig_a(1) == 3);        // original works while off
    CHECK(g_api->enable_hooks(g_a, a) == 1);
    CHECK(run() == "AV");
    FH2Hook* b = g_api->hook_function(g_b, "victim", reinterpret_cast<void*>(&victim),
                                      reinterpret_cast<void*>(&detour_b), reinterpret_cast<void**>(&orig_b));
    CHECK(b != nullptr);
    CHECK(g_api->enable_hooks(g_b, nullptr) == 1);
    CHECK(run() == "BAV");        // newest first
    CHECK(g_api->disable_hooks(g_a, a) == 1);
    CHECK(run() == "BV");         // the middle one leaves, the chain closes
    CHECK(g_api->enable_hooks(g_a, nullptr) == 1);
    CHECK(run() == "ABV");        // back in, now the newest
    CHECK(g_api->disable_hooks(g_b, a) == 0); // not B's to switch
    CHECK(g_api->disable_hooks(g_b, nullptr) == 1 && g_api->disable_hooks(g_a, nullptr) == 1);
    CHECK(run() == "V");
    CHECK(std::memcmp(before, reinterpret_cast<void*>(&victim), sizeof before) == 0); // bytes put back

    // A call site, and overlap with a function hook.
    unsigned char* caller = make_caller();
    CHECK(caller != nullptr);
    auto call_fn = reinterpret_cast<int (*)(int)>(caller);
    FH2Hook* c   = g_api->hook_call(g_a, "caller->victim", caller + 4, reinterpret_cast<void*>(&call_detour),
                                    reinterpret_cast<void**>(&orig_call));
    CHECK(c != nullptr);
    CHECK(follow(reinterpret_cast<void*>(orig_call)) == reinterpret_cast<void*>(&victim));
    CHECK(g_api->enable_hooks(g_a, c) == 1);
    g_trace.clear();
    CHECK(call_fn(5) == 11 && g_trace == "CV");
    CHECK(follow(call_target(caller + 4)) == reinterpret_cast<void*>(&call_detour));
    CHECK(g_api->hook_function(g_b, "caller", caller, reinterpret_cast<void*>(&dummy_detour), &orig_dummy) ==
          nullptr); // overlaps HookA's call hook
    CHECK(g_api->disable_hooks(g_a, c) == 1);
    CHECK(call_target(caller + 4) == reinterpret_cast<void*>(&victim));
    g_trace.clear();
    CHECK(call_fn(5) == 11 && g_trace == "V");

    // A call whose rel32 straddles 16 bytes: written with the other threads paused.
    auto call_fn2 = reinterpret_cast<int (*)(int)>(caller + 0x100);
    FH2Hook* c2   = g_api->hook_call(g_a, "caller2->victim", caller + 0x10C, reinterpret_cast<void*>(&call_detour),
                                     reinterpret_cast<void**>(&orig_call));
    CHECK(c2 != nullptr && g_api->enable_hooks(g_a, c2) == 1);
    g_trace.clear();
    CHECK(call_fn2(5) == 11 && g_trace == "CV");
    CHECK(g_api->disable_hooks(g_a, c2) == 1 && call_target(caller + 0x10C) == reinterpret_cast<void*>(&victim));

    // A hook that's off goes straight to the game's code, past any other's detour.
    CHECK(g_api->enable_hooks(g_a, a) == 1 && g_api->enable_hooks(g_b, b) == 1);
    CHECK(g_api->disable_hooks(g_b, b) == 1 && g_api->disable_hooks(g_a, a) == 1);
    g_trace.clear();
    orig_b(1);
    CHECK(g_trace == "V");

    // Bad arguments.
    CHECK(g_api->hook_function(g_a, "null", nullptr, reinterpret_cast<void*>(&detour_a), &orig_dummy) == nullptr);
    CHECK(g_api->enable_hooks(g_a, reinterpret_cast<FH2Hook*>(&g_trace)) == 0);
    CHECK(g_api->hook_call(g_a, "not a call", reinterpret_cast<void*>(&victim), reinterpret_cast<void*>(&detour_a),
                           &orig_dummy) == nullptr);

    // The game. Too early first, if it isn't ready yet.
    char* base = static_cast<char*>(g_api->wait_for_game(30000));
    CHECK(base == reinterpret_cast<char*>(0x140000000));
    const ULONGLONG t0 = GetTickCount64();
    base               = static_cast<char*>(g_api->wait_for_code(60000));
    char line[96];
    std::snprintf(line, sizeof line, "wait_for_code took %llu ms", static_cast<unsigned long long>(GetTickCount64() - t0));
    g_api->log(g_a, line);
    CHECK(base != nullptr);

    // FMOD::System::createSound: a function hook from each mod.
    void* create = base + 0x1997740;
    unsigned char create_before[8];
    std::memcpy(create_before, create, 8);
    FH2Hook* g1 = g_api->hook_function(g_a, "FMOD::System::createSound", create,
                                       reinterpret_cast<void*>(&dummy_detour), &orig_dummy);
    FH2Hook* g2 = g_api->hook_function(g_b, "FMOD::System::createSound", create,
                                       reinterpret_cast<void*>(&dummy_detour), &orig_dummy2);
    CHECK(g1 && g2);
    CHECK(g_api->enable_hooks(g_a, g1) == 1 && g_api->enable_hooks(g_b, g2) == 1);
    CHECK(*static_cast<unsigned char*>(create) == 0xE9);
    CHECK(follow(create) == reinterpret_cast<void*>(&dummy_detour));
    // The radio's createSound call site, inside other code: separate bytes, fine.
    FH2Hook* g3 = g_api->hook_call(g_b, "RadioStreamFmod createSound call", base + 0x1018A13,
                                   reinterpret_cast<void*>(&dummy_detour), &orig_dummy3);
    CHECK(g3 != nullptr);
    CHECK(g3 && follow(orig_dummy3) == follow(create)); // its original is createSound, hooks and all
    CHECK(g_api->disable_hooks(g_a, nullptr) == 1 && g_api->disable_hooks(g_b, nullptr) == 1);
    CHECK(std::memcmp(create_before, create, 8) == 0);

    // WheresMyBuddy's vtable slot.
    void** slot = reinterpret_cast<void**>(base + 0x2D44F20) + 2;
    void* serialize_orig = nullptr;
    FH2Hook* v = g_api->hook_pointer(g_a, "ChampionshipDrivatarsState::Serialize", slot,
                                     reinterpret_cast<void*>(&dummy_detour), &serialize_orig);
    CHECK(v != nullptr);
    CHECK(follow(serialize_orig) == base + 0xA68870);
    CHECK(g_api->enable_hooks(g_a, v) == 1);
    CHECK(*slot != base + 0xA68870 && follow(*slot) == reinterpret_cast<void*>(&dummy_detour));
    CHECK(g_api->disable_hooks(g_a, v) == 1);
    CHECK(*slot == base + 0xA68870);

    // FMOD::Sound::getUserData was changed by the test host, as Extra Options would.
    CHECK(g_api->hook_function(g_b, "FMOD::Sound::getUserData", base + 0x199FBE0,
                               reinterpret_cast<void*>(&dummy_detour), &orig_dummy) == nullptr);

    if (crash) {
        g_api->enable_hooks(g_a, a);
        g_api->enable_hooks(g_a, v);
        victim(666);
    }
    char done[64];
    std::snprintf(done, sizeof done, "hook tests done, %d failed", g_fails);
    g_api->log(g_a, done);
    return 0;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    if (api->api_version < 3) return 1;
    FH2ModInfo a{sizeof a, "HookA", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    FH2ModInfo b{sizeof b, "HookB", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    g_a = api->register_mod(&a);
    g_b = api->register_mod(&b);
    if (HANDLE t = CreateThread(nullptr, 0, tests, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

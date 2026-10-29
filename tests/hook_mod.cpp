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


// ---- code of our own for instruction hooks and byte patches (API 7) ----------------
// ivictim(x) = x + 1 + 10 + 100; ijump has a jump back into the bytes a hook at ijump_at needs.
extern "C" int ivictim(int x);
extern "C" int ijump(int x);
extern "C" char ivictim_at[], ivictim_rip[], ivictim_jmp[], ivictim_imm[], ijump_at[], icall_at[], ijcc_at[];
extern "C" int icall(int x);
extern "C" int icaller(int x);
extern "C" char icaller_ret[];
extern "C" int ijcc(int x);
asm(R"(
    .text
    .globl ivictim
    .def ivictim; .scl 2; .type 32; .endef
    .seh_proc ivictim
ivictim:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    .globl ivictim_at
ivictim_at:
    movl %ecx, %eax
    addl $1, %eax
    .globl ivictim_rip
ivictim_rip:
    leaq ivictim_data(%rip), %rdx
    addl (%rdx), %eax
    .globl ivictim_jmp
ivictim_jmp:
    jmp 1f
    nop
    nop
    nop
1:
    .globl ivictim_imm
ivictim_imm:
    addl $100, %eax
    addq $0x28, %rsp
    ret
    .seh_endproc

    .globl ijump
    .def ijump; .scl 2; .type 32; .endef
    .seh_proc ijump
ijump:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    movl %ecx, %eax
    .globl ijump_at
ijump_at:
    nop
2:
    addl $1, %eax
    decl %ecx
    jg 2b
    addq $0x28, %rsp
    ret
    .seh_endproc

    .globl icall
    .def icall; .scl 2; .type 32; .endef
    .seh_proc icall
icall:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    .globl icall_at
icall_at:
    call ihelper
    addq $0x28, %rsp
    ret
    .seh_endproc
ihelper:
    leal (%rcx,%rcx), %eax
    ret

    .globl ijcc
    .def ijcc; .scl 2; .type 32; .endef
    .seh_proc ijcc
ijcc:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    .globl ijcc_at
ijcc_at:
    testl %ecx, %ecx
    jz 3f
    movl $5, %eax
    jmp 4f
3:
    movl $7, %eax
4:
    addq $0x28, %rsp
    ret
    .seh_endproc

    .globl icaller
    .def icaller; .scl 2; .type 32; .endef
    .seh_proc icaller
icaller:
    subq $0x28, %rsp
    .seh_stackalloc 0x28
    .seh_endprologue
    call ivictim
    .globl icaller_ret
icaller_ret:
    addq $0x28, %rsp
    ret
    .seh_endproc

    .data
ivictim_data:
    .long 10
    .text
)");

static std::string g_itrace;
static void add_rcx(FH2Context* c, void*) { c->rcx += 1000; }
static void add_rax(FH2Context* c, void* user) { c->rax += reinterpret_cast<uintptr_t>(user); }
static void mark_a(FH2Context*, void*) { g_itrace += "a"; }
static void mark_b(FH2Context*, void*) { g_itrace += "b"; }
static void crashes(FH2Context*, void*) { *static_cast<volatile int*>(nullptr) = 1; }
static void bump_xmm(FH2Context* c, void*) { c->xmm[0].f[0] += 1.0f; c->r12 = 7; }
// A fn built for AVX: its VEX instruction zeroes ymm1's upper half, which has to come back.
static void clobber_ymm(FH2Context*, void*) { asm volatile("vxorps %%xmm1, %%xmm1, %%xmm1" ::: "xmm1"); }
// A stack walk from inside a hook gets through the stub to the hooked function's caller.
static int g_walk = -1;
static void walk(FH2Context*, void*) {
    void* frames[32] = {};
    const USHORT n   = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    g_walk           = 0;
    for (USHORT i = 0; i < n; ++i)
        if (frames[i] == icaller_ret) g_walk = 1;
}

static void precise_tests(char* base) {
    if (g_api->api_version < 7) {
        check(false, "api_version >= 7");
        return;
    }
    unsigned char before[48];
    std::memcpy(before, reinterpret_cast<void*>(&ivictim), sizeof before);
    CHECK(ivictim(1) == 112);

    // An instruction hook changes a register before the instruction runs.
    FH2Hook* h1 = g_api->hook_instruction(g_a, "ivictim mov", ivictim_at, &add_rcx, nullptr);
    CHECK(h1 != nullptr);
    CHECK(ivictim(1) == 112); // made, but off
    CHECK(g_api->enable_hooks(g_a, h1) == 1);
    CHECK(static_cast<unsigned char>(ivictim_at[0]) == 0xE9);
    CHECK(ivictim(1) == 1112);
    // Two mods on the same instruction: newest first, and both run.
    FH2Hook* ha = g_api->hook_instruction(g_a, "mark a", ivictim_at, &mark_a, nullptr);
    FH2Hook* hb = g_api->hook_instruction(g_b, "mark b", ivictim_at, &mark_b, nullptr);
    CHECK(ha && hb);
    CHECK(g_api->enable_hooks(g_a, ha) == 1 && g_api->enable_hooks(g_b, hb) == 1);
    g_itrace.clear();
    CHECK(ivictim(1) == 1112 && g_itrace == "ba");
    CHECK(g_api->disable_hooks(g_b, hb) == 1);
    g_itrace.clear();
    CHECK(ivictim(1) == 1112 && g_itrace == "a");
    CHECK(g_api->disable_hooks(g_a, nullptr) == 1);
    CHECK(ivictim(1) == 112);
    CHECK(std::memcmp(before, reinterpret_cast<void*>(&ivictim), sizeof before) == 0); // bytes put back

    // Moved instructions: a rip-relative lea, and a short jmp.
    FH2Hook* h2 = g_api->hook_instruction(g_a, "ivictim lea", ivictim_rip, &add_rax, reinterpret_cast<void*>(5));
    FH2Hook* h3 = g_api->hook_instruction(g_a, "ivictim jmp", ivictim_jmp, &add_rax, reinterpret_cast<void*>(7));
    CHECK(h2 && h3);
    CHECK(g_api->enable_hooks(g_a, h2) == 1);
    {
        char b[160];
        const unsigned char* t = nullptr;
        std::memcpy(&t, &ivictim_rip, 0);
        std::snprintf(b, sizeof b, "ivictim(1) with the lea hook = %d; bytes at the lea %02X %02X %02X %02X %02X %02X %02X",
                      ivictim(1), (unsigned char)ivictim_rip[0], (unsigned char)ivictim_rip[1], (unsigned char)ivictim_rip[2],
                      (unsigned char)ivictim_rip[3], (unsigned char)ivictim_rip[4], (unsigned char)ivictim_rip[5],
                      (unsigned char)ivictim_rip[6]);
        g_api->log(g_a, b);
    }
    CHECK(ivictim(1) == 117);
    CHECK(g_api->enable_hooks(g_a, h3) == 1);
    CHECK(ivictim(1) == 124);
    // A crashing fn is skipped from then on; the code still runs.
    FH2Hook* hc = g_api->hook_instruction(g_b, "crashes", ivictim_rip, &crashes, nullptr);
    CHECK(hc && g_api->enable_hooks(g_b, hc) == 1);
    CHECK(ivictim(1) == 124);
    CHECK(ivictim(1) == 124);
    // Floats and callee-saved registers come back changed.
    FH2Hook* hx = g_api->hook_instruction(g_b, "xmm", ivictim_jmp, &bump_xmm, nullptr);
    CHECK(hx && g_api->enable_hooks(g_b, hx) == 1);
    {
        float f = 0;
        uint64_t r12 = 0;
        asm volatile("movss %2, %%xmm0\n\t"
                     "movl $1, %%ecx\n\t"
                     "call ivictim\n\t"
                     "movss %%xmm0, %0\n\t"
                     "movq %%r12, %1"
                     : "=m"(f), "=r"(r12)
                     : "m"(*(const float*)"\x00\x00\x20\x40") // 2.5f
                     : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
                       "xmm5", "memory");
        CHECK(f == 3.5f && r12 == 7);
    }
    CHECK(g_api->disable_hooks(g_a, nullptr) == 1 && g_api->disable_hooks(g_b, nullptr) == 1);
    CHECK(std::memcmp(before, reinterpret_cast<void*>(&ivictim), sizeof before) == 0);

    // The upper halves of the ymm registers survive a fn that uses AVX.
    if (IsProcessorFeaturePresent(39)) {
        FH2Hook* hy = g_api->hook_instruction(g_b, "ymm", ivictim_jmp, &clobber_ymm, nullptr);
        CHECK(hy && g_api->enable_hooks(g_b, hy) == 1);
        alignas(32) static float in[8] = {1, 2, 3, 4, 5, 6, 7, 8}, out[8];
        asm volatile("vmovups %0, %%ymm1\n\t"
                     "movl $1, %%ecx\n\t"
                     "call ivictim\n\t"
                     "vmovups %%ymm1, %1\n\t"
                     "vzeroupper"
                     : : "m"(in), "m"(out)
                     : "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                       "memory");
        CHECK(out[4] == 5 && out[7] == 8);
        CHECK(g_api->disable_hooks(g_b, hy) == 1);
    }

    // Stack walks through a hook's stub.
    FH2Hook* hw = g_api->hook_instruction(g_a, "walk", ivictim_at, &walk, nullptr);
    CHECK(hw && g_api->enable_hooks(g_a, hw) == 1);
    CHECK(icaller(1) == 112 && g_walk == 1);
    CHECK(g_api->disable_hooks(g_a, hw) == 1);

    // Moved CALL rel32 and Jcc.
    FH2Hook* hcall = g_api->hook_instruction(g_a, "icall call", icall_at, &mark_a, nullptr);
    FH2Hook* hjcc  = g_api->hook_instruction(g_a, "ijcc test+jz+mov", ijcc_at, &mark_b, nullptr);
    CHECK(hcall && hjcc && g_api->enable_hooks(g_a, hcall) == 1 && g_api->enable_hooks(g_a, hjcc) == 1);
    g_itrace.clear();
    CHECK(icall(4) == 8 && ijcc(0) == 7 && ijcc(3) == 5 && g_itrace == "abb");
    CHECK(g_api->disable_hooks(g_a, hcall) == 1 && g_api->disable_hooks(g_a, hjcc) == 1);

    // Places it refuses.
    CHECK(g_api->hook_instruction(g_a, "prolog", reinterpret_cast<void*>(&ivictim), &mark_a, nullptr) == nullptr);
    CHECK(g_api->hook_instruction(g_a, "mid-instruction", ivictim_at + 1, &mark_a, nullptr) == nullptr);
    CHECK(g_api->hook_instruction(g_a, "jumped into", ijump_at, &mark_a, nullptr) == nullptr);
    CHECK(ijump(3) == 6);

    // Byte patches: 100 -> 50, checked, overlapping refused, put back.
    const unsigned char want = 100, now = 50, wrong = 7;
    CHECK(g_api->patch_bytes(g_a, "wrong expect", ivictim_imm + 2, &wrong, &now, 1) == nullptr);
    FH2Hook* p1 = g_api->patch_bytes(g_a, "ivictim 100 -> 50", ivictim_imm + 2, &want, &now, 1);
    CHECK(p1 != nullptr);
    CHECK(g_api->patch_bytes(g_b, "same byte", ivictim_imm + 2, nullptr, &now, 1) == nullptr);
    CHECK(g_api->hook_instruction(g_b, "over a patch", ivictim_imm, &mark_b, nullptr) == nullptr);
    CHECK(ivictim(1) == 112);
    CHECK(g_api->enable_hooks(g_a, p1) == 1);
    CHECK(ivictim(1) == 62);
    CHECK(g_api->disable_hooks(g_a, p1) == 1);
    CHECK(ivictim(1) == 112);

    // The game (RadioStreamFmod::load, around the createSound call another test hooks): instruction
    // hooks that move a lea+mov, a CALL rel32 and a test+je; a patch checked against the exe on disk.
    char* const fn_code = base + 0x1018A06;
    unsigned char game_before[0x40];
    std::memcpy(game_before, fn_code, sizeof game_before);
    const int offs[] = {0x1018A06, 0x1018A1D, 0x1018A25};
    FH2Hook* gi[3]   = {};
    for (int i = 0; i < 3; ++i) {
        gi[i] = g_api->hook_instruction(g_a, "RadioStreamFmod::load", base + offs[i], &mark_a, nullptr);
        CHECK(gi[i] != nullptr);
    }
    CHECK(g_api->enable_hooks(g_a, nullptr) == 1);
    CHECK(static_cast<unsigned char>(base[0x1018A06]) == 0xE9 && static_cast<unsigned char>(base[0x1018A1D]) == 0xE9 &&
          static_cast<unsigned char>(base[0x1018A25]) == 0xE9);
    CHECK(g_api->disable_hooks(g_a, nullptr) == 1);
    CHECK(std::memcmp(game_before, fn_code, sizeof game_before) == 0);
    const unsigned char d68 = 0x68, d70 = 0x70;
    FH2Hook* gp = g_api->patch_bytes(g_b, "game byte", base + 0x1018A2C, &d68, &d70, 1);
    CHECK(gp && g_api->enable_hooks(g_b, gp) == 1 && static_cast<unsigned char>(base[0x1018A2C]) == 0x70);
    CHECK(gp && g_api->disable_hooks(g_b, gp) == 1 && static_cast<unsigned char>(base[0x1018A2C]) == 0x68);
}

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

    // Another game (Azimuth 1.0.1 runs more than FH2): told straight away, and nothing goes into it.
    if (GetFileAttributesW(L"other_game.txt") != INVALID_FILE_ATTRIBUTES) {
        const ULONGLONG t0 = GetTickCount64();
        CHECK(g_api->wait_for_game(30000) == nullptr);
        CHECK(g_api->wait_for_code(60000) == nullptr);
        CHECK(GetTickCount64() - t0 < 5000);
        CHECK(g_api->game_base() == nullptr);
        char* other = reinterpret_cast<char*>(0x140000000);
        CHECK(g_api->hook_function(g_a, "other game's code", other + 0x1000, reinterpret_cast<void*>(&dummy_detour),
                                   &orig_dummy) == nullptr);
        CHECK(g_api->hook_pointer(g_a, "other game's pointer", reinterpret_cast<void**>(other + 0x2000),
                                  reinterpret_cast<void*>(&dummy_detour), &orig_dummy) == nullptr);
        static const unsigned char nop = 0x90;
        CHECK(g_api->patch_bytes(g_a, "other game's bytes", other + 0x1000, nullptr, &nop, 1) == nullptr);
        CHECK(g_api->hook_instruction(g_a, "other game's instruction", other + 0x1000, &mark_a, nullptr) == nullptr);
        char done[64];
        std::snprintf(done, sizeof done, "hook tests done, %d failed", g_fails);
        g_api->log(g_a, done);
        return 0;
    }

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

    precise_tests(base);

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

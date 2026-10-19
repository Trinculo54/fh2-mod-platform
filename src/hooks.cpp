// Hooks: one place where every mod changes game code (API 3).
//
// Three kinds, all ending up in the same chain:
//   function  an inline detour at the start of a function, made with MinHook
//   call      a CALL rel32 (or JMP rel32) at one call site, retargeted
//   pointer   a function pointer in memory (a vtable slot, a callback table)
//
// Every patched place is a Site. The game is sent to the Site's entry stub,
// a "jmp [slot]" whose slot holds the newest enabled hook's detour. Each
// hook's `original` is a stub of its own, pointing at the next older hook's
// detour, and the oldest one's at the real code (MinHook's trampoline, the
// call's old target, or the pointer's old value). So several mods can hook
// the same function, and any of them can come and go in any order: only
// 8-byte slots change, atomically, and nothing a running thread may be in
// is ever freed.
//
// Before anything is written, the bytes are checked: against every other
// hook's (no overlaps), and for game code against the exe on disk, so code
// that Extra Options (or a mod patching the game by itself) has already
// changed is never hooked over. Hooks in game code also wait for Extra
// Options to finish patching (wait_for_code): it validates all of its sites
// before writing any, and refuses every patch if one doesn't match.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include "MinHook.h"
#include "hde/hde64.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FH2Hook;

namespace fh2platform {
namespace {

namespace fs = std::filesystem;

enum Kind { kFunction, kCall, kPointer };
const char* const kKindName[] = {"function", "call", "pointer"};

std::mutex g_mu;

// ---- small helpers ---------------------------------------------------------------

bool query(const void* p, MEMORY_BASIC_INFORMATION& mbi) {
    return VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!query(p, mbi)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

bool executable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    return query(p, mbi) &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

const char* game_base() { return static_cast<const char*>(find_game()); }

bool in_game(const void* p) {
    const char* g = game_base();
    const char* c = static_cast<const char*>(p);
    return g && c >= g && c < g + kGameSizeOfImage;
}

std::string hex(uintptr_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// "game+0xA68870", "WheresMyBuddy.dll+0x1234" or "0x7FF6...".
std::string where(const void* p) {
    if (!p) return "null";
    if (in_game(p)) return "game+" + hex(static_cast<const char*>(p) - game_base());
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(p), &m)) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
        if (n && n < MAX_PATH)
            return narrow(fs::path{path}.filename().wstring()) + "+" +
                   hex(static_cast<const char*>(p) - reinterpret_cast<const char*>(m));
    }
    return hex(reinterpret_cast<uintptr_t>(p));
}

// ---- stubs -------------------------------------------------------------------------
//
// A block is two pages: code (read/execute) with 512 stubs of
// "jmp [rip+0xFFA]; int3; int3", each reading the qword at the same index
// in the data page (read/write) right after it.

struct Stub {
    void* code = nullptr;
    void* volatile* slot = nullptr;
};

constexpr std::size_t kPage = 0x1000;
constexpr int kStubsPerBlock = kPage / 8;
constexpr int kMaxBlocks     = 64;

struct Block {
    char* code;
    int used;
};
Block g_blocks[kMaxBlocks];
int g_nblocks = 0;

// Free memory within +-1 GB of `close_to` (anywhere if null), so a rel32 from
// there reaches it.
char* alloc_near(const void* close_to, std::size_t size) {
    if (!close_to)
        return static_cast<char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran   = si.dwAllocationGranularity;
    const uintptr_t origin = reinterpret_cast<uintptr_t>(close_to);
    const uintptr_t range  = 0x40000000;
    const uintptr_t lo     = std::max<uintptr_t>(origin > range ? origin - range : 0,
                                                 reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress));
    const uintptr_t hi     = std::min<uintptr_t>(origin + range,
                                                 reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress));
    MEMORY_BASIC_INFORMATION mbi{};
    // Upwards first (past the game image is usually free), then down.
    for (uintptr_t a = (origin + gran - 1) / gran * gran; a + size < hi;) {
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) != sizeof mbi) break;
        if (mbi.State == MEM_FREE)
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<char*>(p);
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        a = std::max(a + gran, (next + gran - 1) / gran * gran);
    }
    for (uintptr_t a = origin / gran * gran; a > lo + gran;) {
        a -= gran;
        if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) != sizeof mbi) break;
        if (mbi.State == MEM_FREE) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))
                return static_cast<char*>(p);
        } else if (mbi.AllocationBase) {
            a = reinterpret_cast<uintptr_t>(mbi.AllocationBase) / gran * gran;
        }
    }
    return nullptr;
}

bool reaches(const void* from, const void* to) {
    const auto d = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
    return d > -0x7FFF0000LL && d < 0x7FFF0000LL;
}

Stub new_stub(const void* close_to) {
    Block* b = nullptr;
    for (int i = 0; i < g_nblocks && !b; ++i)
        if (g_blocks[i].used < kStubsPerBlock && (!close_to || reaches(close_to, g_blocks[i].code))) b = &g_blocks[i];
    if (!b) {
        if (g_nblocks == kMaxBlocks) return {};
        char* mem = alloc_near(close_to, 2 * kPage);
        if (!mem) return {};
        static const unsigned char kStub[8] = {0xFF, 0x25, 0xFA, 0x0F, 0x00, 0x00, 0xCC, 0xCC};
        for (int i = 0; i < kStubsPerBlock; ++i) std::memcpy(mem + 8 * i, kStub, 8);
        DWORD old = 0;
        VirtualProtect(mem, kPage, PAGE_EXECUTE_READ, &old);
        FlushInstructionCache(GetCurrentProcess(), mem, kPage);
        b  = &g_blocks[g_nblocks];
        *b = {mem, 0};
        ++g_nblocks; // published after the block is ready, for crash reports
    }
    const int i = b->used++;
    return {b->code + 8 * i, reinterpret_cast<void* volatile*>(b->code + kPage + 8 * i)};
}

void set_slot(void* volatile* slot, void* v) { InterlockedExchangePointer(const_cast<void**>(slot), v); }

// ---- the game exe on disk ----------------------------------------------------------

struct DiskImage {
    bool tried = false;
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t image_base = 0;
    std::vector<IMAGE_SECTION_HEADER> sections;
} g_disk;

void open_disk_image() {
    if (g_disk.tried) return;
    g_disk.tried = true;
    std::vector<fs::path> candidates;
    if (!game_mount().empty()) candidates.push_back(game_mount() / L"forza_durango_release_final.exe");
    candidates.push_back(azimuth_dir() / L"forza_durango_release_final.exe"); // Mezcal
    for (const auto& c : candidates) {
        HANDLE f = CreateFileW(c.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        char head[0x1000];
        DWORD got = 0;
        bool ok   = ReadFile(f, head, sizeof head, &got, nullptr) && got == sizeof head;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head);
        const auto* nt  = ok && dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew < 0x800
                              ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(head + dos->e_lfanew)
                              : nullptr;
        ok = nt && nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.SizeOfImage == kGameSizeOfImage;
        if (ok) {
            const auto* sec = IMAGE_FIRST_SECTION(nt);
            const unsigned n = nt->FileHeader.NumberOfSections;
            ok = reinterpret_cast<const char*>(sec + n) <= head + sizeof head;
            if (ok) {
                g_disk.sections.assign(sec, sec + n);
                g_disk.image_base = nt->OptionalHeader.ImageBase;
                g_disk.file       = f;
                plog("hooks: checking game code against " + narrow(c.wstring()));
                return;
            }
        }
        CloseHandle(f);
    }
    plog("hooks: couldn't read the game exe from the game folder, so hooks can't check whether game code "
         "was already changed outside the platform");
}

// Every page of the exe's executable sections is executable in memory. True when the sections
// aren't known (the exe couldn't be read): there's nothing to check against.
bool code_sections_executable(std::string* first_bad) {
    const char* base = game_base();
    if (!base || g_disk.sections.empty()) return true;
    for (const auto& sec : g_disk.sections) {
        if (!(sec.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const char* p   = base + sec.VirtualAddress;
        const char* end = p + sec.Misc.VirtualSize;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) return false;
            const bool exec = mbi.State == MEM_COMMIT &&
                              (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
            if (!exec) {
                if (first_bad) *first_bad = where(p) + " isn't executable yet";
                return false;
            }
            p = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
        }
    }
    return true;
}

// 1 if the game's bytes at [p, p+n) are as the exe on disk has them, 0 if
// not, -1 if that can't be told. `pointer` compares one 8-byte pointer,
// allowing for the image being mapped somewhere other than its preferred
// base.
int matches_disk(const void* p, std::size_t n, bool pointer) {
    open_disk_image();
    if (g_disk.file == INVALID_HANDLE_VALUE) return -1;
    const char* base = game_base();
    const auto rva   = static_cast<uint32_t>(static_cast<const char*>(p) - base);
    for (const auto& s : g_disk.sections) {
        if (rva < s.VirtualAddress || rva + n > s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData))
            continue;
        if (rva + n > s.VirtualAddress + s.SizeOfRawData) return -1; // not in the file (zero-filled)
        unsigned char disk[32];
        if (n > sizeof disk) return -1;
        OVERLAPPED ov{};
        const uint64_t off = s.PointerToRawData + (rva - s.VirtualAddress);
        ov.Offset          = static_cast<DWORD>(off);
        ov.OffsetHigh      = static_cast<DWORD>(off >> 32);
        DWORD got          = 0;
        if (!ReadFile(g_disk.file, disk, static_cast<DWORD>(n), &got, &ov) || got != n) return -1;
        if (pointer) {
            // Only a pointer that's an address in the image on disk (a vtable
            // slot, a callback table) can be compared. Imports hold name
            // RVAs on disk and are filled in by the loader, and many tables
            // are zero until the game fills them.
            uint64_t want = 0, have = 0;
            std::memcpy(&want, disk, 8);
            std::memcpy(&have, p, 8);
            if (want < g_disk.image_base || want >= g_disk.image_base + kGameSizeOfImage) return -1;
            return have == want + (reinterpret_cast<uint64_t>(base) - g_disk.image_base) ? 1 : 0;
        }
        if (reinterpret_cast<uint64_t>(base) != g_disk.image_base) return -1; // relocated code differs anyway
        return std::memcmp(disk, p, n) == 0 ? 1 : 0;
    }
    return -1;
}

// ---- waiting for Extra Options -----------------------------------------------------

std::atomic<bool> g_code_ready{false};
constexpr ULONGLONG kExtraOptionsTimeout = 20000;

FILETIME process_start() {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return c;
}

// Extra Options patches the game while Azimuth loads its TLS helper, right
// after mapping it. Once patch_engine_apply is done, it rewrites
// FH2_ExtraOptions.log (CREATE_ALWAYS) with its status on the second line:
// "OK: ...", "REFUSED: ..." or "DISABLED: ...", and under Azimuth then adds
// "AZIMUTH: winmm proxy initialized". Before that, it writes nothing this
// launch. Returns "" while it's still to come, or what settled it.
std::string extra_options_state() {
    HMODULE w = GetModuleHandleW(L"winmm.dll");
    if (!w) return "no Extra Options";
    wchar_t path[MAX_PATH], sys[MAX_PATH];
    const DWORD n = GetModuleFileNameW(w, path, MAX_PATH);
    const UINT s  = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH) return "no Extra Options";
    const fs::path dir = fs::path{path}.parent_path();
    if (s && s < MAX_PATH && lstrcmpiW(dir.c_str(), sys) == 0) return "no Extra Options";
    const fs::path log = dir / L"FH2_ExtraOptions.log";
    HANDLE f = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string text;
    FILETIME written{};
    const FILETIME started = process_start();
    if (GetFileTime(f, nullptr, nullptr, &written) && CompareFileTime(&written, &started) >= 0) {
        text.resize(64 * 1024);
        DWORD got = 0;
        if (ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) text.resize(got);
        else text.clear();
    }
    CloseHandle(f);
    if (text.find("\nAZIMUTH: winmm proxy initialized") != std::string::npos)
        return "Extra Options has applied its patches";
    const auto nl = text.find('\n');
    const std::string status = nl == std::string::npos ? std::string{} : text.substr(nl + 1, 10);
    if (status.rfind("OK:", 0) == 0) return "Extra Options has applied its patches";
    if (status.rfind("REFUSED:", 0) == 0 || status.rfind("DISABLED:", 0) == 0)
        return "Extra Options made no patches this time";
    return "";
}

bool code_ready() { return g_code_ready.load(std::memory_order_acquire); }

void open_disk_image();
bool code_sections_executable(std::string* first_bad);

// Azimuth maps the game as plain memory and only marks its code executable afterwards; Extra
// Options can be done before that. Hooking in between fails ("it isn't code"), which happened
// once: six of WreckingBall's hooks and WheresMyBuddy's within a few milliseconds of "ready".
// So "ready" also waits until every executable section of the exe on disk is executable here.
void wait_for_executable_code() {
    {
        std::scoped_lock lk{g_mu};
        open_disk_image();
    }
    const ULONGLONG t0 = GetTickCount64();
    std::string bad;
    while (!code_sections_executable(&bad)) {
        if (GetTickCount64() - t0 >= 10000) {
            plog("hooks: the game's code still isn't all executable after 10 seconds (" + bad + "); going ahead");
            return;
        }
        Sleep(10);
    }
    if (const ULONGLONG waited = GetTickCount64() - t0; waited >= 10)
        plog("hooks: waited " + std::to_string(waited) + " ms for the game's code to be marked executable");
}

// Watches for the game being mapped, then for Extra Options to finish.
void watch_for_code() {
    std::thread([] {
        while (!find_game()) Sleep(50);
        const ULONGLONG seen = GetTickCount64();
        std::string why;
        while ((why = extra_options_state()).empty()) {
            if (GetTickCount64() - seen >= kExtraOptionsTimeout) {
                why = "Extra Options didn't say it had finished within 20 seconds of the game being mapped; "
                      "going ahead";
                break;
            }
            Sleep(50);
        }
        wait_for_executable_code();
        plog("hooks: game code is ready (" + why + ")");
        g_code_ready.store(true, std::memory_order_release);
    }).detach();
}

// ---- records, for crash reports ------------------------------------------------------
// Plain fixed-size data, readable from the crash handler.

struct Record {
    char text[160]; // "WheresMyBuddy: pointer game+0x2D44F30 (ChampionshipDrivatarsState::Serialize)"
    uintptr_t tramp;
    volatile LONG enabled;
};
constexpr int kMaxRecords = 256;
Record g_records[kMaxRecords];
volatile LONG g_nrecords = 0;

// ---- sites -------------------------------------------------------------------------

struct Site {
    Kind kind;
    char* at;            // function start, call instruction, or pointer
    void* original;      // trampoline, the call's old target, the pointer's old value
    Stub entry;
    char* lo;            // bytes this site changes or depends on
    char* hi;
    std::vector<unsigned char> before; // [lo, hi) before the first patch
    bool live = false;
    std::vector<FH2Hook*> chain; // in the order they were last enabled
};
std::vector<std::unique_ptr<Site>> g_sites;
bool g_minhook = false;

} // namespace
} // namespace fh2platform

struct FH2Hook {
    FH2Mod* mod;
    std::string name;
    fh2platform::Site* site;
    void* detour;
    fh2platform::Stub next;
    bool enabled = false;
    int record   = -1;
};

namespace fh2platform {
namespace {

std::vector<std::unique_ptr<FH2Hook>> g_hooks;

std::string describe(const FH2Hook* h) {
    return std::string(kKindName[h->site->kind]) + " " + where(h->site->at) +
           (h->name.empty() ? "" : " (" + h->name + ")");
}

bool is_padding(const unsigned char* p, std::size_t n) {
    if (p[0] != 0x00 && p[0] != 0x90 && p[0] != 0xCC) return false;
    for (std::size_t i = 1; i < n; ++i)
        if (p[i] != p[0]) return false;
    return true;
}

// The bytes MinHook will copy into the trampoline or overwrite, as it
// decides them (trampoline.c): instructions up to 5 bytes, or up to a return
// or jump out, with the jump written over padding after it or above the
// function. False if MinHook couldn't hook it.
bool function_range(const unsigned char* t, char** lo, char** hi) {
    const auto start = reinterpret_cast<uintptr_t>(t);
    std::size_t pos  = 0;
    uintptr_t jmp_dest = 0; // the furthest jump target inside the first 5 bytes
    bool finished      = false;
    do {
        if (!readable(t + pos, 16)) return false;
        hde64s hs{};
        hde64_disasm(t + pos, &hs);
        if (hs.flags & F_ERROR) return false;
        if (pos >= 5) break; // long enough: MinHook jumps back here, this instruction isn't copied
        const uintptr_t inst = start + pos;
        bool resized         = false; // MinHook rewrites it as a longer, absolute form
        if ((hs.modrm & 0xC7) == 0x05) {
            if (hs.opcode == 0xFF && hs.modrm_reg == 4) finished = true; // jmp [rip+x]
        } else if (hs.opcode == 0xE8) {
            resized = true;
        } else if ((hs.opcode & 0xFD) == 0xE9) {
            const uintptr_t dest = inst + hs.len + (hs.opcode == 0xEB ? static_cast<int8_t>(hs.imm.imm8)
                                                                      : static_cast<int32_t>(hs.imm.imm32));
            if (start <= dest && dest < start + 5) {
                jmp_dest = std::max(jmp_dest, dest);
            } else {
                resized  = true;
                finished = inst >= jmp_dest;
            }
        } else if ((hs.opcode & 0xF0) == 0x70 || (hs.opcode & 0xFC) == 0xE0 || (hs.opcode2 & 0xF0) == 0x80) {
            const bool short_form = (hs.opcode & 0xF0) == 0x70 || (hs.opcode & 0xFC) == 0xE0;
            const uintptr_t dest  = inst + hs.len + (short_form ? static_cast<int8_t>(hs.imm.imm8)
                                                                : static_cast<int32_t>(hs.imm.imm32));
            if (start <= dest && dest < start + 5) jmp_dest = std::max(jmp_dest, dest);
            else if ((hs.opcode & 0xFC) == 0xE0) return false; // LOOP/JECXZ out: MinHook can't
            else resized = true;
        } else if ((hs.opcode & 0xFE) == 0xC2) {
            finished = inst >= jmp_dest;
        }
        if (inst < jmp_dest && resized) return false;
        pos += hs.len;
    } while (!finished);
    auto* c = reinterpret_cast<char*>(const_cast<unsigned char*>(t));
    if (pos < 5 && !is_padding(t + pos, 5 - pos)) {
        // Too short: a short jmp at the start, to a long one in the padding above it.
        if (pos < 2 && !is_padding(t + pos, 2 - pos)) return false;
        if (!executable(t - 5) || !is_padding(t - 5, 5)) return false;
        *lo = c - 5;
        *hi = c + std::max<std::size_t>(pos, 2);
    } else {
        *lo = c;
        *hi = c + std::max<std::size_t>(pos, 5);
    }
    return true;
}

Site* find_site(Kind k, const void* at) {
    for (auto& s : g_sites)
        if (s->kind == k && s->at == at) return s.get();
    return nullptr;
}

// A hook of another kind (or another place) whose bytes overlap these.
const Site* overlapping(const char* lo, const char* hi) {
    for (auto& s : g_sites)
        if (lo < s->hi && s->lo < hi) return s.get();
    return nullptr;
}

std::string owners(const Site* s) {
    std::string out;
    for (const FH2Hook* h : s->chain)
        if (out.find(h->mod->name) == std::string::npos) out += (out.empty() ? "" : ", ") + h->mod->name;
    return out.empty() ? "a hook" : out;
}

bool check_bytes(FH2Mod* m, const char* what, char* lo, char* hi, bool pointer) {
    if (const Site* o = overlapping(lo, hi)) {
        log_line(m->name, std::string("can't hook ") + what + ": it overlaps " + owners(o) + "'s " +
                              kKindName[o->kind] + " hook at " + where(o->at));
        return false;
    }
    if (in_game(lo) && matches_disk(lo, static_cast<std::size_t>(hi - lo), pointer) == 0) {
        log_line(m->name, std::string("can't hook ") + what +
                              ": that game code was already changed outside the platform (by Extra Options, or "
                              "a mod that patches the game by itself)");
        return false;
    }
    return true;
}

bool common_checks(FH2Mod* m, const char* what, void* at, void* detour, void** original) {
    if (!m) return false;
    if (!at || !detour || !original) {
        log_line(m->name, std::string("can't hook ") + what + ": null address, detour or original");
        return false;
    }
    if (in_game(at) && !code_ready()) {
        log_line(m->name, std::string("can't hook ") + what +
                              " yet: call wait_for_code first, so Extra Options can patch the game before you do");
        return false;
    }
    if (!executable(detour)) {
        log_line(m->name, std::string("can't hook ") + what + ": the detour isn't code");
        return false;
    }
    return true;
}

FH2Hook* add_hook(FH2Mod* m, const char* name, Site* s, void* detour, void** original) {
    auto h    = std::make_unique<FH2Hook>();
    h->mod    = m;
    h->name   = name ? name : "";
    h->site   = s;
    h->detour = detour;
    h->next   = new_stub(s->at);
    if (!h->next.code) {
        log_line(m->name, "can't hook " + where(s->at) + ": out of memory for hook stubs");
        return nullptr;
    }
    set_slot(h->next.slot, s->original);
    *original = h->next.code;
    FH2Hook* raw = h.get();
    s->chain.push_back(raw);
    g_hooks.push_back(std::move(h));
    if (g_nrecords < kMaxRecords) {
        Record& r = g_records[g_nrecords];
        std::snprintf(r.text, sizeof r.text, "%s: %s", m->name.c_str(), describe(raw).c_str());
        r.tramp   = s->kind == kFunction ? reinterpret_cast<uintptr_t>(s->original) : 0;
        r.enabled = 0;
        raw->record = g_nrecords;
        InterlockedIncrement(&g_nrecords);
    }
    log_line(m->name, "hooked " + describe(raw) +
                          (s->chain.size() > 1 ? " (shared with " + owners(s) + ")" : ""));
    return raw;
}

Site* new_site(Kind k, char* at, void* original, char* lo, char* hi) {
    auto s      = std::make_unique<Site>();
    s->kind     = k;
    s->at       = at;
    s->original = original;
    s->lo       = lo;
    s->hi       = hi;
    s->before.assign(lo, hi);
    Site* raw = s.get();
    g_sites.push_back(std::move(s));
    return raw;
}

// ---- writing patches ------------------------------------------------------------------

// For a rel32 that straddles a 16-byte boundary, which no single atomic
// write covers: pause every other thread (as MinHook does), make sure none
// of them is on the instruction, write, and let them go. Nothing may
// allocate or lock while they're paused, so the list is made first.
bool frozen_write(char* site, int32_t rel) {
    std::vector<HANDLE> threads;
    threads.reserve(256);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID))
                threads.push_back(h);
    CloseHandle(snap);
    const auto lo = reinterpret_cast<DWORD64>(site), hi = lo + 5;
    bool done = false;
    for (int attempt = 0; attempt < 100 && !done; ++attempt) {
        for (HANDLE h : threads) SuspendThread(h);
        bool clear = true;
        for (HANDLE h : threads) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Rip > lo && ctx.Rip < hi) clear = false;
        }
        if (clear) {
            std::memcpy(site + 1, &rel, 4);
            done = true;
        }
        for (HANDLE h : threads) ResumeThread(h);
        if (!done) Sleep(1);
    }
    for (HANDLE h : threads) CloseHandle(h);
    return done;
}

// Atomically rewrites the rel32 of the 5-byte CALL/JMP at `site`.
bool write_rel32(char* site, int32_t rel) {
    DWORD old = 0;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok             = false;
    const auto addr     = reinterpret_cast<uintptr_t>(site + 1);
    const auto a8       = addr & ~uintptr_t{7};
    const auto a16      = addr & ~uintptr_t{15};
    if (addr - a8 <= 4) {
        auto* q = reinterpret_cast<volatile LONG64*>(a8);
        for (int i = 0; i < 64 && !ok; ++i) {
            LONG64 cur = *q, nxt = cur;
            std::memcpy(reinterpret_cast<char*>(&nxt) + (addr - a8), &rel, 4);
            ok = InterlockedCompareExchange64(q, nxt, cur) == cur;
        }
    } else if (addr - a16 <= 12) {
        auto* q = reinterpret_cast<LONG64*>(a16);
        for (int i = 0; i < 64 && !ok; ++i) {
            alignas(16) LONG64 cur[2] = {q[0], q[1]};
            LONG64 nxt[2]             = {cur[0], cur[1]};
            std::memcpy(reinterpret_cast<char*>(nxt) + (addr - a16), &rel, 4);
#ifdef _MSC_VER
            ok = _InterlockedCompareExchange128(q, nxt[1], nxt[0], cur) != 0;
#else
            unsigned __int128 c, x;
            std::memcpy(&c, cur, 16);
            std::memcpy(&x, nxt, 16);
            ok = __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128*>(q), c, x);
#endif
        }
    }
    if (!ok) ok = frozen_write(site, rel);
    DWORD ignored = 0;
    VirtualProtect(site, 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return ok;
}

bool write_pointer(void** slot, void* expect, void* value) {
    // Keep the page executable if it was: the slot may share it with code.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!query(slot, mbi)) return false;
    const DWORD cur      = mbi.Protect & 0xFF;
    const bool writable  = cur == PAGE_READWRITE || cur == PAGE_EXECUTE_READWRITE;
    const bool exec      = cur == PAGE_EXECUTE_READ || cur == PAGE_EXECUTE_READWRITE || cur == PAGE_EXECUTE_WRITECOPY ||
                      cur == PAGE_EXECUTE;
    DWORD old = 0;
    if (!writable && !VirtualProtect(slot, sizeof(void*), exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old))
        return false;
    const bool ok = InterlockedCompareExchangePointer(slot, value, expect) == expect;
    DWORD ignored = 0;
    if (!writable) VirtualProtect(slot, sizeof(void*), old, &ignored);
    if (exec) FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return ok;
}

// What a site has to do once its chain has changed.
enum class Change { none, patch, unpatch };

// Rewires the stubs of `s` for its enabled hooks, and says whether the
// patch has to go in or come out.
Change rewire(Site* s) {
    std::vector<FH2Hook*> on;
    for (FH2Hook* h : s->chain)
        if (h->enabled) on.push_back(h);
        else set_slot(h->next.slot, s->original); // an off hook's `original` skips every detour
    if (on.empty()) {
        set_slot(s->entry.slot, s->original);
        return s->live ? Change::unpatch : Change::none;
    }
    // Inner links first, then the entry, so a thread already in a detour
    // always finds a complete chain.
    for (std::size_t i = 0; i < on.size(); ++i) set_slot(on[i]->next.slot, i ? on[i - 1]->detour : s->original);
    set_slot(s->entry.slot, on.back()->detour);
    return s->live ? Change::none : Change::patch;
}

// Puts the patch of a call or pointer site in or out. Function sites are
// done through MinHook's queue by the caller.
bool apply_simple(Site* s, bool in) {
    if (s->kind == kCall) {
        void* to = in ? s->entry.code : s->original;
        return write_rel32(s->at, static_cast<int32_t>(static_cast<char*>(to) - (s->at + 5)));
    }
    void** slot = reinterpret_cast<void**>(s->at);
    return in ? write_pointer(slot, s->original, s->entry.code) : write_pointer(slot, s->entry.code, s->original);
}

void set_records(Site* s) {
    for (FH2Hook* h : s->chain)
        if (h->record >= 0) g_records[h->record].enabled = h->enabled && s->live;
}

// Enables or disables `hook`, or every hook of `m` if it's null.
int switch_hooks(FH2Mod* m, FH2Hook* hook, bool on) {
    if (!m) return 0;
    std::scoped_lock lk{g_mu};
    if (hook && std::none_of(g_hooks.begin(), g_hooks.end(), [&](const auto& h) { return h.get() == hook; })) {
        log_line(m->name, "can't switch a hook: that's not a hook handle");
        return 0;
    }
    std::vector<FH2Hook*> mine;
    for (auto& h : g_hooks)
        if (h->mod == m && (!hook || h.get() == hook) && h->enabled != on) mine.push_back(h.get());
    if (hook && hook->mod != m) {
        log_line(m->name, "can't switch another mod's hook");
        return 0;
    }
    if (hook && mine.empty()) return 1; // already that way
    std::vector<Site*> sites;
    for (FH2Hook* h : mine) {
        h->enabled = on;
        if (on) {
            // The most recently enabled detour runs first: move it to the end.
            auto& ch = h->site->chain;
            ch.erase(std::find(ch.begin(), ch.end(), h));
            ch.push_back(h);
        }
        if (std::find(sites.begin(), sites.end(), h->site) == sites.end()) sites.push_back(h->site);
    }
    int ok = 1;
    std::vector<Site*> queued;
    for (Site* s : sites) {
        const Change c = rewire(s);
        if (c == Change::none) continue;
        const bool in = c == Change::patch;
        if (in && std::memcmp(s->before.data(), s->lo, s->before.size()) != 0) {
            log_line(m->name, "can't enable the hook at " + where(s->at) +
                                  ": the code there changed after it was hooked");
            for (FH2Hook* h : s->chain)
                if (h->mod == m && std::find(mine.begin(), mine.end(), h) != mine.end()) h->enabled = false;
            rewire(s);
            ok = 0;
            continue;
        }
        if (s->kind == kFunction) {
            const MH_STATUS st = (in ? MH_QueueEnableHook : MH_QueueDisableHook)(s->at);
            if (st == MH_OK) {
                queued.push_back(s);
            } else {
                log_line(m->name, "MinHook couldn't queue " + where(s->at) + ": " + MH_StatusToString(st));
                ok = 0;
            }
        } else if (apply_simple(s, in)) {
            s->live = in;
        } else {
            log_line(m->name, std::string("couldn't ") + (in ? "patch " : "unpatch ") + where(s->at));
            ok = 0;
        }
    }
    if (!queued.empty()) {
        // One pass: MinHook suspends the other threads once for all of them.
        const MH_STATUS st = MH_ApplyQueued();
        if (st != MH_OK) {
            log_line(m->name, std::string("MinHook couldn't apply all the hooks: ") + MH_StatusToString(st));
            ok = 0;
        }
        // MinHook stops at the first failure, keeping what it did before it.
        // Go by the bytes, and leave nothing queued for the next caller.
        for (Site* s : queued) {
            const bool was = s->live;
            s->live        = std::memcmp(s->before.data(), s->lo, s->before.size()) != 0;
            (s->live ? MH_QueueEnableHook : MH_QueueDisableHook)(s->at);
            if (s->live == was) ok = 0;
        }
    }
    for (Site* s : sites) {
        set_records(s);
        if (!s->live)
            for (FH2Hook* h : s->chain)
                if (h->enabled) {
                    // Couldn't go live: put the chain back as it is.
                    h->enabled = false;
                }
        if (!s->live) rewire(s);
    }
    int count = 0;
    for (FH2Hook* h : mine) count += h->enabled == on;
    if (count)
        log_line(m->name, std::string(on ? "enabled " : "disabled ") + std::to_string(count) +
                              (count == 1 ? " hook" : " hooks") +
                              (hook ? " (" + describe(hook) + ")" : std::string{}));
    return ok;
}

// ---- the API -------------------------------------------------------------------------

// A target in the game that isn't executable yet gets up to 2 seconds (see wait_for_executable_code).
void settle(const void* code) {
    if (!in_game(code)) return;
    for (int i = 0; i < 200 && !executable(code); ++i) Sleep(10);
}

FH2Hook* api_hook_function(FH2Mod* m, const char* name, void* target, void* detour, void** original) {
    const std::string what = "function " + where(target) + (name ? std::string(" (") + name + ")" : "");
    if (!common_checks(m, what.c_str(), target, detour, original)) return nullptr;
    settle(target);
    std::scoped_lock lk{g_mu};
    if (!executable(target)) {
        log_line(m->name, "can't hook " + what + ": it isn't code");
        return nullptr;
    }
    Site* s = find_site(kFunction, target);
    if (!s) {
        char *lo = nullptr, *hi = nullptr;
        if (!function_range(static_cast<unsigned char*>(target), &lo, &hi)) {
            log_line(m->name, "can't hook " + what + ": MinHook can't patch the start of that function");
            return nullptr;
        }
        if (!check_bytes(m, what.c_str(), lo, hi, false)) return nullptr;
        if (!g_minhook) {
            const MH_STATUS st = MH_Initialize();
            if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
                log_line(m->name, std::string("can't hook: MinHook didn't start: ") + MH_StatusToString(st));
                return nullptr;
            }
            g_minhook = true;
        }
        const Stub entry = new_stub(target);
        if (!entry.code) {
            log_line(m->name, "can't hook " + what + ": out of memory for hook stubs");
            return nullptr;
        }
        void* tramp        = nullptr;
        const MH_STATUS st = MH_CreateHook(target, entry.code, &tramp);
        if (st != MH_OK) {
            log_line(m->name, "can't hook " + what + ": " + MH_StatusToString(st));
            return nullptr;
        }
        s        = new_site(kFunction, static_cast<char*>(target), tramp, lo, hi);
        s->entry = entry;
        set_slot(entry.slot, tramp);
    }
    return add_hook(m, name, s, detour, original);
}

FH2Hook* api_hook_call(FH2Mod* m, const char* name, void* call_site, void* detour, void** original) {
    const std::string what = "call " + where(call_site) + (name ? std::string(" (") + name + ")" : "");
    if (!common_checks(m, what.c_str(), call_site, detour, original)) return nullptr;
    settle(call_site);
    std::scoped_lock lk{g_mu};
    auto* at = static_cast<char*>(call_site);
    Site* s  = find_site(kCall, at);
    if (!s) {
        unsigned char op = 0;
        int32_t rel      = 0;
        if (!executable(at) || !readable(at, 5) || ((op = static_cast<unsigned char>(*at)) != 0xE8 && op != 0xE9)) {
            log_line(m->name, "can't hook " + what + ": there's no CALL or JMP rel32 there");
            return nullptr;
        }
        std::memcpy(&rel, at + 1, 4);
        if (!check_bytes(m, what.c_str(), at, at + 5, false)) return nullptr;
        const Stub entry = new_stub(at);
        if (!entry.code || !reaches(at + 5, entry.code)) {
            log_line(m->name, "can't hook " + what + ": no memory for a stub within reach of it");
            return nullptr;
        }
        s        = new_site(kCall, at, at + 5 + rel, at, at + 5);
        s->entry = entry;
        set_slot(entry.slot, s->original);
    }
    return add_hook(m, name, s, detour, original);
}

FH2Hook* api_hook_pointer(FH2Mod* m, const char* name, void** slot, void* detour, void** original) {
    const std::string what = "pointer " + where(slot) + (name ? std::string(" (") + name + ")" : "");
    if (!common_checks(m, what.c_str(), slot, detour, original)) return nullptr;
    if (readable(slot, 8) && !(reinterpret_cast<uintptr_t>(slot) & 7)) settle(*slot);
    std::scoped_lock lk{g_mu};
    auto* at = reinterpret_cast<char*>(slot);
    Site* s  = find_site(kPointer, at);
    if (!s) {
        if ((reinterpret_cast<uintptr_t>(slot) & 7) || !readable(slot, 8)) {
            log_line(m->name, "can't hook " + what + ": not a readable, aligned pointer");
            return nullptr;
        }
        void* old = *slot;
        if (!old || !executable(old)) {
            log_line(m->name, "can't hook " + what + ": it doesn't point at code");
            return nullptr;
        }
        if (!check_bytes(m, what.c_str(), at, at + 8, true)) return nullptr;
        const Stub entry = new_stub(at);
        if (!entry.code) {
            log_line(m->name, "can't hook " + what + ": out of memory for hook stubs");
            return nullptr;
        }
        s        = new_site(kPointer, at, old, at, at + 8);
        s->entry = entry;
        set_slot(entry.slot, old);
    }
    return add_hook(m, name, s, detour, original);
}

int api_enable_hooks(FH2Mod* m, FH2Hook* h) { return switch_hooks(m, h, true); }
int api_disable_hooks(FH2Mod* m, FH2Hook* h) { return switch_hooks(m, h, false); }

void* api_wait_for_code(uint32_t timeout_ms) {
    const ULONGLONG until = timeout_ms == 0xFFFFFFFFu ? ~0ull : GetTickCount64() + timeout_ms;
    for (;;) {
        if (code_ready()) return find_game();
        if (GetTickCount64() >= until) return nullptr;
        Sleep(50);
    }
}

} // namespace

void fill_hook_api(FH2ModApi& api) {
    watch_for_code();
    api.hook_function = api_hook_function;
    api.hook_call     = api_hook_call;
    api.hook_pointer  = api_hook_pointer;
    api.enable_hooks  = api_enable_hooks;
    api.disable_hooks = api_disable_hooks;
    api.wait_for_code = api_wait_for_code;
}

// For crash reports: which hook's trampoline or stub `p` is in.
bool hook_at(const void* p, char* out, std::size_t n) {
    const auto a  = reinterpret_cast<uintptr_t>(p);
    const LONG nr = g_nrecords;
    for (LONG i = 0; i < nr && i < kMaxRecords; ++i)
        if (g_records[i].tramp && a >= g_records[i].tramp && a < g_records[i].tramp + 64) {
            std::snprintf(out, n, "the trampoline (copied game code) of %s", g_records[i].text);
            return true;
        }
    for (int i = 0; i < g_nblocks; ++i)
        if (a >= reinterpret_cast<uintptr_t>(g_blocks[i].code) &&
            a < reinterpret_cast<uintptr_t>(g_blocks[i].code) + 2 * kPage) {
            std::snprintf(out, n, "a platform hook stub");
            return true;
        }
    return false;
}

int hook_count() { return static_cast<int>(std::min<LONG>(static_cast<LONG>(g_nrecords), kMaxRecords)); }

const char* hook_line(int i, bool* enabled) {
    if (i < 0 || i >= hook_count()) return nullptr;
    *enabled = g_records[i].enabled != 0;
    return g_records[i].text;
}

} // namespace fh2platform

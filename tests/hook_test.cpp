// Offline test of the hook API, run as "azimuth.exe" under Wine by
// tests/run_hook_test.sh with the real game exe:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\HookMod.dll
//   FH2\Mount\forza_durango_release_final.exe, FH2\Mount\media\Stripped\StringTables\
//   azimuth.exe <phase> <path to forza_durango_release_final.exe>
// phase "plain": no Extra Options. "eo": a stand-in winmm.dll is loaded and
// FH2_ExtraOptions.log only says it's done 3 seconds after the game is
// mapped (an old log from before this launch must not count). "wine-builtin":
// the stand-in is next to azimuth.exe, but Wine loads its own winmm.dll (no
// override), as for a Linux player who hasn't set one: no waiting for Extra
// Options, and the log says how to fix it. "eo-hidden": as "eo", but the
// system's own winmm.dll is loaded first, so asking for "winmm.dll" by name
// finds that one and not the stand-in (Extra Options' proxy loads the
// system's too); the platform still has to find Extra Options and leave
// mediaoverride to it. "changed-code": as "plain", but
// one of the game functions the media overrides use has a changed byte, so
// they must leave the game's file lookup alone. "crash": a hooked function
// crashes, and the report has to list the hooks.
//
// Azimuth 1.0.1 runs other Forza games too, with the same exe name and base
// address (platform 2.8.0). "other-game": the exe mapped is another game's
// (Forza Motorsport 5's, set up in data\azimuth.ini), so nothing may be hooked
// or patched in it, wait_for_game and wait_for_code have to say so straight
// away, and the mod from before the platform (Legacy.dll) mustn't load.
// "skip-launcher": Azimuth is told to start that game straight away
// (--skip-launcher with [launcher] game=7b35d917), so no mods load at all.
// In the other phases, Legacy.dll loads once the game is FH2.
//
// Media overrides (2.7.3): without Extra Options the platform swaps the
// game's file-lookup pointer (game+0x2E6EF28) for its own function; with
// it, or with the game's code changed, the pointer stays the game's.
// Mounting the folders runs game code, so only the live game shows that.
//
// Car packs with Extra Options (2.9.0): with a pack in fh2-cars the platform chains behind the stand-in's lookups
// instead. "eo-packs": the stand-in takes the game's two lookups 300 ms after the game is mapped, as Extra Options
// does; the platform's must end up in both slots, mount the pack's folder and eo-media ahead of the stand-in's own
// folder on the first lookup (a sound bank's path), and have the zip list answered by eo-media. "eo-packs-refused":
// the stand-in says REFUSED and takes nothing, so the slots stay the game's and the packs are lost.
// "eo-packs-late": the stand-in has set up its folder before the platform's first lookup, so the platform mounts
// nothing and the packs are lost. "eo-packs-changed": as "eo-packs" with the changed byte of "changed-code", so the
// platform leaves the slots to the stand-in and the packs are lost. These four run without HookMod (see
// check_eo_packs below).
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

static std::string slurp(const fs::path& f) {
    std::ifstream in{f, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}
static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

#include "../include/fh2game.h"

// include/fh2game.h: what the game's exe has on disk at every entry with bytes (platform 2.8.0).
static void check_fh2game(const wchar_t* exe) {
    const std::string file = slurp(exe);
    if (file.size() < 4096) return;
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data())->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    auto at = [&](uint32_t rva) -> const char* {
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].SizeOfRawData)
                return file.data() + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
        return nullptr;
    };
    struct Named {
        const char* name;
        uint32_t at;
        const char* bytes;
        std::size_t n;
    };
    static const Named named[] = {
#include "fh2game_checks.inc"
    };
    int bad = 0;
    for (const Named& e : named) {
        const char* p = at(e.at);
        if (!p || std::memcmp(p, e.bytes, e.n) != 0) {
            std::printf("FAIL fh2game.h: FH2_GAME_%s isn't what the game has there\n", e.name);
            ++bad;
        }
    }
    CHECK(bad == 0 && sizeof named / sizeof *named >= 40);
}

static void stub(char* game, uint32_t rva, const void* to);
static void stub_file_system(char* game);

// Maps the exe the way Azimuth does, enough for its bytes and protections:
// sections at their addresses, code read/execute, read-only data read-only.
static char* map_game(const wchar_t* path, bool change_volume_code, bool other_game = false, bool packs = false) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> file((std::istreambuf_iterator<char>(in)), {});
    if (file.size() < 4096) return nullptr;
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    auto* base = static_cast<char*>(VirtualAlloc(reinterpret_cast<void*>(nt->OptionalHeader.ImageBase),
                                                 nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_READWRITE));
    if (!base) return nullptr;
    // Headers last, so the platform only finds the game once it's all there.
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData,
                    std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD c = sec[i].Characteristics;
        DWORD prot    = c & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : c & IMAGE_SCN_MEM_WRITE ? PAGE_READWRITE
                                                                                                 : PAGE_READONLY;
        DWORD old = 0;
        VirtualProtect(base + sec[i].VirtualAddress, std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData), prot,
                       &old);
    }
    // What Extra Options would do: change FMOD::Sound::getUserData's first bytes.
    DWORD old = 0;
    if (!other_game) {
        VirtualProtect(base + 0x199FBE0, 1, PAGE_EXECUTE_READWRITE, &old);
        base[0x199FBE0] = static_cast<char>(0xCC);
        VirtualProtect(base + 0x199FBE0, 1, old, &old);
    }
    if (change_volume_code) { // the game's folder volume constructor, used by the media overrides
        VirtualProtect(base + 0x11E05A0, 1, PAGE_EXECUTE_READWRITE, &old);
        base[0x11E05A0] = static_cast<char>(0xCC);
        VirtualProtect(base + 0x11E05A0, 1, old, &old);
    }
    if (packs) stub_file_system(base);   // before the headers: there from the moment the game is found
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    return base;
}

// ---- the database guards (src/db_guards.cpp), on the game's own DriveTrainConverter ----------------------------
//
// The converter (game+0xB99BE0) runs natively in the mapped exe. The game functions it calls that need the running
// game are pointed at stand-ins here (its type checks, the drive type's text, the icon path's string, the release
// that frees an object, and the "text" compare's import); everything else is the game's code as on disk: the cache's
// Powertrains lookup (game+0x21DEC0, 0x26F900), the types (game+0x16A4B30, 0x15C91E0) and the empty value
// (game+0x160C340). The car database cache is a fake one, with Powertrains Ids 0 to 7 and a hole at 3.
// With the guards on ("plain": another mod's gamedbRC.slt is in mediaoverride), a known Id goes the game's way and an
// unknown one comes back as the game's empty value, with every reference count as it was; with them off ("eo": the
// game's own database), the same unknown Id stops at game+0xB99CD3, as in issue #23.

struct GObj {             // what the converter touches in a value, parameter or result
    void* vtbl;
    uint64_t kind;        // the stand-ins' own: 1 an int, 2 a string
    volatile LONG refs;   // +0x10, as the game's objects have it
    LONG pad;
    uint64_t pad2[2];
    uint64_t v;           // +0x28: an int's value, or a string's text (a wchar_t*)
};
static_assert(offsetof(GObj, refs) == 0x10 && offsetof(GObj, v) == 0x28, "the game's layout");

static char* g_game = nullptr;
static char g_type_int[16];   // the int type the converter's first check asks for (game+0x15C8B20)
static GObj g_made[8];
static int g_nmade = 0, g_drive_calls = 0, g_released = 0, g_last_drive = -1;
static std::string g_last_path;

static void* gs_type_int(int) { return g_type_int; }
static bool gs_is_instance(GObj* o, void* type) {
    if (!o) return false;
    if (type == g_type_int) return o->kind == 1;
    if (type == g_game + 0x36CFB90) return o->kind == 2;   // the string type, game+0x16A4B30's
    return false;
}
static bool gs_assignable(void* target, void* type) { return target == type; }
static int gs_wcscmp(const wchar_t* a, const wchar_t* b) { return std::wcscmp(a, b); }
static GObj** gs_drive_text(void*, GObj** out, int drive) {
    ++g_drive_calls;
    g_last_drive = drive;
    GObj* o      = &g_made[g_nmade++ & 7];
    *o           = {};
    o->refs      = 1;
    *out         = o;
    return out;
}
static GObj** gs_string(GObj** out, const char* text) {
    g_last_path = text;
    GObj* o     = &g_made[g_nmade++ & 7];
    *o          = {};
    o->refs     = 1;
    *out        = o;
    return out;
}
static void gs_release(GObj*) { ++g_released; }

static void stub(char* game, uint32_t rva, const void* to) {
    DWORD old = 0;
    VirtualProtect(game + rva, 12, PAGE_EXECUTE_READWRITE, &old);
    unsigned char b[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};   // mov rax, imm64; jmp rax
    std::memcpy(b + 2, &to, 8);
    std::memcpy(game + rva, b, sizeof b);
    VirtualProtect(game + rva, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), game + rva, 12);
}

using ConvertFn = GObj** (*)(void* self, GObj** out, GObj* value, void* target, GObj* param);
static LONG converter_filter(EXCEPTION_POINTERS* e, DWORD* code, uintptr_t* at) {
    *code = e->ExceptionRecord->ExceptionCode;
    *at   = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
// No object with a destructor here: it has __try.
static bool call_converter(GObj* value, void* target, GObj* param, GObj** out, DWORD* code, uintptr_t* at) {
    static char self[0x100];
    const auto fn = reinterpret_cast<ConvertFn>(g_game + 0xB99BE0);
    __try {
        fn(self + 0x80, out, value, target, param);
        return true;
    } __except (converter_filter(GetExceptionInformation(), code, at)) {
        return false;
    }
}

static void check_db_guards(char* game, bool guarded) {
    g_game = game;
    // Stack walks through the game's code: its function table, with the converter's C++ unwind handler left out
    // (that's the game's CRT, which isn't running here).
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(game + reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    const auto& pd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    CHECK(RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(game + pd.VirtualAddress), pd.Size / sizeof(RUNTIME_FUNCTION),
                              reinterpret_cast<DWORD64>(game)));
    DWORD old = 0;
    char* unwind = game + 0x28D0D14;   // the converter's UNWIND_INFO: version 1, a termination handler
    CHECK(static_cast<unsigned char>(*unwind) == 0x11);
    VirtualProtect(unwind, 1, PAGE_READWRITE, &old);
    *unwind = 0x01;
    VirtualProtect(unwind, 1, old, &old);
    stub(game, 0x15C8B20, reinterpret_cast<const void*>(&gs_type_int));
    stub(game, 0x14530E0, reinterpret_cast<const void*>(&gs_is_instance));
    stub(game, 0x154F960, reinterpret_cast<const void*>(&gs_assignable));
    stub(game, 0xB9B570, reinterpret_cast<const void*>(&gs_drive_text));
    stub(game, 0x16A0200, reinterpret_cast<const void*>(&gs_string));
    stub(game, 0x257730, reinterpret_cast<const void*>(&gs_release));
    void** iat = reinterpret_cast<void**>(game + 0x1DD0B00);   // the "text" compare's import
    VirtualProtect(iat, 8, PAGE_READWRITE, &old);
    *iat = reinterpret_cast<void*>(&gs_wcscmp);
    VirtualProtect(iat, 8, old, &old);

    // The cache: Powertrains Ids 0 to 7, record 3 missing. A record: its drive type at +1, its icon's path at +0x30
    // (an MSVC std::string, short, so in place: the size at +0x10 and the capacity, 15, at +0x18 from there).
    static char cache[0x400], table[0x80], records[8][0x60];
    static char* list[9];
    for (int i = 0; i < 8; ++i) {
        records[i][1] = static_cast<char>(1 + i % 3);
        std::snprintf(records[i] + 0x30, 16, "ICON%d", i);
        const uint64_t size = std::strlen(records[i] + 0x30), cap = 15;
        std::memcpy(records[i] + 0x40, &size, 8);
        std::memcpy(records[i] + 0x48, &cap, 8);
        list[i] = i == 3 ? nullptr : records[i];
    }
    list[8] = reinterpret_cast<char*>(0x4141414141414141ull);   // past the end, where the game's jbe reads for Id 8
    char* table_ptr = table;
    std::memcpy(cache + 0x3A0, &table_ptr, 8);
    char* begin = reinterpret_cast<char*>(list);
    char* end   = reinterpret_cast<char*>(list + 8);
    std::memcpy(table + 0x68, &begin, 8);
    std::memcpy(table + 0x70, &end, 8);
    char* cache_ptr = cache;
    std::memcpy(game + 0x34ABA60, &cache_ptr, 8);

    void* const string_type = game + 0x36CFB90;
    void* const other_type  = game + 0x36AD5E0;   // game+0x15C91E0's: the icon itself
    auto* const empty       = reinterpret_cast<GObj*>(game + 0x36BA4F8);
    static wchar_t text[] = L"text", icon[] = L"icon";
    auto run = [&](uint32_t id, void* target, wchar_t* param_text, GObj** out, LONG* param_refs, DWORD* code, uintptr_t* at) {
        static GObj value, param;
        value      = {};
        value.kind = 1;
        value.v    = id;
        param      = {};
        param.kind = 2;
        param.refs = 1;
        param.v    = reinterpret_cast<uint64_t>(param_text);
        *out       = nullptr;
        const bool ok = call_converter(&value, target, &param, out, code, at);
        *param_refs   = param.refs;
        return ok;
    };
    GObj* out = nullptr;
    LONG prefs = 0;
    DWORD code = 0;
    uintptr_t at = 0;
    std::string log;
    for (int i = 0; i < 100; ++i) {
        log = slurp(L"fh2-mods\\platform.log");
        if (has(log, "[Database guards] o")) break;
        Sleep(100);
    }
    if (!guarded) {
        CHECK(has(log, "[Database guards] off: the game reads its own database (no car pack or mod changes it)"));
        // Without the guard, Id 9 (not in the table) stops the converter where issue #23's crash did.
        const bool ok = run(9, string_type, text, &out, &prefs, &code, &at);
        std::printf("     unguarded, Id 9: %s, exception 0x%08lX at game+0x%llX\n", ok ? "returned" : "stopped", code,
                    static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(game)));
        CHECK(!ok && code == EXCEPTION_ACCESS_VIOLATION && at == reinterpret_cast<uintptr_t>(game) + 0xB99CD3);
        return;
    }
    CHECK(has(log, "[Database guards] on (mediaoverride has another mod's database): a car whose powertrain isn't in the "
                   "game's Powertrains table is shown without its drivetrain instead of stopping the game"));
    CHECK(has(log, "hooked instruction game+0xB99CA0") && has(log, "hooked instruction game+0xB99D74"));
    // A known Id, as text: the game's way, the drive type's text made and handed over, the parameter released.
    bool ok = run(2, string_type, text, &out, &prefs, &code, &at);
    CHECK(ok && g_drive_calls == 1 && g_last_drive == 3 && out == &g_made[0] && out->refs == 1 && prefs == 1);
    // A known Id, as the icon's path: the record's own string.
    ok = run(5, string_type, icon, &out, &prefs, &code, &at);
    CHECK(ok && g_last_path == "ICON5" && out == &g_made[1] && out->refs == 1 && prefs == 1);
    CHECK(!has(slurp(L"fh2-mods\\platform.log"), "a car's powertrain"));
    // Unknown Ids: Id 8 (one past the end, which the game would read), the hole at 3 (as the icon's path) and Id 40
    // (as the icon itself): the game's empty value, AddRef'd once each, nothing made, the parameter released.
    const LONG empty_refs = empty->refs;
    const int made        = g_nmade;
    ok = run(8, string_type, text, &out, &prefs, &code, &at);
    CHECK(ok && out == empty && empty->refs == empty_refs + 1 && prefs == 1);
    ok = run(3, string_type, icon, &out, &prefs, &code, &at);
    CHECK(ok && out == empty && empty->refs == empty_refs + 2 && prefs == 1);
    ok = run(40, other_type, icon, &out, &prefs, &code, &at);
    CHECK(ok && out == empty && empty->refs == empty_refs + 3 && prefs == 1);
    CHECK(g_nmade == made && g_drive_calls == 1 && g_released == 0);
    log = slurp(L"fh2-mods\\platform.log");
    CHECK(has(log, "[Database guards] a car's powertrain, 8, isn't in the game's Powertrains table, so the game would have "
                   "stopped at game+0xB99CD3"));
    CHECK(has(log, "The likely cause is a car pack or a mod that changes the game database (mediaoverride has another mod's database)"));
    CHECK(log.find("a car's powertrain") == log.rfind("a car's powertrain"));   // once a session
    // And a known Id still goes the game's way after them.
    ok = run(0, string_type, text, &out, &prefs, &code, &at);
    CHECK(ok && g_drive_calls == 2 && g_last_drive == 1 && out->refs == 1 && prefs == 1);
}

// ---- car packs with Extra Options (src/media_overrides.cpp, watch_eo): the "eo-packs" phases -------------------
//
// The game isn't running, so the test plays it. Its file system is an object with the game's own vtable
// (game+0x2E6ED18, whose +0x210 and +0x1B0 slots are the ones Extra Options and the platform take), a list of volumes
// laid out as the game's (56-byte entries from +0x30 to +0x38) and a root volume at +0x68. The game functions the
// platform and the stand-in Extra Options call are stand-ins: the mount (game+0x11F82B0) and the sort
// (game+0x12039D0) from the start; and, once the platform has checked the game's code against the exe, the resolver
// (game+0x11F49B0), a file's path on disk (game+0x11F1460), operator new (game+0x3229C0), the folder volume's
// constructor (game+0x11E05A0) and its "does a file exist" (game+0x11F9C70). A folder volume's reference counting is
// the game's own code. T:\ is data\titles\1A96545B\tmp here, and game:\ is FH2\Mount.

struct PString {   // an MSVC std::string
    union {
        const char* data;
        char in_place[16];
    };
    std::size_t length, capacity;
};
static PString pstring(const char* s) {
    PString p{};
    p.data     = s;
    p.length   = std::strlen(s);
    p.capacity = 31;
    return p;
}
static std::string ptext(const PString* s) { return std::string(s->capacity < 16 ? s->in_place : s->data, s->length); }

struct PVolume {   // the game's folder volume: references at +0x20, its folder at +0x28
    void** vtable;
    char pad[0x18];
    volatile LONG refs;
    LONG pad2;
    PString folder;
    char flag;
    char pad3[7];
};
struct PEntry {   // one of the file system's volumes
    uint32_t priority, pad;
    void* volume;
    PString path;
    uint32_t hash, pad2;
};
struct PFileSystem {
    void** vtable;
    char pad[0x28];
    PEntry *begin, *end;
    char pad2[0x28];
    void* root;
};
static_assert(sizeof(PEntry) == 56 && offsetof(PFileSystem, begin) == 0x30 && offsetof(PFileSystem, root) == 0x68 &&
              offsetof(PVolume, refs) == 0x20 && offsetof(PVolume, folder) == 0x28 && sizeof(PVolume) == 0x50, "the game's layout");

using ResolveFn  = unsigned char (*)(PFileSystem*, void**, void*, const PString*, unsigned char, unsigned char, unsigned char, void**, unsigned char);
using DiskPathFn = void* (*)(PFileSystem*, const PString*, PString*);

static PEntry g_entries[64];
static PFileSystem g_fs;
static void* g_own_vtable[1 + 0x1F0 / sizeof(void*)];   // the game's own volumes here: in this exe's image
static PVolume g_own[3];

static unsigned char own_exists(PVolume*, const PString*) { return 1; }   // the game's media has whatever is asked for

static unsigned char gs_mount(PFileSystem* fs, const PString* target, void** volume, unsigned priority, unsigned char, unsigned char,
                              const void*) {
    if (fs->end - fs->begin >= 64) return 0;
    auto* v = static_cast<PVolume*>(*volume);
    reinterpret_cast<LONG (*)(PVolume*)>(v->vtable[0x10 / 8])(v);
    *fs->end++ = PEntry{priority, 0, v, *target, 0, 0};
    return 1;
}
static void gs_sort(PFileSystem* fs) {   // highest priority first, as unsigned numbers
    std::stable_sort(fs->begin, fs->end, [](const PEntry& a, const PEntry& b) { return a.priority > b.priority; });
}
// The first volume in the list's order that has the file.
static unsigned char gs_resolve(PFileSystem* fs, void** volume, void*, const PString* path, unsigned char, unsigned char, unsigned char,
                                void**, unsigned char) {
    const std::string full = ptext(path), under = full.rfind("game:\\media\\", 0) == 0 ? full.substr(12) : full;
    const PString relative = pstring(under.c_str());
    for (PEntry* e = fs->begin; e != fs->end; ++e) {
        auto* v = static_cast<PVolume*>(e->volume);
        if (reinterpret_cast<unsigned char (*)(PVolume*, const PString*)>(v->vtable[0xE8 / 8])(v, &relative)) {
            if (volume) *volume = v;
            return 1;
        }
    }
    return 0;
}
// It asks the resolver through the file system's own slot, as the game's does.
static void* gs_disk_path(PFileSystem* fs, const PString* path, PString* output) {
    void* volume = nullptr;
    PString relative{};
    relative.capacity = 15;
    reinterpret_cast<ResolveFn>(fs->vtable[0x210 / 8])(fs, &volume, &relative, path, 0, 0, 0, nullptr, 0);
    return output;
}
static void* gs_new(std::size_t n) { return std::calloc(1, n); }
static PVolume* gs_construct(PVolume* v, const PString* folder, unsigned char flag) {
    static std::string kept[16];
    static int made = 0;
    std::memset(static_cast<void*>(v), 0, sizeof *v);
    kept[made & 15] = ptext(folder);
    v->folder       = pstring(kept[made++ & 15].c_str());
    v->flag         = static_cast<char>(flag);
    return v;
}
static unsigned char gs_exists(PVolume* v, const PString* path) {
    std::string folder = ptext(&v->folder);
    if (folder.rfind("T:\\", 0) == 0) folder = "data\\titles\\1A96545B\\tmp\\" + folder.substr(3);
    else if (folder.rfind("game:\\", 0) == 0) folder = "FH2\\Mount\\" + folder.substr(6);
    const DWORD a = GetFileAttributesA((folder + ptext(path)).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void stub_file_system(char* game) {
    stub(game, 0x11F82B0, reinterpret_cast<const void*>(&gs_mount));
    stub(game, 0x12039D0, reinterpret_cast<const void*>(&gs_sort));
}

// The game's file system before anything is mounted over it: three volumes of the game's own.
static void make_file_system() {
    g_own_vtable[1 + 0xE8 / 8] = reinterpret_cast<void*>(&own_exists);
    const unsigned priorities[3] = {0x96, 0x64, 5};
    for (int i = 0; i < 3; ++i) {
        g_own[i].vtable = g_own_vtable + 1;
        g_entries[i]    = PEntry{priorities[i], 0, &g_own[i], pstring("game:\\media\\"), 0, 0};
    }
    g_fs.vtable = reinterpret_cast<void**>(0x142E6ED18ull);   // the game's, once it's mapped
    g_fs.begin  = g_entries;
    g_fs.end    = g_entries + 3;
    g_fs.root   = &g_own[2];
}

// The game looking a file up, and asking for a file's path on disk.
static bool look_up(const char* path, PVolume** answered) {
    const PString p = pstring(path);
    void* volume    = nullptr;
    PString relative{};
    relative.capacity = 15;
    const bool found  = reinterpret_cast<ResolveFn>(g_fs.vtable[0x210 / 8])(&g_fs, &volume, &relative, &p, 0, 0, 0, nullptr, 0) != 0;
    *answered         = static_cast<PVolume*>(volume);
    return found;
}
static void path_on_disk(const char* path) {
    const PString p = pstring(path);
    PString out{};
    reinterpret_cast<DiskPathFn>(g_fs.vtable[0x1B0 / 8])(&g_fs, &p, &out);
}

static bool wait_for_log(const char* what, int tenths) {
    for (int i = 0; i < tenths; ++i) {
        if (has(slurp(L"fh2-mods\\platform.log"), what)) return true;
        Sleep(100);
    }
    return false;
}

static void check_eo_packs(const std::wstring& phase, char* game) {
    void** slot      = reinterpret_cast<void**>(game + 0x2E6EF28);
    void** path_slot = reinterpret_cast<void**>(game + 0x2E6EEC8);
    const auto platforms = [](void* p) {
        MEMORY_BASIC_INFORMATION mbi{};
        return p && VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && mbi.AllocationBase == GetModuleHandleW(L"d3dcompiler_47.dll");
    };
    // fh2-mods\platform-eo-packs.txt, which the platform's own thread writes a moment after the log line (and tries
    // again when the file is still open somewhere): `is` within three seconds.
    const auto outcome = [](const char* is) {
        for (int i = 0; i < 30; ++i) {
            if (slurp(L"fh2-mods\\platform-eo-packs.txt").rfind(is, 0) == 0) return true;
            Sleep(100);
        }
        return false;
    };
    const auto game_functions = [&] {   // from here on the platform has checked them against the exe
        stub(game, 0x11F49B0, reinterpret_cast<const void*>(&gs_resolve));
        stub(game, 0x11F1460, reinterpret_cast<const void*>(&gs_disk_path));
        stub(game, 0x3229C0, reinterpret_cast<const void*>(&gs_new));
        stub(game, 0x11E05A0, reinterpret_cast<const void*>(&gs_construct));
        stub(game, 0x11F9C70, reinterpret_cast<const void*>(&gs_exists));
    };
    const std::string eo_media = "T:\\fh2-platform\\eo-media\\";
    PVolume* v = nullptr;
    std::string log;
    if (phase == L"eo-packs-refused") {
        CHECK(wait_for_log("car packs with Extra Options: the packs are NOT in the game this session", 150));
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "car packs with Extra Options: Extra Options didn't take over the game's file lookup this time (its status: "
                       "\"REFUSED: hook test stand-in. No patches applied.\"); game+0x2E6EF28 is left as the game's"));
        CHECK(has(log, "the packs are NOT in the game this session: Extra Options didn't take over the game's file lookup: REFUSED: hook "
                       "test stand-in. No patches applied."));
        CHECK(*slot == game + 0x11F49B0 && *path_slot == game + 0x11F1460);
        Sleep(200);
        CHECK(outcome("lost\t"));
        CHECK(!has(log, "chain:"));
    } else if (phase == L"eo-packs-changed") {
        CHECK(wait_for_log("car packs with Extra Options: the packs are NOT in the game this session", 150));
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "car packs with Extra Options: the game's code isn't what this build expects: game+0x11E05A0 (the folder volume "
                       "constructor) isn't the same as in the exe on disk, "));
        CHECK(has(log, " ms after the game was mapped; the game's file lookup stays Extra Options'"));
        CHECK(has(log, "the packs are NOT in the game this session: The game's code isn't what this build expects."));
        Sleep(800);   // the stand-in takes the slots 300 ms after the game is mapped: they stay its own
        CHECK(*slot != game + 0x11F49B0 && !platforms(*slot) && !platforms(*path_slot));
        CHECK(!has(slurp(L"fh2-mods\\platform.log"), "chain:"));
        CHECK(outcome("lost\t"));
    } else if (phase == L"eo-packs-late") {
        CHECK(wait_for_log("car packs with Extra Options: chain: Extra Options finished", 150));
        CHECK(platforms(*slot) && platforms(*path_slot));
        game_functions();
        // The stand-in's own folder is in the list already: the platform must mount nothing, and pass the lookup on.
        CHECK(g_fs.end - g_fs.begin == 4);
        CHECK(look_up("game:\\media\\zipmanifest.xml", &v) && v == &g_own[0]);
        CHECK(g_fs.end - g_fs.begin == 4);
        CHECK(wait_for_log("car packs with Extra Options: the packs are NOT in the game this session", 30));
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "first lookup: #1, a file, ") && has(log, "the game had 4 volume(s) then, 1 of them Extra Options'"));
        CHECK(has(log, "car packs with Extra Options: LATE: Extra Options had already set up its folders before the platform's first lookup"));
        CHECK(has(log, "0x7FFFFFFE Extra Options' game:\\mediaoverride\\; and 3 of the game's own below them"));
        CHECK(has(log, "the packs are NOT in the game this session: Extra Options had set up its folders before the car packs' were added."));
        CHECK(!has(log, " mounted at 0x") && !has(log, "precedence:"));
        Sleep(200);
        CHECK(outcome("lost\t"));
    } else {
        CHECK(wait_for_log("car packs with Extra Options: chain: Extra Options finished", 150));
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "car packs: 1 pack(s) in fh2-cars are read with Extra Options loaded (version 0.6.0)"));
        CHECK(has(log, "car packs with Extra Options: chain: Extra Options took the game's file lookup (game+0x2E6EF28), seen "));
        CHECK(has(log, " ms after the game was mapped; the platform chained behind it "));
        CHECK(has(log, "chain: Extra Options took the lookup of a file's path on disk too (game+0x2E6EEC8, for sound banks); the platform "
                       "chained behind it "));
        CHECK(has(log, "(\"OK: hook test stand-in, lookups taken.\"); the game's file lookup is still the platform's, with Extra Options' "
                       "behind it"));
        CHECK(platforms(*slot) && platforms(*path_slot));
        CHECK(outcome("on\t"));
        game_functions();
        // The game's first lookup is a sound bank's path on disk, which Extra Options answers without going through
        // the file lookup: the platform has to be in front of that one too.
        path_on_disk("game:\\media\\audio\\fmodanthem\\durango\\PG_Test.bank");
        CHECK(g_fs.end - g_fs.begin == 6);
        CHECK(g_fs.begin[0].priority == 0x7FFFFFFF && ptext(&static_cast<PVolume*>(g_fs.begin[0].volume)->folder) == eo_media);
        CHECK(g_fs.begin[1].priority == 0x7FFFFFFE && g_fs.begin[2].priority == 0x7FFFFFF0 &&
              ptext(&static_cast<PVolume*>(g_fs.begin[2].volume)->folder) == "T:\\fh2-platform\\packs\\TestPack\\");
        CHECK(look_up("game:\\media\\zipmanifest.xml", &v) && v == g_fs.begin[0].volume);
        CHECK(look_up("game:\\media\\Stripped\\gamedbRC.slt", &v) && v == &g_own[0]);
        CHECK(has(slurp(L"data\\titles\\1A96545B\\tmp\\fh2-platform\\eo-media\\zipmanifest.xml"), "path=\"game:\\media\\cars\\test_car.zip\""));
        CHECK(wait_for_log("car packs with Extra Options: gamedbRC.slt (lookup #", 30));
        Sleep(300);   // the outcome is written by the platform's own thread
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "car packs with Extra Options: lookup #1 (a sound bank's path on disk) on thread "));
        CHECK(has(log, ": game:\\media\\audio\\fmodanthem\\durango\\PG_Test.bank"));
        CHECK(has(log, "car packs with Extra Options: first lookup: #1, a sound bank's path on disk, ") &&
              has(log, " ms after the platform chained; the game had 3 volume(s) then, 0 of them Extra Options'"));
        CHECK(has(log, "car packs with Extra Options: car pack TestPack T:\\fh2-platform\\packs\\TestPack\\ mounted at 0x7FFFFFF0"));
        CHECK(has(log, "car packs with Extra Options: eo-media T:\\fh2-platform\\eo-media\\ mounted at 0x7FFFFFFF, ahead of every other folder"));
        CHECK(has(log, "car packs with Extra Options: precedence: PASS: eo-media is alone at the top, with 1 folder(s) of Extra Options' "
                       "below it. The volumes, first read first: 0x7FFFFFFF eo-media (T:\\fh2-platform\\eo-media\\); 0x7FFFFFFE Extra "
                       "Options' game:\\mediaoverride\\; 0x7FFFFFF0 the platform's T:\\fh2-platform\\packs\\TestPack\\; and 3 of the "
                       "game's own below them"));
        CHECK(has(log, "car packs with Extra Options: zipmanifest.xml (lookup #3, game:\\media\\zipmanifest.xml) was answered by eo-media"));
        CHECK(has(log, "car packs with Extra Options: gamedbRC.slt (lookup #4, game:\\media\\Stripped\\gamedbRC.slt) was answered by a zip "
                       "or other volume of the game's kind, NOT eo-media; this build keeps none in eo-media"));
        CHECK(!has(log, "NOT in the game this session") && !has(log, "LATE") && !has(log, "FAIL"));
        CHECK(outcome("in\t"));
        // The game mapped again, or Extra Options undoing its changes: the slot is the game's own again. The packs
        // are lost, and eo-media stops answering.
        *slot = game + 0x11F49B0;
        CHECK(wait_for_log("car packs with Extra Options: the packs are NOT in the game this session", 30));
        log = slurp(L"fh2-mods\\platform.log");
        CHECK(has(log, "car packs with Extra Options: the game's file lookup was changed back "));
        CHECK(has(log, "the packs are NOT in the game this session: The game's file lookup was changed again after the platform had chained "
                       "behind Extra Options."));
        CHECK(look_up("game:\\media\\zipmanifest.xml", &v) && v == &g_own[0]);
        Sleep(200);
        CHECK(outcome("lost\t"));
    }
    log = slurp(L"fh2-mods\\platform.log");
    std::printf("---- platform.log\n%s----\n", log.c_str());
    CHECK(has(log, "FH2 Mod Platform 2.9.0 (API 10)"));
    CHECK(has(log, "media overrides: Extra Options is here, so mediaoverride is left to it"));
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
}

static LONG WINAPI after_crash(EXCEPTION_POINTERS*) {
    std::string report;
    for (const auto& e : fs::directory_iterator("fh2-mods\\crashes"))
        if (e.path().extension() == ".txt") report = slurp(e.path());
    std::printf("---- crash report\n%s----\n", report.c_str());
    CHECK(has(report, "CRASH: access violation (0xC0000005) at HookMod.dll+0x"));
    CHECK(has(report, "the crash came from HookMod.dll"));
    CHECK(has(report, "hooks (* = on):"));
    CHECK(has(report, "* HookA: function HookMod.dll+0x"));
    CHECK(has(report, "* HookA: pointer game+0x2D44F30 (ChampionshipDrivatarsState::Serialize)"));
    CHECK(has(report, "  HookB: call game+0x1018A13"));
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: azimuth.exe plain|eo|eo-hidden|eo-packs|eo-packs-refused|eo-packs-late|eo-packs-changed|wine-builtin|changed-code|crash|other-game|"
                    "skip-launcher "
                    "<forza_durango_release_final.exe> [--skip-launcher]\n");
        return 2;
    }
    const std::wstring phase = argv[1];
    if (phase == L"crash") {
        std::ofstream{"crash.txt"} << "1";
        SetUnhandledExceptionFilter(after_crash);
    }
    const bool other_game = phase == L"other-game" || phase == L"skip-launcher";
    if (other_game) std::ofstream{"other_game.txt"} << "1"; // for HookMod
    const bool eo    = phase == L"eo" || phase == L"eo-hidden";
    const bool packs = phase.rfind(L"eo-packs", 0) == 0;
    if (phase == L"eo-hidden") {
        // The system's winmm.dll first: then GetModuleHandleW(L"winmm.dll") answers with it, not the stand-in.
        wchar_t sys[MAX_PATH];
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        CHECK(n && n < MAX_PATH && LoadLibraryW((std::wstring(sys) + L"\\winmm.dll").c_str()) != nullptr);
    }
    if (eo) CHECK(GetProcAddress(LoadLibraryW(L".\\winmm.dll"), "Fh2TweaksStatus")); // Extra Options' proxy
    if (packs) {
        HMODULE stand_in = LoadLibraryW(L".\\winmm.dll");
        CHECK(GetProcAddress(stand_in, "Fh2TweaksStatus"));
        const auto file_system = reinterpret_cast<void (*)(void*)>(reinterpret_cast<void*>(GetProcAddress(stand_in, "FakeEoFilesystem")));
        CHECK(file_system != nullptr);
        make_file_system();
        if (file_system) file_system(&g_fs);
    }
    if (phase == L"eo-hidden") {
        // Under Wine which of the two the name finds isn't fixed, so only Windows checks it's the system's.
        HMODULE n = GetModuleHandleW(L"ntdll.dll");
        if (!n || !GetProcAddress(n, "wine_get_version"))
            CHECK(!GetProcAddress(GetModuleHandleW(L"winmm.dll"), "Fh2TweaksStatus"));
    }
    if (phase == L"wine-builtin") {
        HMODULE w = LoadLibraryW(L".\\winmm.dll"); // Wine's own, though it says it's this one
        CHECK(w && !GetProcAddress(w, "Fh2TweaksStatus"));
    }
    CHECK(LoadLibraryW(L"d3dcompiler_47.dll") != nullptr);
    Sleep(500);
    char* game = map_game(argv[2], phase == L"changed-code" || phase == L"eo-packs-changed", other_game, packs);
    CHECK(game != nullptr); // "press Play"
    if (packs && game) check_eo_packs(phase, game);
    if (phase == L"plain") check_fh2game(argv[2]);
    if (eo) {
        Sleep(3000);
        std::ofstream{"FH2_ExtraOptions.log", std::ios::app} << "AZIMUTH: winmm proxy initialized; original TLS helper retained.\r\n";
    }
    std::string log;
    for (int i = 0; i < 300; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if (has(log, "hook tests done") || has(log, "no mods are loaded")) break;
    }
    if (phase == L"crash") {
        Sleep(10000);
        std::printf("FAIL no crash\n");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    Sleep(200);
    log = slurp(L"fh2-mods\\platform.log");
    std::printf("---- platform.log\n%s----\n", log.c_str());
    CHECK(has(log, "FH2 Mod Platform 2.9.0 (API 10)"));
    if (other_game) {
        Sleep(500); // whatever the platform would still do once the game is up
        log = slurp(L"fh2-mods\\platform.log");
        if (phase == L"skip-launcher") {
            CHECK(has(log, "Azimuth was started for Forza Motorsport 5 (title 7b35d917), from --skip-launcher, and "
                           "[launcher] game=7b35d917 in data\\azimuth.ini, not Forza Horizon 2: no mods are loaded"));
            CHECK(!has(log, "loaded HookMod.dll") && !has(log, "[HookA]"));
            CHECK(!has(log, "media overrides"));
        } else {
            CHECK(has(log, "game: Azimuth started Forza Motorsport 5 (title 7b35d917), not Forza Horizon 2. The "
                           "platform and its mods leave it alone"));
            CHECK(has(log, "hook tests done, 0 failed"));
            CHECK(has(log, "[HookA] can't hook function 0x140001000 (other game's code): it's in Forza Motorsport 5 "
                           "(title 7b35d917), which the platform leaves alone"));
            CHECK(has(log, "[HookA] can't make patch 0x140001000 (other game's bytes): it's in Forza Motorsport 5"));
            CHECK(has(log, "[HookA] can't hook pointer 0x140002000"));
            CHECK(has(log, "[HookA] can't hook instruction 0x140001000"));
            CHECK(has(log, "Legacy.dll isn't loaded: it's a mod from before the platform"));
            CHECK(!has(log, "hooks: game code is ready"));
            CHECK(!has(log, "media overrides: hooked"));
        }
        CHECK(!fs::exists("legacy_ran.txt"));
        CHECK(!has(log, "loaded Legacy.dll"));
        std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), fails);
    }
    CHECK(has(log, "game: Azimuth started Forza Horizon 2 (the 24 Nov 2015 exe the mods are made for)"));
    CHECK(has(log, "Legacy.dll has no fh2mod_init (a mod from before the platform); it runs on its own, and loads "
                   "once the game Azimuth starts is known to be Forza Horizon 2"));
    CHECK(has(log, "loaded Legacy.dll") && fs::exists("legacy_ran.txt"));
    CHECK(has(log, "(ivictim mov)") && has(log, "(mark b) (shared with HookA, HookB)"));
    CHECK(has(log, "(prolog): it's in the function's prolog"));
    CHECK(has(log, "(mid-instruction): it isn't the start of an instruction"));
    CHECK(has(log, "(jumped into): the function jumps into the bytes the hook would replace"));
    CHECK(has(log, "(wrong expect): the bytes there aren't the ones the mod expected"));
    CHECK(has(log, "[HookB] its instruction hook HookMod.dll+0x") && has(log, "(crashes) crashed (exception 0xC0000005)"));
    CHECK(has(log, "hook tests done, 0 failed"));
    CHECK(!has(log, "check FAIL"));
    CHECK(has(log, "[HookA] hooked function HookMod.dll+0x"));
    CHECK(has(log, "[HookB] hooked function HookMod.dll+0x") && has(log, "(victim) (shared with HookA, HookB)"));
    CHECK(has(log, "[HookB] can't hook function") && has(log, "it overlaps HookA's call hook at"));
    CHECK(has(log, "[HookB] can't hook function game+0x199FBE0 (FMOD::Sound::getUserData): that game code was "
                   "already changed outside the platform"));
    CHECK(has(log, "[HookA] can't switch another mod's hook") == false); // B tried A's: logged under B
    CHECK(has(log, "[HookB] can't switch another mod's hook"));
    CHECK(has(log, "[HookA] can't hook call HookMod.dll+0x") && has(log, "(not a call): there's no CALL or JMP rel32 there"));
    CHECK(has(log, "hooks: checking game code against"));
    const auto at = log.find("wait_for_code took ");
    CHECK(at != std::string::npos);
    const unsigned long waited = at == std::string::npos ? 0 : std::stoul(log.substr(at + 19));
    if (eo) {
        CHECK(has(log, "hooks: game code is ready (Extra Options has applied its patches)"));
        CHECK(waited >= 2000);
    } else {
        CHECK(has(log, "hooks: game code is ready (no Extra Options)"));
        CHECK(waited < 2000);
    }
    CHECK(has(log, "WINEDLLOVERRIDES=\"winmm=n,b\"") == (phase == L"wine-builtin"));
    if (phase == L"plain" || phase == L"eo") check_db_guards(game, phase == L"plain");
    // The game's file lookup: the platform's own function only without Extra Options.
    void* lookup = game ? *reinterpret_cast<void**>(game + 0x2E6EF28) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    const bool platforms = lookup && VirtualQuery(lookup, &mbi, sizeof mbi) == sizeof mbi &&
                           mbi.AllocationBase == GetModuleHandleW(L"d3dcompiler_47.dll");
    if (eo) {
        CHECK(has(log, "media overrides: Extra Options is here, so mediaoverride is left to it"));
        CHECK(lookup == game + 0x11F49B0);
    } else if (phase == L"changed-code") {
        CHECK(has(log, "media overrides: the game's code isn't what this build expects; left alone"));
        CHECK(lookup == game + 0x11F49B0);
    } else {
        CHECK(has(log, "media overrides: Extra Options isn't loaded, so the platform mounts mediaoverride"));
        CHECK(has(log, "media overrides: hooked the game's file lookup"));
        CHECK(platforms);
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
    return fails;
}

// Media overrides when Extra Options isn't loaded (platform 2.7.3).
//
// Extra Options makes the game read files from <Mount>\mediaoverride (and
// \mediapc) ahead of its own media, and every platform service that writes
// files relies on that. When it isn't loaded (Azimuth 1.0.1, which it
// doesn't support yet, or Wine without the winmm override), the platform
// does the same itself. Only then, because Extra Options checks every place
// it patches and refuses all of its patches if any one was changed first,
// this one included.
//
// The game finds a file through its file system's resolver, a function the
// pointer at game+0x2E6EF28 points to (game+0x11F49B0). That pointer is
// swapped for resolve() below as soon as the image is mapped, before the
// game's code runs. On the first lookup, resolve() builds two of the game's
// own folder volumes, for game:\mediaoverride\ and game:\mediapc\, mounts
// them over game:\media\ ahead of everything else (no hash-manifest check),
// and has the file system sort its volumes again. Files inside the game's
// zips are overridden the same way: mediaoverride\UI\Scenes\... stands for
// media\UI.zip's Scenes\....
//
// Addresses are for the 24 Nov 2015 exe, RVAs from its base:
//   0x2E6EF28  file system vtable slot: the resolver
//   0x11F49B0  the resolver
//   0x03229C0  the game's operator new
//   0x11E05A0  folder volume constructor (this, const string& folder, bool)
//   0x2EAB910  folder volume vtable, preceded by its RTTI locator
// File system methods used (vtable offsets): +0x1E8 mount a volume, +0x1F8
// sort the volumes; root volume pointer at +0x68 (zero until it's set up).
// Folder volume methods: +0x10 add a reference, +0x18 release, +0xE8 does a
// file exist.
#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTimestamp       = 0x565501EDu;
constexpr uintptr_t kPreferredBase  = 0x140000000ull;
constexpr uint32_t RVA_RESOLVE_SLOT = 0x2E6EF28;
constexpr uint32_t RVA_RESOLVE      = 0x11F49B0;
constexpr uint32_t RVA_ALLOCATE     = 0x03229C0;
constexpr uint32_t RVA_CONSTRUCT    = 0x11E05A0;
constexpr uint32_t RVA_FOLDER_VT    = 0x2EAB910; // RTTI locator; the methods follow

// An MSVC std::string the game only reads: a pointer, as for capacity >= 16.
struct GameString {
    const char* data;
    uintptr_t unused;
    std::size_t length, capacity;
};
GameString game_string(const char* s) { return {s, 0, std::strlen(s), 31}; }

using Resolve   = uint8_t (*)(void*, void**, void*, const void*, uint8_t, uint8_t, uint8_t, void**, uint8_t);
using Allocate  = void* (*)(std::size_t);
using Construct = void* (*)(void*, const GameString*, uint8_t);
using Method    = void (*)(void*);
using Mount     = uint8_t (*)(void*, const GameString*, void**, unsigned, uint8_t, uint8_t, const void*);
using Exists    = uint8_t (*)(void*, const GameString*);

char* g_base = nullptr;
Resolve g_resolve = nullptr;
Exists g_exists   = nullptr;
void* g_folder_vtable[1 + 0x1F0 / sizeof(void*)];
std::atomic<int> g_state{0}; // 0 not mounted, 1 mounting, 2 done
std::atomic<int> g_lookups{0};

// The game's own folder volume answers archive selectors such as "<*>0" as
// disk names; they aren't files. Same check as Extra Options'.
uint8_t folder_exists(void* volume, const GameString* path) {
    const char* text = path->capacity < 16 ? reinterpret_cast<const char*>(path) : path->data;
    for (std::size_t i = 0; i < path->length; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == '<' || c == '>' || c == '*' || c == '?' || c == '"' || c == '|') return 0;
    }
    return g_exists(volume, path);
}

int mount_folder(void* filesystem, const char* folder, unsigned priority) {
    void* storage = reinterpret_cast<Allocate>(g_base + RVA_ALLOCATE)(0x50);
    if (!storage) return 0;
    const GameString source = game_string(folder), target = game_string("game:\\media\\");
    void* volume = reinterpret_cast<Construct>(g_base + RVA_CONSTRUCT)(storage, &source, 1);
    *reinterpret_cast<void***>(volume) = g_folder_vtable + 1;
    void** methods = *reinterpret_cast<void***>(volume);
    reinterpret_cast<Method>(methods[0x10 / 8])(volume);
    void** fs_methods = *reinterpret_cast<void***>(filesystem);
    const int ok      = reinterpret_cast<Mount>(fs_methods[0x1E8 / 8])(filesystem, &target, &volume, priority, 0, 0, nullptr);
    reinterpret_cast<Method>(methods[0x18 / 8])(volume);
    return ok;
}

// Returns a line for the log, or "" when it's too early (no root volume yet).
std::string mount_all(void* filesystem) {
    void* root = nullptr;
    if (!safe_copy(&root, static_cast<char*>(filesystem) + 0x68, sizeof root) || !root) return {};
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return {};
    int over = 0, pc = 0;
    bool crashed = false;
    __try {
        over = mount_folder(filesystem, "game:\\mediaoverride\\", 0x7FFFFFFE);
        pc   = mount_folder(filesystem, "game:\\mediapc\\", 0x7FFFFFFD);
        reinterpret_cast<Method>((*reinterpret_cast<void***>(filesystem))[0x1F8 / 8])(filesystem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        crashed = true;
    }
    g_state.store(2);
    if (crashed) return "media overrides: mounting the folders crashed; the game's own files are used";
    return std::string("media overrides: game:\\mediaoverride\\ ") + (over ? "mounted" : "NOT mounted") +
           ", game:\\mediapc\\ " + (pc ? "mounted" : "NOT mounted") + ", over game:\\media\\ (first lookup was #" +
           std::to_string(g_lookups.load()) + ")";
}

uint8_t resolve(void* filesystem, void** volume, void* relative, const void* path, uint8_t a, uint8_t b, uint8_t c,
                void** mount, uint8_t d) {
    g_lookups.fetch_add(1, std::memory_order_relaxed);
    if (g_state.load(std::memory_order_acquire) == 0) {
        const std::string line = mount_all(filesystem);
        if (!line.empty()) plog(line);
    }
    return g_resolve(filesystem, volume, relative, path, a, b, c, mount, d);
}

// ---- the exe on disk, to check the game's bytes before using them ----

struct Disk {
    std::vector<char> head;
    std::vector<IMAGE_SECTION_HEADER> sections;
    HANDLE file = INVALID_HANDLE_VALUE;
};

bool open_disk(Disk& d) {
    const fs::path exe = game_mount().empty() ? azimuth_dir() / L"forza_durango_release_final.exe"
                                              : game_mount() / L"forza_durango_release_final.exe";
    d.file = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (d.file == INVALID_HANDLE_VALUE) return false;
    d.head.resize(0x1000);
    DWORD got = 0;
    if (!ReadFile(d.file, d.head.data(), 0x1000, &got, nullptr) || got != 0x1000) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d.head.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x800) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(d.head.data() + dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != kTimestamp || nt->OptionalHeader.SizeOfImage != kGameSizeOfImage) return false;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    d.sections.assign(sec, sec + nt->FileHeader.NumberOfSections);
    return true;
}

bool disk_bytes(const Disk& d, uint32_t rva, void* out, DWORD n) {
    for (const auto& s : d.sections) {
        if (rva < s.VirtualAddress || rva + n > s.VirtualAddress + s.SizeOfRawData) continue;
        const uint64_t off = s.PointerToRawData + (rva - s.VirtualAddress);
        OVERLAPPED ov{};
        ov.Offset     = static_cast<DWORD>(off);
        ov.OffsetHigh = static_cast<DWORD>(off >> 32);
        DWORD got     = 0;
        return ReadFile(d.file, out, n, &got, &ov) && got == n;
    }
    return false;
}

bool same_as_disk(const Disk& d, uint32_t rva, DWORD n) {
    unsigned char disk[32], mem[32];
    if (n > sizeof disk || !disk_bytes(d, rva, disk, n)) return false;
    return safe_copy(mem, g_base + rva, n) && std::memcmp(disk, mem, n) == 0;
}

// The folder vtable holds absolute pointers, so when the image isn't at its preferred base (Azimuth 1.0.1 under
// Wine maps it elsewhere and relocates it) they differ from the file by the relocation delta.
bool same_pointers_as_disk(const Disk& d, uint32_t rva, int count) {
    uint64_t disk[2], mem[2];
    if (count > 2 || !disk_bytes(d, rva, disk, count * 8)) return false;
    __try {
        std::memcpy(mem, g_base + rva, count * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uint64_t delta = reinterpret_cast<uint64_t>(g_base) - kPreferredBase;
    for (int i = 0; i < count; ++i)
        if (mem[i] - delta != disk[i]) return false;
    return true;
}

bool header_is_game(const char* base) {
    // A copy of the headers' page, as Azimuth may still be writing (or unmapping) it.
    static char head[0x1000];
    if (!safe_copy(head, base, sizeof head)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(head);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x800) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(head + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kTimestamp &&
           nt->OptionalHeader.SizeOfImage == kGameSizeOfImage;
}

bool committed(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
           !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

bool executable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

uint64_t read_slot(void** slot) {
    uint64_t v = 0;
    return safe_copy(&v, slot, sizeof v) ? v : 0;
}

void watch() {
    // Find the image as soon as it's there: its preferred base every
    // millisecond, a full search now and then.
    char* base = nullptr;
    for (unsigned i = 0; !base; ++i) {
        if (launched_game() == Launched::other) return; // not FH2: its files are left to it
        const auto* pref = reinterpret_cast<const char*>(kPreferredBase);
        if (committed(pref) && header_is_game(pref)) base = const_cast<char*>(pref);
        else if (i % 50 == 49) base = static_cast<char*>(find_game());
        if (!base) Sleep(1);
    }
    const ULONGLONG mapped = GetTickCount64();
    g_base = base;
    void** slot              = reinterpret_cast<void**>(base + RVA_RESOLVE_SLOT);
    const uint64_t original  = reinterpret_cast<uint64_t>(base + RVA_RESOLVE);

    // Wait for the slot to hold the resolver: the sections may still be
    // being copied in.
    uint64_t now = 0;
    while ((now = read_slot(slot)) != original) {
        if (GetTickCount64() - mapped > 30000) {
            plog("media overrides: the game's file lookup isn't the one expected (game+0x2E6EF28 holds 0x" +
                 [&] { char b[24]; std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(now)); return std::string(b); }() +
                 "); left alone");
            return;
        }
        Sleep(0);
    }
    const bool code_was_running = executable(base + RVA_RESOLVE);

    Disk disk;
    if (!open_disk(disk)) {
        plog("media overrides: couldn't read the game exe to check it against; left alone");
        if (disk.file != INVALID_HANDLE_VALUE) CloseHandle(disk.file);
        return;
    }
    const char* differs = !same_as_disk(disk, RVA_RESOLVE, 16)          ? "game+0x11F49B0 (resolver)"
                          : !same_as_disk(disk, RVA_ALLOCATE, 16)         ? "game+0x3229C0 (allocator)"
                          : !same_as_disk(disk, RVA_CONSTRUCT, 16)        ? "game+0x11E05A0 (folder volume constructor)"
                          : !same_pointers_as_disk(disk, RVA_FOLDER_VT + 8, 2) ? "game+0x2EAB918 (folder volume vtable)"
                                                                          : nullptr;
    CloseHandle(disk.file);
    if (differs) {
        plog("media overrides: the game's code isn't what this build expects; left alone");
        plog(std::string("media overrides: ") + differs + " differs from the exe on disk");
        return;
    }

    g_resolve = reinterpret_cast<Resolve>(original);
    // The folder volume's methods, copied before the swap: mount_all can run on a game thread right after it.
    std::memcpy(g_folder_vtable, base + RVA_FOLDER_VT, sizeof g_folder_vtable);
    g_exists                      = reinterpret_cast<Exists>(g_folder_vtable[1 + 0xE8 / 8]);
    g_folder_vtable[1 + 0xE8 / 8] = reinterpret_cast<void*>(&folder_exists);
    DWORD old = 0;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        plog("media overrides: couldn't change the game's file lookup (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    const bool swapped = InterlockedCompareExchangePointer(slot, reinterpret_cast<void*>(&resolve),
                                                           reinterpret_cast<void*>(original)) ==
                         reinterpret_cast<void*>(original);
    VirtualProtect(slot, 8, old, &old);
    plog(std::string("media overrides: ") +
         (swapped ? "hooked the game's file lookup " : "couldn't hook the game's file lookup (it changed) ") +
         std::to_string(GetTickCount64() - mapped) + " ms after the game was mapped; its code was " +
         (code_was_running ? "already executable" : "not executable yet"));
}

// Extra Options' winmm.dll next to azimuth.exe loads on Windows (the app folder comes first); under Wine only when
// WINEDLLOVERRIDES puts native winmm first ("winmm=n,b", "winmm,dxgi=n,b", ...); otherwise Wine's builtin loads.
bool extra_options_will_load() {
    HANDLE f = CreateFileW((azimuth_dir() / L"winmm.dll").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string data(4 * 1024 * 1024, '\0');
    DWORD got = 0;
    if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr)) got = 0;
    CloseHandle(f);
    data.resize(got);
    if (data.find("Fh2TweaksStatus") == std::string::npos) return false;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return true;
    char env[1024];
    const DWORD n = GetEnvironmentVariableA("WINEDLLOVERRIDES", env, sizeof env);
    if (!n || n >= sizeof env) return false;
    // Entries are "name[,name...]=modes", separated by ';'; native first means the modes start with 'n'.
    std::string all(env, n);
    for (char& c : all) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (size_t at = 0; at <= all.size();) {
        size_t end = all.find(';', at);
        if (end == std::string::npos) end = all.size();
        const std::string entry = all.substr(at, end - at);
        at = end + 1;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos || eq + 1 >= entry.size() || entry[eq + 1] != 'n') continue;
        const std::string names = "," + entry.substr(0, eq) + ",";
        if (names.find(",winmm,") != std::string::npos || names.find(",winmm.dll,") != std::string::npos) return true;
    }
    return false;
}

} // namespace

void start_media_overrides() {
    if (!extra_options_dir().empty()) {
        plog("media overrides: Extra Options is here, so mediaoverride is left to it");
        return;
    }
    if (extra_options_will_load()) {
        // Its winmm.dll can load after this DLL (seen under Proton). Swapping the resolver first makes Extra Options
        // refuse every patch ("unexpected loaded bytes at media_override_native_resolver"), so leave it to it.
        plog("media overrides: Extra Options' winmm.dll is next to azimuth.exe and will load; waiting for it");
        std::thread([] {
            for (int i = 0; i < 300; ++i, Sleep(100))
                if (!extra_options_dir().empty()) {
                    plog("media overrides: Extra Options loaded, so mediaoverride is left to it");
                    return;
                }
            plog("media overrides: Extra Options' winmm.dll didn't load within 30 s; mediaoverride isn't mounted");
        }).detach();
        return;
    }
    plog("media overrides: Extra Options isn't loaded, so the platform mounts mediaoverride and mediapc itself");
    std::thread(watch).detach();
}

} // namespace fh2platform

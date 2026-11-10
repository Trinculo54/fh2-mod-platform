// Media overrides when Extra Options isn't loaded (platform 2.7.3).
//
// Extra Options makes the game read files from <Mount>\mediaoverride (and
// \mediapc) ahead of its own media, and every platform service that writes
// files relies on that. When it isn't loaded (Azimuth 1.0.1, which it
// doesn't support yet, or Wine without the winmm override), the platform
// does the same itself. Only then, because Extra Options checks every place
// it patches and refuses all of its patches if any one was changed first,
// this one included. Extra Options' winmm.dll is loaded before the platform
// starts (azimuth.exe imports winmm.dll), so whether it's loaded is decided
// then. Under Wine, when that can't be told and Wine is set to load it
// (extra_options_expected), it counts as loaded.
//
// Under Proton the game isn't mapped at 0x140000000, so the pointers the
// checks below compare with the exe on disk are moved by the same amount
// (wine.cpp's same_pointer).
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
// When this Azimuth folder has data\titles\1A96545B, where Azimuth keeps the
// game's T:\ drive (1.0 does, and so does 0.9.10), with the Turn10Temp.scratch
// folder the game makes on T:\ in its tmp folder (so T:\ is there), it also
// mounts this Azimuth folder's own folders ahead of mediaoverride
// (car_packs.cpp): its layer, where the platform's files go, and the car
// packs, both on T:\. Whether it does is decided once, at start
// (instance_layer), and the log says why not. The game reads only through
// Azimuth's drives: a game:\ path with ".." stops the game, and a host path
// isn't found. Without the layer, mediaoverride and mediapc keep the
// priorities they had up to 2.8.0.
//
// With Extra Options loaded and car packs to read (platform 2.9.0, eo_packs in
// car_packs.cpp), the platform chains behind it instead (watch_eo). Extra
// Options writes its own lookup into the slot while Azimuth maps the game,
// before the game's code runs, and its lookup for a sound bank's path on disk
// into the slot at game+0x2E6EEC8 just after it; it checks its sites only
// before writing them. As each comes to hold Extra Options' pointer, the
// platform swaps it for its own (resolve_eo, disk_path_eo), which call Extra
// Options' in turn. On the first lookup (of either kind) the platform mounts
// the car pack folders, then T:\fh2-platform\eo-media (the packs' merged
// zipmanifest.xml) at 0x7FFFFFFF, ahead of everything Extra Options mounts
// (mediaoverride 0x7FFFFFFE, mediapc 0x7FFFFFFD, its carbin cache
// 0x7FFFFFFC, its mods from 0x70000001), before passing the lookup on, so
// Extra Options mounts its folders after the platform's. Nothing on that path
// waits: a lookup on another thread meanwhile goes straight on. Each step is
// checked by what it does, from the game's list of volumes (volume_walk.hpp):
// if Extra Options' folders are there before the platform's first lookup,
// nothing is mounted; if after it eo-media isn't alone at the top with
// Extra Options' folders below it, eo-media answers that it has no file from
// then on (as it does whenever the packs are lost later). Either way
// the packs aren't in the game this session (car_packs_lost), and the game
// reads what it would without them.
//
// Addresses are for the 24 Nov 2015 exe, RVAs from its base:
//   0x2E6EF28  file system vtable slot (+0x210): the resolver
//   0x2E6EEC8  file system vtable slot (+0x1B0): a file's path on disk, which
//              Extra Options takes for .bank files
//   0x11F49B0  the resolver
//   0x11F1460  the path on disk (it asks the resolver, through the slot)
//   0x03229C0  the game's operator new
//   0x11E05A0  folder volume constructor (this, const string& folder, bool)
//   0x2EAB910  folder volume vtable, preceded by its RTTI locator
//   0x11F83A2  in the mount: the volume list entry pushed onto fs+0x30
//   0x11FB550  the volume sort's comparator (unsigned priority, highest first)
// File system methods used (vtable offsets): +0x1E8 mount a volume, +0x1F8
// sort the volumes; root volume pointer at +0x68 (zero until it's set up),
// the list of volumes from +0x30 to +0x38 (volume_walk.hpp).
// Folder volume methods: +0x10 add a reference, +0x18 release, +0xE8 does a
// file exist; the folder it reads at +0x28.
#include "platform.hpp"
#include "text.hpp"
#include "updates.hpp"
#include "volume_walk.hpp"
#include "wine.hpp"

#include <windows.h>

#include <algorithm>
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
constexpr uint32_t RVA_DISK_PATH_SLOT = 0x2E6EEC8;
constexpr uint32_t RVA_DISK_PATH      = 0x11F1460;
constexpr uint32_t RVA_MOUNT_PUSH     = 0x11F83A2;
constexpr uint32_t RVA_PRIORITY_LESS  = 0x11FB550;
// What the two of those the platform only reads about have, in the exe on disk too.
constexpr unsigned char kMountPush[]    = {0x48, 0x8D, 0x55, 0xC0, 0x48, 0x8D, 0x4F, 0x30, 0xE8, 0x81, 0xA9, 0x01, 0x00};
constexpr unsigned char kPriorityLess[] = {0x8B, 0x12, 0x39, 0x11, 0x0F, 0x97, 0xC0, 0xC3};

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

// `vtable`: the volume's methods (the folder volume's, with the platform's "does a file exist"). `made`: where the
// volume is noted while it's mounted (with Extra Options, eo-media's), set before the game can ask it anything.
int mount_folder(void* filesystem, const char* folder, unsigned priority, void** vtable = g_folder_vtable + 1,
                 std::atomic<void*>* made = nullptr) {
    void* storage = reinterpret_cast<Allocate>(g_base + RVA_ALLOCATE)(0x50);
    if (!storage) return 0;
    const GameString source = game_string(folder), target = game_string("game:\\media\\");
    void* volume = reinterpret_cast<Construct>(g_base + RVA_CONSTRUCT)(storage, &source, 1);
    *reinterpret_cast<void***>(volume) = vtable;
    void** methods = *reinterpret_cast<void***>(volume);
    reinterpret_cast<Method>(methods[0x10 / 8])(volume);
    if (made) made->store(volume, std::memory_order_release);
    void** fs_methods = *reinterpret_cast<void***>(filesystem);
    const int ok      = reinterpret_cast<Mount>(fs_methods[0x1E8 / 8])(filesystem, &target, &volume, priority, 0, 0, nullptr);
    if (made && !ok) made->store(nullptr, std::memory_order_release);
    reinterpret_cast<Method>(methods[0x18 / 8])(volume);
    return ok;
}

// A folder of this Azimuth folder's, as the game spells it, and its priority (above mediaoverride's).
struct Outside {
    std::string label, folder;
    unsigned priority;
};

struct Mounted {
    std::string label, folder;
    int ok;
};

// Called inside mount_guarded's __try (which has no objects with destructors).
void mount_outside(void* filesystem, const std::vector<Outside>& folders, std::vector<Mounted>& out) {
    for (const auto& f : folders)
        out.push_back({f.label, f.folder, mount_folder(filesystem, f.folder.c_str(), f.priority)});
}

// This folder's layer, then the packs, in name order: where two packs have the same file, the
// first one's is read (and the merged manifest has the first one's zip lines too). mediaoverride and mediapc go
// below them only with the layer; without it they keep 2.8.0's priorities (kOldOverridePriority, then one below).
constexpr unsigned kLayerPriority = 0x7FFFFFFE, kFirstPackPriority = 0x7FFFFFF0, kOverridePriority = 0x7FFFFF00;
constexpr unsigned kOldOverridePriority = 0x7FFFFFFE;

unsigned override_priority() { return instance_layer() ? kOverridePriority : kOldOverridePriority; }

std::vector<Outside> outside_folders() {
    std::vector<Outside> f;
    if (!instance_layer()) return f;
    if (const auto layer = temp_drive_spelling(instance_media()); !layer.empty())
        f.push_back({"this folder's layer", layer, kLayerPriority});
    unsigned priority = kFirstPackPriority;
    for (const auto& p : car_packs()) {
        if (priority <= kOverridePriority) {
            plog("media overrides: more car packs than there's room for; " + p.name + " and the ones after it aren't read");
            break;
        }
        if (const auto folder = temp_drive_spelling(p.mirror); !folder.empty())
            f.push_back({"car pack " + p.name, folder, priority--});
    }
    return f;
}

int mount_guarded(void* filesystem, const std::vector<Outside>& folders, std::vector<Mounted>& outside, unsigned priority,
                  int& over, int& pc) {
    __try {
        mount_outside(filesystem, folders, outside);
        over = mount_folder(filesystem, "game:\\mediaoverride\\", priority);
        pc   = mount_folder(filesystem, "game:\\mediapc\\", priority - 1);
        reinterpret_cast<Method>((*reinterpret_cast<void***>(filesystem))[0x1F8 / 8])(filesystem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

// Returns a line for the log, or "" when it's too early (no root volume yet).
std::string mount_all(void* filesystem) {
    void* root = nullptr;
    if (!safe_copy(&root, static_cast<char*>(filesystem) + 0x68, sizeof root) || !root) return {};
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return {};
    int over = 0, pc = 0;
    std::vector<Mounted> outside;
    const auto folders = outside_folders();
    const bool crashed = !mount_guarded(filesystem, folders, outside, override_priority(), over, pc);
    g_state.store(2);
    for (const auto& m : outside)
        plog("media overrides: " + m.label + " " + m.folder + (m.ok ? " mounted" : " NOT mounted"));
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

// The same for `count` absolute pointers (the folder volume's vtable): where the image isn't mapped at
// its preferred base, as under Proton, each one differs from the file's by how far the image moved.
// (The code checked with same_as_disk has nothing the loader changes.)
bool same_pointers_as_disk(const Disk& d, uint32_t rva, int count) {
    uint64_t disk[4], mem[4];
    if (count < 1 || count > 4 || !disk_bytes(d, rva, disk, count * 8) || !safe_copy(mem, g_base + rva, count * 8))
        return false;
    for (int i = 0; i < count; ++i)
        if (!same_pointer(disk[i], mem[i], kPreferredBase, reinterpret_cast<uint64_t>(g_base), kGameSizeOfImage))
            return false;
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
    if (reinterpret_cast<uintptr_t>(base) != kPreferredBase) { // under Proton, for one
        char at[24];
        std::snprintf(at, sizeof at, "0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base)));
        plog(std::string("media overrides: the game is mapped at ") + at +
             ", not at 0x140000000; the pointers in it are checked with that taken into account");
    }
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
    const char* differs = nullptr;
    if (!same_as_disk(disk, RVA_RESOLVE, 16)) differs = "game+0x11F49B0 (the resolver)";
    else if (!same_as_disk(disk, RVA_ALLOCATE, 16)) differs = "game+0x3229C0 (operator new)";
    else if (!same_as_disk(disk, RVA_CONSTRUCT, 16)) differs = "game+0x11E05A0 (the folder volume constructor)";
    else if (!same_pointers_as_disk(disk, RVA_FOLDER_VT + 8, 2)) differs = "game+0x2EAB918 (the folder volume's vtable)";
    CloseHandle(disk.file);
    if (differs) {
        plog("media overrides: the game's code isn't what this build expects; left alone");
        plog(std::string("media overrides: ") + differs + " isn't the same as in the exe on disk");
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

// ---- with Extra Options: the car packs, chained behind its lookups (platform 2.9.0) -----------------------------
//
// Every line this part logs starts "car packs with Extra Options:", so a log shows the whole chain with one search.

using DiskPath = void* (*)(void*, const GameString*, GameString*);

// eo-media is at kOverlayPriority (volume_walk.hpp), one above Extra Options' mediaoverride; the packs go from here
// down, above its mods' folders and zips (0x70000001 up).
constexpr unsigned kEoFirstPackPriority = 0x7FFFFFF0, kEoFloor = 0x7F000000;

std::atomic<Resolve> g_eo_resolve{nullptr};       // Extra Options' file lookup, which the platform's passes each one on to
std::atomic<DiskPath> g_eo_disk_path{nullptr};    // and its lookup of a file's path on disk (it takes it for .bank files)
uintptr_t g_eo_lo = 0, g_eo_hi = 0;               // its image; set before either lookup is swapped
void* g_overlay_vtable[1 + 0x1F0 / sizeof(void*)];   // eo-media's volume's methods: the folder volume's, with overlay_exists
std::vector<Outside> g_eo_packs;                  // the packs' folders and priorities; set before either lookup is swapped
std::string g_eo_overlay;                         // eo-media as the game spells it; the same
std::atomic<void*> g_overlay{nullptr};            // eo-media's volume, once it's mounted
std::atomic<bool> g_overlay_hidden{false};        // its files aren't there for the game any more this session
std::atomic<bool> g_overlay_ready{false};         // the platform's first write is done: its database and names are this session's
std::atomic<bool> g_overlay_has_db{false};        // and it has a database
std::atomic<bool> g_eo_lost{false};               // the packs aren't in the game this session after all
std::atomic<std::string*> g_lost_why{nullptr};    // why, from a game thread, for watch_eo to pass on (car_packs_lost)
std::atomic<bool> g_in_front{false};              // the precedence check passed
std::atomic<bool> g_precedence_checked{false};
std::atomic<DWORD> g_mount_thread{0};             // the game thread that mounted them
std::atomic<bool> g_watching{true};               // still looking at paths for the three files below
std::atomic<bool> g_answered[3];                  // zipmanifest.xml, gamedbRC.slt, the first Data_Car.str: logged
std::atomic<ULONGLONG> g_chained_at{0};           // GetTickCount64 when the platform's lookup went in

std::string hex(uint64_t v, int digits = 0) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%0*llX", digits, static_cast<unsigned long long>(v));
    return b;
}

bool in_eo(uint64_t p) { return g_eo_lo && p >= g_eo_lo && p < g_eo_hi; }

// From a game thread: the packs aren't in the game after all. eo-media stops answering at once; watch_eo, which looks
// every 50 ms, tells the rest of the platform (car_packs_lost takes a lock and writes a file, which a lookup mustn't
// wait for). The first reason is the one kept.
void lose_later(const std::string& why) {
    g_overlay_hidden.store(true, std::memory_order_release);
    if (g_eo_lost.exchange(true)) return;
    auto* text         = new std::string(why);
    std::string* none = nullptr;
    if (!g_lost_why.compare_exchange_strong(none, text)) delete text;
}

// From watch_eo itself.
void lose_now(const std::string& why) {
    g_overlay_hidden.store(true, std::memory_order_release);
    g_eo_lost.store(true);
    car_packs_lost(why);
}

bool ends_with(const char* text, std::size_t n, const char* end);

// eo-media's own "does a file exist": no for everything once the packs are lost, so the game reads what it would
// without them. Answering is all it does; nothing here waits. Its database and its cars' names are last session's
// (or not there yet) until the platform's first write is done, which is normally seconds before the game is mapped.
// If the game asks for either before that, it isn't told to wait: the packs are lost for the session, and it reads the
// database and names it would without them, which go together.
uint8_t overlay_exists(void* volume, const GameString* path) {
    if (g_overlay_hidden.load(std::memory_order_acquire)) return 0;
    if (!g_overlay_ready.load(std::memory_order_acquire)) {
        const char* text = path->capacity < 16 ? reinterpret_cast<const char*>(path) : path->data;
        if (ends_with(text, path->length, "gamedbrc.slt") || ends_with(text, path->length, "data_car.str")) {
            lose_later(later("The game asked for its database before the car packs' was ready."));
            return 0;
        }
    }
    return folder_exists(volume, path);
}

// The text of one of the game's strings (an MSVC std::string) into `out`, through guarded reads: its length, 0 when
// it can't be read or is longer than a path. No memory is asked for: this runs on a game thread, for every lookup at
// first.
std::size_t game_text(const void* s, char* out, std::size_t cap) {
    GameString g{};
    out[0] = '\0';
    if (!s || !safe_copy(&g, s, sizeof g) || !g.length || g.length >= cap) return 0;
    const void* from = g.capacity < 16 ? s : static_cast<const void*>(g.data);
    if (!safe_copy(out, from, g.length)) return out[0] = '\0', 0;
    out[g.length] = '\0';
    return g.length;
}

bool ends_with(const char* text, std::size_t n, const char* end) {
    const std::size_t m = std::strlen(end);
    if (n < m) return false;
    for (std::size_t i = 0; i < m; ++i)
        if (std::tolower(static_cast<unsigned char>(text[n - m + i])) != end[i]) return false;
    return true;
}

bool is_folder_volume(uint64_t vtable) {
    return vtable == reinterpret_cast<uint64_t>(g_base + RVA_FOLDER_VT + 8) || vtable == reinterpret_cast<uint64_t>(g_folder_vtable + 1) ||
           vtable == reinterpret_cast<uint64_t>(g_overlay_vtable + 1) || in_eo(vtable);
}

// The game's list of volumes as it is now (volume_walk.hpp), through guarded reads only. Another thread may be
// mounting meanwhile: a list that doesn't add up then comes back as not read, never as something to act on.
VolumeList read_volumes(void* filesystem) {
    VolumeList list;
    uint64_t range[2] = {};
    if (!safe_copy(range, static_cast<char*>(filesystem) + 0x30, sizeof range)) {
        list.problem = "where it starts and ends couldn't be read";
        return list;
    }
    if (range[1] < range[0] || range[1] - range[0] > kMaxVolumes * kVolumeEntrySize) {
        list.problem = "its start and end (" + hex(range[0]) + ", " + hex(range[1]) + ") aren't a list's";
        return list;
    }
    const std::size_t n = static_cast<std::size_t>(range[1] - range[0]);
    std::vector<unsigned char> bytes(n);
    if (n && !safe_copy(bytes.data(), reinterpret_cast<const void*>(range[0]), n)) {
        list.problem = "its entries couldn't be read";
        return list;
    }
    list = parse_volume_list(bytes.data(), n);
    for (VolumeEntry& e : list.entries) {
        e.vtable_read = e.volume && safe_copy(&e.vtable, reinterpret_cast<const void*>(e.volume), sizeof e.vtable);
        if (!e.vtable_read) {
            list.read = false;
            if (list.problem.empty()) list.problem = "a volume in it (" + hex(e.volume) + ") couldn't be read";
            continue;
        }
        char folder[264];
        if (is_folder_volume(e.vtable) && game_text(reinterpret_cast<const char*>(e.volume) + 0x28, folder, sizeof folder)) e.folder = folder;
    }
    return list;
}

VolumeOwners owners() { return {g_eo_lo, g_eo_hi, reinterpret_cast<uint64_t>(g_overlay.load(std::memory_order_acquire))}; }

// Whose a volume is, for the log.
std::string whose(uint64_t volume, uint64_t vtable, const std::string& folder) {
    if (volume && volume == reinterpret_cast<uint64_t>(g_overlay.load(std::memory_order_acquire))) return "eo-media (" + folder + ")";
    if (vtable == reinterpret_cast<uint64_t>(g_folder_vtable + 1) || vtable == reinterpret_cast<uint64_t>(g_overlay_vtable + 1))
        return "the platform's " + folder;
    if (in_eo(vtable)) return "Extra Options' " + folder;
    if (!folder.empty()) return "the game's folder " + folder;
    return "a zip or other volume of the game's kind";
}

std::string describe_volume(const void* volume) {
    uint64_t vtable = 0;
    if (!volume || !safe_copy(&vtable, volume, sizeof vtable)) return "a volume that couldn't be read";
    char folder[264] = "";
    if (is_folder_volume(vtable)) game_text(static_cast<const char*>(volume) + 0x28, folder, sizeof folder);
    return whose(reinterpret_cast<uint64_t>(volume), vtable, folder);
}

// The list for the log, in the order the game reads them: every volume at 0x70000000 or above (the platform's and
// Extra Options'), and how many the game's own are (5 to 0x96).
std::string volumes_line(const VolumeList& list) {
    std::string s;
    int own = 0;
    for (const VolumeEntry& e : list.entries) {
        if (e.priority < 0x70000000u && !in_eo(e.vtable)) {
            ++own;
            continue;
        }
        s += (s.empty() ? "" : "; ") + hex(e.priority, 8) + " " + (e.vtable_read ? whose(e.volume, e.vtable, e.folder) : "a volume that couldn't be read");
    }
    return (s.empty() ? std::string("none at 0x70000000 or above") : s) + "; and " + std::to_string(own) + " of the game's own below them";
}

// Everything mount_guarded_eo's __try covers is in this call: the packs' folders, then eo-media only if every one of
// them mounted (its zip list names files in them), then the game's own sort.
void mount_eo(void* filesystem, std::vector<Mounted>& mounted, int& overlay) {
    mount_outside(filesystem, g_eo_packs, mounted);
    bool all = true;
    for (const Mounted& m : mounted) all = all && m.ok;
    if (all) overlay = mount_folder(filesystem, g_eo_overlay.c_str(), kOverlayPriority, g_overlay_vtable + 1, &g_overlay);
    reinterpret_cast<Method>((*reinterpret_cast<void***>(filesystem))[0x1F8 / 8])(filesystem);
}

int mount_guarded_eo(void* filesystem, std::vector<Mounted>& mounted, int& overlay) {
    __try {
        mount_eo(filesystem, mounted, overlay);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

// The first lookup of either kind once the game's file system is ready (its root volume is set, which Extra Options
// waits for too before it mounts anything): the platform's folders go in before the lookup goes on to Extra Options,
// which sets up its own on its first. It runs once, on the game thread that got there first, and nothing waits for
// it: a lookup on another thread meanwhile goes straight on.
void mount_before_eo(void* filesystem, int n, const char* kind) {
    void* root = nullptr;
    if (!safe_copy(&root, static_cast<char*>(filesystem) + 0x68, sizeof root) || !root) return;
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) return;
    g_mount_thread.store(GetCurrentThreadId());
    const VolumeList before = read_volumes(filesystem);
    const Classified was    = classify(before, owners());
    plog("car packs with Extra Options: first lookup: #" + std::to_string(n) + ", " + kind + ", " +
         std::to_string(GetTickCount64() - g_chained_at.load()) + " ms after the platform chained; the game had " +
         (before.read ? std::to_string(before.entries.size()) + " volume(s) then, " + std::to_string(was.eo_volumes) + " of them Extra Options'"
                      : "a list of volumes that couldn't be read then (" + before.problem + ")"));
    if (g_eo_lost.load()) {
        plog("car packs with Extra Options: nothing is mounted: the packs were already lost this session");
    } else if (!was.readable) {
        plog("car packs with Extra Options: nothing is mounted: whether Extra Options had set up its folders already can't be told");
        lose_later(later("The game's list of folders couldn't be checked."));
    } else if (volumes_late(was)) {
        plog("car packs with Extra Options: LATE: Extra Options had already set up its folders before the platform's first lookup, so "
             "a lookup reached it before the platform chained; nothing is mounted. The volumes: " + volumes_line(before));
        lose_later(later("Extra Options had set up its folders before the car packs' were added."));
    } else {
        std::vector<Mounted> mounted;
        int overlay        = 0;
        const bool crashed = !mount_guarded_eo(filesystem, mounted, overlay);
        for (std::size_t i = 0; i < mounted.size(); ++i)
            plog("car packs with Extra Options: " + mounted[i].label + " " + mounted[i].folder +
                 (mounted[i].ok ? " mounted at " + hex(g_eo_packs[i].priority, 8) : " NOT mounted"));
        if (crashed) {
            plog("car packs with Extra Options: mounting the folders crashed; eo-media isn't read");
            lose_later(later("A car pack's folder couldn't be added to the game's folders."));
        } else if (!overlay) {
            plog("car packs with Extra Options: eo-media " + g_eo_overlay + " NOT mounted" +
                 (mounted.size() == g_eo_packs.size() && std::all_of(mounted.begin(), mounted.end(), [](const Mounted& m) { return m.ok != 0; })
                      ? std::string{}
                      : std::string(": a pack's folder didn't mount, and its zip list names files in every one of them")));
            lose_later(later("A car pack's folder couldn't be added to the game's folders."));
        } else {
            plog("car packs with Extra Options: eo-media " + g_eo_overlay + " mounted at " + hex(kOverlayPriority, 8) + ", ahead of every other "
                 "folder, with " + std::to_string(mounted.size()) + " car pack folder(s) below it; the lookup goes on to Extra Options now");
        }
    }
    g_state.store(2, std::memory_order_release);
}

// Once, after the lookup the platform mounted on has been to Extra Options and back (it sets up its folders on its
// first): Extra Options' folders must be there, and eo-media alone at 0x7FFFFFFF or above. If not, what the packs'
// zip list was built on isn't what the game reads below it, so eo-media stops answering.
void check_precedence(void* filesystem) {
    if (g_precedence_checked.exchange(true)) return;
    const VolumeList list = read_volumes(filesystem);
    const Classified c    = classify(list, owners());
    const std::string volumes = list.read ? volumes_line(list) : "the list couldn't be read (" + list.problem + ")";
    if (overlay_in_front(c)) {
        g_in_front.store(true, std::memory_order_release);
        plog("car packs with Extra Options: precedence: PASS: eo-media is alone at the top, with " + std::to_string(c.eo_volumes) +
             " folder(s) of Extra Options' below it. The volumes, first read first: " + volumes);
        return;
    }
    const std::string why = !c.readable     ? "the game's list of volumes couldn't be read"
                            : !c.overlay     ? "eo-media isn't in the game's list of volumes"
                            : !c.eo_volumes ? "Extra Options has set up no folder of its own"
                                             : "eo-media isn't alone at the top (" + c.above + " is at its priority or above)";
    plog("car packs with Extra Options: precedence: FAIL: " + why + ", so eo-media's files are hidden from the game for this session. "
         "The volumes, first read first: " + volumes);
    lose_later(later("The car packs' folder isn't ahead of Extra Options' folders."));
}

// What every lookup on this path does before it goes on to Extra Options: the first 8 are logged with their thread,
// and the first with the file system ready mounts. Returns the lookup's number.
int before_eo_lookup(void* filesystem, const void* path, bool disk_path) {
    const int n = g_lookups.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool first = g_state.load(std::memory_order_acquire) == 0;
    if (n > 8 && !first) return n;
    char text[264];
    const std::size_t len  = game_text(path, text, sizeof text);
    const bool bank        = ends_with(text, len, ".bank");
    const std::string kind = disk_path ? (bank ? "a sound bank's path on disk" : "a file's path on disk") : "a file";
    if (n <= 8)
        plog("car packs with Extra Options: lookup #" + std::to_string(n) + " (" + kind + ") on thread " + std::to_string(GetCurrentThreadId()) +
             ": " + (len ? std::string(text) : std::string("(its path couldn't be read)")));
    if (first) mount_before_eo(filesystem, n, kind.c_str());
    return n;
}

// And after it has been there: the precedence check once (on the thread that mounted, whose lookup has now been to
// Extra Options since), and for zipmanifest.xml, gamedbRC.slt and the first
// Data_Car.str, which volume answered. The game must read the zip list from eo-media when it holds one; the database
// and the cars' names aren't kept there in this build, so they're only logged.
void after_eo_lookup(void* filesystem, const void* path, int n, bool found, void** volume) {
    if (!g_precedence_checked.load(std::memory_order_relaxed) && g_overlay.load(std::memory_order_acquire) &&
        g_state.load(std::memory_order_acquire) == 2 && g_mount_thread.load() == GetCurrentThreadId())
        check_precedence(filesystem);
    if (!g_watching.load(std::memory_order_relaxed)) return;
    if (n > 200000) return g_watching.store(false);
    char text[264];
    const std::size_t len = game_text(path, text, sizeof text);
    static const char* const kNames[3] = {"zipmanifest.xml", "gamedbrc.slt", "data_car.str"};
    int which = -1;
    for (int i = 0; i < 3 && which < 0; ++i)
        if (ends_with(text, len, kNames[i])) which = i;
    if (which < 0 || g_answered[which].exchange(true)) return;
    if (g_answered[0].load() && g_answered[1].load() && g_answered[2].load()) g_watching.store(false);
    void* answered = nullptr;
    if (found && volume) safe_copy(&answered, volume, sizeof answered);
    const bool ours = answered && answered == g_overlay.load(std::memory_order_acquire);
    static const char* const kShown[3] = {"zipmanifest.xml", "gamedbRC.slt", "Data_Car.str"};
    const bool has_db = g_overlay_ready.load(std::memory_order_acquire) && g_overlay_has_db.load(std::memory_order_acquire);
    plog(std::string("car packs with Extra Options: ") + kShown[which] + " (lookup #" + std::to_string(n) + ", " + text + ") was " +
         (!found ? "not found" : ours ? "answered by eo-media" : "answered by " + describe_volume(answered) + ", NOT eo-media") +
         (which == 1 && !ours && !has_db ? "; eo-media has no database this session" : ""));
    if (which == 1 && !ours && has_db && ends_with(text, len, "media\\stripped\\gamedbrc.slt") && !g_eo_lost.load()) {
        plog("car packs with Extra Options: the game read its database without the packs' cars, so they aren't in the game this session");
        lose_later(later("The game read its database from another folder than the car packs'."));
    }
    if (which == 0 && !ours && car_packs_manifest() && ends_with(text, len, "media\\zipmanifest.xml") && !g_eo_lost.load()) {
        plog("car packs with Extra Options: the game read its zip list without the packs' zips, so they aren't in the game this session");
        lose_later(later("The game read its list of zips before the car packs' folder was added."));
    }
}

// A lookup the game makes from inside the platform's own mount, on the thread that is mounting. It goes to the game's
// own function, not on to Extra Options: Extra Options sets up its folders on the first lookup it sees, and that
// mustn't happen in the middle of the platform's mount, before eo-media is in. (Extra Options keeps its own nested
// lookups to itself the same way.) Both functions are among the ones compared with the exe before the chain is made.
bool mounting_here() {
    return g_state.load(std::memory_order_acquire) == 1 && g_mount_thread.load(std::memory_order_acquire) == GetCurrentThreadId();
}

// The platform's file lookup with Extra Options: Extra Options' own, with the platform's folders mounted first.
uint8_t resolve_eo(void* filesystem, void** volume, void* relative, const void* path, uint8_t a, uint8_t b, uint8_t c,
                   void** mount, uint8_t d) {
    if (mounting_here()) return reinterpret_cast<Resolve>(g_base + RVA_RESOLVE)(filesystem, volume, relative, path, a, b, c, mount, d);
    const int n         = before_eo_lookup(filesystem, path, false);
    const uint8_t found = g_eo_resolve.load(std::memory_order_acquire)(filesystem, volume, relative, path, a, b, c, mount, d);
    after_eo_lookup(filesystem, path, n, found != 0, volume);
    return found;
}

// And the lookup of a file's path on disk (game+0x2E6EEC8). Extra Options takes it for sound banks: for a .bank it
// sets up its folders and asks the game's resolver itself (game+0x11F49B0, not through the slot), so without this
// one the game's first sound bank would have it mount before the platform. Any other path goes on to the game's own
// (game+0x11F1460), which asks through the slot, and so comes through resolve_eo too.
void* disk_path_eo(void* filesystem, const GameString* path, GameString* output) {
    if (mounting_here()) return reinterpret_cast<DiskPath>(g_base + RVA_DISK_PATH)(filesystem, path, output);
    const int n  = before_eo_lookup(filesystem, path, true);
    void* result = g_eo_disk_path.load(std::memory_order_acquire)(filesystem, path, output);
    after_eo_lookup(filesystem, nullptr, n, false, nullptr);
    return result;
}

// The swap itself, in a call of its own so a fault in it is caught (the compare-and-swap is one instruction, and a
// __try only covers calls).
void* swap_pointer(void** slot, void* to, void* from) { return InterlockedCompareExchangePointer(slot, to, from); }
void* (*volatile g_swap)(void**, void*, void*) = swap_pointer;

// 1 swapped, 0 the slot held something else, -1 writing it faulted.
int swap_guarded(void** slot, void* to, void* from) {
    __try {
        return g_swap(slot, to, from) == from ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The slot's page can be written as it is: nothing changes its protection here, because Extra Options may be putting
// back the protection of the same pages at that moment. (The slots are in .data.)
bool writable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

// The file an address is in, for the log.
std::string module_of(uint64_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    wchar_t path[MAX_PATH];
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) == sizeof mbi && mbi.Type == MEM_IMAGE && mbi.AllocationBase)
        if (const DWORD n = GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH); n && n < MAX_PATH)
            return narrow(fs::path{path}.filename().wstring());
    return "memory that's no module's";
}

// Extra Options has finished with the game, one way or the other.
bool final_status(const std::string& s) {
    return s.rfind("OK", 0) == 0 || s.rfind("REFUSED", 0) == 0 || s.rfind("DISABLED", 0) == 0 || s.rfind("INACTIVE", 0) == 0;
}

// A slot's value, read twice a moment apart and the same both times (a pointer half written is never acted on).
uint64_t settled(void** slot) {
    const uint64_t a = read_slot(slot);
    Sleep(0);
    return read_slot(slot) == a ? a : 0;
}

// Reads a byte of every page of this DLL's image, so nothing it runs or reads between Extra Options' write and the
// swap has to come from the disk first. (Windows pages a DLL's code in as it's first run; in the hook test, with the
// DLL on a network share, the swap's own few instructions took up to 0.7 s that way.)
void page_in_own_image() {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(&page_in_own_image), &mbi, sizeof mbi) != sizeof mbi || !mbi.AllocationBase) return;
    const void* const image = mbi.AllocationBase;
    for (const char* at = static_cast<const char*>(image);
         VirtualQuery(at, &mbi, sizeof mbi) == sizeof mbi && mbi.AllocationBase == image && mbi.RegionSize;
         at = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        char byte = 0;
        for (SIZE_T page = 0; page < mbi.RegionSize; page += 0x1000) safe_copy(&byte, static_cast<const char*>(mbi.BaseAddress) + page, 1);
    }
}

// With Extra Options and car packs to read: finds the game as watch() does, checks the game's code, waits for Extra
// Options to put its own lookups in, swaps them for the platform's, and then keeps an eye on them.
void watch_eo() {
    const auto lost = [](const std::string& log, const std::string& why) {
        plog("car packs with Extra Options: " + log);
        lose_now(why);
    };
    // What the exe on disk has at the places the platform relies on, read now, before the game is mapped: once it is,
    // Extra Options is at work and the game follows, and there's no time to spend on a file. What the platform calls
    // to mount (as without Extra Options), what Extra Options' bank lookup goes on to, and the two places that say how
    // the list of volumes is laid out and sorted (those two also against the bytes this build was written for). None
    // of them is a place Extra Options changes.
    struct Site {
        uint32_t rva;
        DWORD n;
        const char* shown;
        const unsigned char* must;
        unsigned char disk[16];
    };
    Site sites[] = {{RVA_RESOLVE, 16, "game+0x11F49B0 (the resolver)", nullptr, {}},
                    {RVA_ALLOCATE, 16, "game+0x3229C0 (operator new)", nullptr, {}},
                    {RVA_CONSTRUCT, 16, "game+0x11E05A0 (the folder volume constructor)", nullptr, {}},
                    {RVA_DISK_PATH, 16, "game+0x11F1460 (a file's path on disk)", nullptr, {}},
                    {RVA_MOUNT_PUSH, sizeof kMountPush, "game+0x11F83A2 (the mount adding a volume to the list)", kMountPush, {}},
                    {RVA_PRIORITY_LESS, sizeof kPriorityLess, "game+0x11FB550 (the volumes' sort order)", kPriorityLess, {}}};
    uint64_t vtable_disk[2] = {};
    bool disk_read = false;
    {
        Disk disk;
        disk_read = open_disk(disk) && disk_bytes(disk, RVA_FOLDER_VT + 8, vtable_disk, sizeof vtable_disk);
        for (Site& s : sites) disk_read = disk_read && disk_bytes(disk, s.rva, s.disk, s.n);
        if (disk.file != INVALID_HANDLE_VALUE) CloseHandle(disk.file);
    }
    // Extra Options' image, from its PE header (found by its export, never by the name winmm.dll, which the system's
    // own has too), and what to mount, so a game thread has nothing to work out.
    const ExtraOptionsFacts eo = extra_options_facts();
    g_eo_lo                    = eo.lo;
    g_eo_hi                    = eo.hi;
    g_eo_overlay               = temp_drive_spelling(eo_media());
    unsigned priority          = kEoFirstPackPriority;
    for (const auto& p : car_packs()) {
        if (p.left_out) continue;
        if (priority <= kEoFloor) {
            plog("car packs with Extra Options: more car packs than there's room for; " + p.name + " and the ones after it aren't read");
            break;
        }
        if (const auto folder = temp_drive_spelling(p.mirror); !folder.empty()) g_eo_packs.push_back({"car pack " + p.name, folder, priority--});
    }
    // And the swap's own steps once on a pointer of the platform's, with this DLL's image paged in, so the real one
    // has nothing to wait for.
    page_in_own_image();
    void* practice = nullptr;
    if (settled(&practice) || !writable(&practice) || swap_guarded(&practice, &practice, nullptr) != 1)
        plog("car packs with Extra Options: a practice swap on a pointer of the platform's own didn't go as expected");

    char* base = nullptr;
    for (unsigned i = 0; !base; ++i) {
        if (launched_game() == Launched::other) return; // not FH2: its files are left to it
        const auto* pref = reinterpret_cast<const char*>(kPreferredBase);
        if (committed(pref) && header_is_game(pref)) base = const_cast<char*>(pref);
        else if (i % 50 == 49) base = static_cast<char*>(find_game());
        if (!base) Sleep(1);
    }
    const ULONGLONG mapped = GetTickCount64();
    const auto since       = [&] { return std::to_string(GetTickCount64() - mapped); };
    g_base                 = base;
    if (reinterpret_cast<uintptr_t>(base) != kPreferredBase)
        plog("car packs with Extra Options: the game is mapped at " + hex(reinterpret_cast<uintptr_t>(base)) +
             ", not at 0x140000000; the pointers in it are checked with that taken into account");
    void** slot             = reinterpret_cast<void**>(base + RVA_RESOLVE_SLOT);
    void** path_slot        = reinterpret_cast<void**>(base + RVA_DISK_PATH_SLOT);
    const uint64_t original = reinterpret_cast<uint64_t>(base + RVA_RESOLVE), path_original = reinterpret_cast<uint64_t>(base + RVA_DISK_PATH);
    const uint64_t ours     = reinterpret_cast<uint64_t>(&resolve_eo), path_ours = reinterpret_cast<uint64_t>(&disk_path_eo);

    if (!eo.lo || eo.hi <= eo.lo)
        return lost("Extra Options' image couldn't be read, so its lookup couldn't be told from another's; the game's file lookup is left alone",
                    later("The game's file lookup couldn't be changed."));
    if (!disk_read)
        return lost("couldn't read the game exe to check the game's code against; the game's file lookup is left alone",
                    later("The game's code isn't what this build expects."));

    // The slot holds the game's resolver once the sections are copied in, or Extra Options' already if the game was
    // found late. Anything else that stays is someone else's.
    uint64_t now = 0, odd = 0;
    ULONGLONG odd_since = 0;
    const auto strange = [&](ULONGLONG limit) {   // true once `now`, which is neither's, has stayed for `limit` ms
        if (now != odd) odd = now, odd_since = GetTickCount64();
        return now && GetTickCount64() - odd_since >= limit;
    };
    for (;;) {
        now = read_slot(slot);
        if (now == original || (in_eo(now) && settled(slot) == now)) break;
        if (strange(30000) || GetTickCount64() - mapped > 30000)
            return lost("the game's file lookup isn't the one expected (game+0x2E6EF28 holds " + hex(now) + ", in " + module_of(now) +
                            "); left alone",
                        later("Something other than Extra Options changed the game's file lookup."));
        Sleep(0);
    }

    // The game's code as it is in memory, against what was read from the exe. It's looked at once Extra Options'
    // pointer is in the slot, and not before: Extra Options writes it after Azimuth has finished mapping the game,
    // and until then a section may not be copied in yet. (The slot holding the game's resolver only says its own
    // section is. The first run in game compared the folder volume's vtable, 0x3C9E8 further on, the moment the slot
    // did, and found it different; without Extra Options the exe is read between the two, which takes long enough.)
    // Nothing here waits or reads a file, so the swap is a few microseconds later for it.
    const auto code_ok = [&]() -> bool {
        std::string differs;
        for (const Site& s : sites) {
            unsigned char mem[sizeof s.disk];
            if (!safe_copy(mem, base + s.rva, s.n) || std::memcmp(mem, s.disk, s.n) != 0 || (s.must && std::memcmp(s.disk, s.must, s.n) != 0)) {
                differs = s.shown;
                break;
            }
        }
        // (Absolute pointers: where the image isn't at its preferred base, each differs from the file's by how far it moved.)
        uint64_t vtable_mem[2] = {};
        if (differs.empty() && (!safe_copy(vtable_mem, base + RVA_FOLDER_VT + 8, sizeof vtable_mem) ||
                                !same_pointer(vtable_disk[0], vtable_mem[0], kPreferredBase, reinterpret_cast<uint64_t>(base), kGameSizeOfImage) ||
                                !same_pointer(vtable_disk[1], vtable_mem[1], kPreferredBase, reinterpret_cast<uint64_t>(base), kGameSizeOfImage)))
            differs = "game+0x2EAB918 (the folder volume's vtable: " + hex(vtable_mem[0]) + " and " + hex(vtable_mem[1]) + " in memory, " +
                      hex(vtable_disk[0]) + " and " + hex(vtable_disk[1]) + " in the exe)";
        if (!differs.empty()) {
            lost("the game's code isn't what this build expects: " + differs + " isn't the same as in the exe on disk, " + since() +
                     " ms after the game was mapped; the game's file lookup stays Extra Options'",
                 later("The game's code isn't what this build expects."));
            return false;
        }
        // The folder volume's methods, copied before the swap: the mount can run on a game thread right after it.
        if (!safe_copy(g_folder_vtable, base + RVA_FOLDER_VT, sizeof g_folder_vtable)) {
            lost("the folder volume's vtable (game+0x2EAB910) couldn't be read; the game's file lookup stays Extra Options'",
                 later("The game's code isn't what this build expects."));
            return false;
        }
        g_exists                      = reinterpret_cast<Exists>(g_folder_vtable[1 + 0xE8 / 8]);
        g_folder_vtable[1 + 0xE8 / 8] = reinterpret_cast<void*>(&folder_exists);
        std::memcpy(g_overlay_vtable, g_folder_vtable, sizeof g_overlay_vtable);
        g_overlay_vtable[1 + 0xE8 / 8] = reinterpret_cast<void*>(&overlay_exists);
        return true;
    };
    bool code_checked = false;

    // Extra Options writes its lookup into the slot while Azimuth maps the game, before any game code runs. Until
    // then: its status every 50 ms.
    ULONGLONG asked = 0, eo_took = 0;
    for (int tries = 0;;) {
        now = read_slot(slot);
        if (in_eo(now) && settled(slot) == now) {
            if (!eo_took) eo_took = GetTickCount64() - mapped;
            if (!code_checked) {
                if (!code_ok()) return;
                code_checked = true;
            }
            if (!writable(slot))
                return lost("game+0x2E6EF28 can't be written as it is, and its protection is left alone; the game's file lookup stays Extra Options'",
                            later("The game's file lookup couldn't be changed."));
            g_eo_resolve.store(reinterpret_cast<Resolve>(now), std::memory_order_release);
            g_chained_at.store(GetTickCount64());
            const int swapped = swap_guarded(slot, reinterpret_cast<void*>(ours), reinterpret_cast<void*>(now));
            if (swapped == 1) break;
            if (swapped < 0 || ++tries >= 8)
                return lost(swapped < 0 ? "writing game+0x2E6EF28 failed; the game's file lookup stays Extra Options'"
                                        : "game+0x2E6EF28 kept changing; the game's file lookup is left alone",
                            later("The game's file lookup couldn't be changed."));
            continue;
        }
        if (now != original && strange(250))
            return lost("the game's file lookup was changed by " + module_of(now) + ", not Extra Options (game+0x2E6EF28 holds " + hex(now) +
                            "); left alone",
                        later("Something other than Extra Options changed the game's file lookup."));
        if (const ULONGLONG t = GetTickCount64(); t - asked >= 50) {
            asked = t;
            const std::string status = extra_options_status();
            const bool late          = t - mapped > 20000;
            if ((final_status(status) || late) && read_slot(slot) == original)   // looked at again: its write comes before its status
                return lost(std::string("Extra Options didn't take over the game's file lookup this time (") +
                                (late && !final_status(status) ? "it hadn't 20 seconds after the game was mapped; its status: \"" : "its status: \"") +
                                status + "\"); game+0x2E6EF28 is left as the game's",
                            later("Extra Options didn't take over the game's file lookup: {0}",
                                  {final_status(status) ? status : std::string("it hadn't after 20 seconds.")}));
        }
        Sleep(0);
    }
    const std::string chained = since();
    plog("car packs with Extra Options: chain: Extra Options took the game's file lookup (game+0x2E6EF28), seen " + std::to_string(eo_took) +
         " ms after the game was mapped; the platform chained behind it " + chained + " ms after the game was mapped");

    // Its lookup of a file's path on disk goes in just after (the same pass over its patches); builds that don't take
    // it leave the game's there, and then no lookup can reach Extra Options but through the one above.
    for (int tries = 0;;) {
        now = read_slot(path_slot);
        if (in_eo(now) && settled(path_slot) == now) {
            g_eo_disk_path.store(reinterpret_cast<DiskPath>(now), std::memory_order_release);
            const int swapped = writable(path_slot) ? swap_guarded(path_slot, reinterpret_cast<void*>(path_ours), reinterpret_cast<void*>(now)) : -1;
            if (swapped == 1) {
                plog("car packs with Extra Options: chain: Extra Options took the lookup of a file's path on disk too (game+0x2E6EEC8, "
                     "for sound banks); the platform chained behind it " + since() + " ms after the game was mapped");
                break;
            }
            if (swapped < 0 || ++tries >= 8) {
                plog("car packs with Extra Options: chain: game+0x2E6EEC8 (a file's path on disk) couldn't be changed, so it stays Extra "
                     "Options'; if a sound bank is the game's first lookup, Extra Options sets up its folders first and the packs are lost");
                break;
            }
            continue;
        }
        const ULONGLONG t = GetTickCount64();
        if (t - asked < 20) {   // its status every 20 ms, the slot all the time
            Sleep(0);
            continue;
        }
        asked           = t;
        const bool done = final_status(extra_options_status());
        if (now == path_original && (done || GetTickCount64() - mapped > 20000) && read_slot(path_slot) == path_original) {
            plog("car packs with Extra Options: chain: Extra Options didn't take the lookup of a file's path on disk (game+0x2E6EEC8), so "
                 "there's nothing to chain there");
            break;
        }
        if (now != path_original && !in_eo(now) && (done || GetTickCount64() - mapped > 20000)) {
            plog("car packs with Extra Options: chain: game+0x2E6EEC8 (a file's path on disk) holds " + hex(now) + ", in " + module_of(now) +
                 ", not Extra Options; left alone");
            break;
        }
        Sleep(0);
    }

    // From here on: what the game threads found, Extra Options' own verdict, and the slot staying the platform's.
    bool finished = false, reported_in = false, gone = false;
    for (unsigned i = 0;; ++i) {
        if (std::string* why = g_lost_why.exchange(nullptr)) {
            car_packs_lost(*why);
            delete why;
        }
        if (g_in_front.load(std::memory_order_acquire) && !reported_in && !g_eo_lost.load()) {
            reported_in = true;
            note_eo_packs_outcome("in", "their folders are ahead of Extra Options' (the precedence check passed)");
        }
        if (!finished) {
            if (const std::string status = extra_options_status(); final_status(status)) {
                finished = true;
                plog("car packs with Extra Options: chain: Extra Options finished " + since() + " ms after the game was mapped (\"" + status +
                     "\"); the game's file lookup is " + (settled(slot) == ours ? "still the platform's, with Extra Options' behind it" : "NOT the platform's any more"));
            } else if (GetTickCount64() - mapped > 20000) {
                finished = true;
                plog("car packs with Extra Options: chain: Extra Options still hasn't said it has finished 20 seconds after the game was "
                     "mapped (its status: \"" + status + "\")");
            }
        }
        if (now = read_slot(slot); !gone && now != ours && settled(slot) != ours && read_slot(slot) != ours) {
            // Extra Options undoing its changes (a failed flush), or the game mapped a second time: either way the
            // platform is no longer in the game's lookup, and what's mounted belongs to a game that's gone or going.
            plog("car packs with Extra Options: the game's file lookup was changed back " + since() + " ms after the game was mapped "
                 "(game+0x2E6EF28 holds " + hex(now) + ", in " + (now == original ? std::string("the game") : module_of(now)) +
                 "): Extra Options undid its changes, or the game was mapped again. It's left alone");
            gone = true;   // a reason a game thread gave first is still passed on, above
            lose_now(later("The game's file lookup was changed again after the platform had chained behind Extra Options."));
        }
        Sleep(i < 2400 ? 50 : 500);   // every 50 ms for two minutes, then twice a second
    }
}

} // namespace

void eo_overlay_ready() {
    if (!eo_packs()) return;
    std::error_code ec;
    const bool db = fs::is_regular_file(eo_media() / L"Stripped" / L"gamedbRC.slt", ec);
    g_overlay_has_db.store(db, std::memory_order_release);
    g_overlay_ready.store(true, std::memory_order_release);
    plog(std::string("car packs with Extra Options: the platform's first write is done; eo-media has ") +
         (db ? "this session's database, with the packs' cars" : "no database (no pack or mod changes it, or it couldn't be built)") +
         (g_eo_lost.load() ? "; the packs were already lost this session" : ""));
}

namespace {

// When whether Extra Options is loaded couldn't be told (extra_options_expected): says whether it ran,
// once it's found or writes its log this launch. It patches the game right after it's mapped, so
// kExtraOptionsTimeoutMs after that it hasn't.
void wait_for_extra_options() {
    ULONGLONG mapped = 0;
    for (unsigned i = 0;; ++i) {
        if (!extra_options_dir().empty()) {
            plog("media overrides: Extra Options is loaded after all, so mediaoverride is left to it");
            return;
        }
        if (extra_options_log_is_new()) {
            plog("media overrides: Extra Options wrote FH2_ExtraOptions.log this launch, so it's running and "
                 "mediaoverride is left to it");
            return;
        }
        if (launched_game() == Launched::other) return;
        if (!mapped && i % 10 == 0 && find_game()) mapped = GetTickCount64();
        if (mapped && GetTickCount64() - mapped > kExtraOptionsTimeoutMs) {
            plog("media overrides: Extra Options still hasn't written FH2_ExtraOptions.log 20 seconds after the "
                 "game was mapped, so it seems not to be running, and nothing mounts mediaoverride this time");
            return;
        }
        Sleep(50);
    }
}

} // namespace

void start_media_overrides() {
    // Where the platform's files go is decided here, once for the whole session (instance_layer), before anything
    // is written.
    const bool layer = instance_layer();
    if (!extra_options_dir().empty()) {
        plog("media overrides: Extra Options is here, so mediaoverride is left to it");
        plog("media overrides: the platform's files go to mediaoverride for this whole session");
        move_old_overrides();
        if (!eo_packs()) {
            note_eo_packs_outcome({}, {});
            car_packs_unavailable(eo_packs_off_reason());
            return;
        }
        // The packs are read as in this folder's own layer, but for where their zip list goes (eo-media) and which of
        // them are left out; then the platform chains behind Extra Options' file lookup once the game is mapped.
        prepare_car_packs();
        const auto read = std::count_if(car_packs().begin(), car_packs().end(), [](const CarPack& p) { return !p.left_out; });
        if (!read) {
            plog("car packs: every pack in fh2-cars is left out this session, so nothing is added behind Extra Options' file lookup");
            note_eo_packs_outcome("lost", "every pack was left out");
            return;
        }
        const std::string version = extra_options_facts().version;
        // (This line's wording is matched by other tools: keep it as it is.)
        plog("car packs: " + std::to_string(read) + " pack(s) in fh2-cars are read with Extra Options loaded (version " +
             (version.empty() ? std::string("unknown") : version) + ")");
        plog("car packs with Extra Options: their folders and " + temp_drive_spelling(eo_media()) + " (their zipmanifest.xml, and the "
             "database and car names with their cars) are added to the game's folders, ahead of Extra Options', when the game first "
             "looks a file up");
        note_eo_packs_outcome("on", "read; the game hasn't looked a file up yet");
        std::thread(watch_eo).detach();
        return;
    }
    note_eo_packs_outcome({}, {});   // last session's, with Extra Options
    if (extra_options_expected()) {
        // Under Wine, set to load, but the list of loaded DLLs couldn't be read. Swapping the game's
        // file lookup under Extra Options would make it refuse every patch ("unexpected loaded bytes
        // at media_override_native_resolver"), so mediaoverride is left to it as when it's loaded.
        plog("media overrides: Extra Options' winmm.dll is next to azimuth.exe and Wine is set to load it, but the "
             "list of loaded DLLs can't be read to see whether it has; mediaoverride is left to it");
        plog("media overrides: the platform's files go to mediaoverride for this whole session");
        move_old_overrides();
        car_packs_unavailable(later("Extra Options is loaded."));   // counted as loaded; in each language on the title screen
        std::thread(wait_for_extra_options).detach();
        return;
    }
    plog("media overrides: Extra Options isn't loaded, so the platform mounts mediaoverride and mediapc itself");
    if (layer) {
        plog("media overrides: this Azimuth folder has data\\titles\\1A96545B (where Azimuth keeps the game's T:\\ drive), "
             "so for this whole session its own files go to " + narrow(instance_media().wstring()) + " (the game's " +
             temp_drive_spelling(instance_media()) + "), read ahead of mediaoverride");
        move_old_overrides();   // first: the packs' merged zipmanifest.xml is built on what the game reads without them
        prepare_car_packs();
    } else if (instance_layer_missing() == NoLayer::no_game) {
        // Nothing the platform writes for the game can be used, and there's no MODS tab: nothing for the player here.
        plog("media overrides: the game's Mount folder wasn't found, so the platform writes nothing for the game this session "
             "(to this Azimuth folder's own layer or anywhere else), and car packs aren't read");
        write_engine_synths();   // so an engine sound mod isn't told about cars from a session that read them
    } else if (instance_layer_missing() == NoLayer::no_scratch) {
        plog("media overrides: this Azimuth folder has data\\titles\\1A96545B, but no tmp\\Turn10Temp.scratch in it (the game makes "
             "that folder on its T:\\ drive the first time it starts), so it can't be told that the game's T:\\ is there; for this "
             "whole session the platform's files go to mediaoverride, mounted as up to 2.8.0");
        move_old_overrides();
        car_packs_unavailable(later("This Azimuth folder has no data\\titles\\1A96545B\\tmp\\Turn10Temp.scratch folder yet: the game "
                                    "makes it the first time it starts."));
    } else {
        plog("media overrides: this Azimuth folder has no data\\titles\\1A96545B yet (Azimuth makes it the first time the "
             "game starts), so for this whole session the platform's files go to mediaoverride, mounted as up to 2.8.0");
        move_old_overrides();
        car_packs_unavailable(later("This Azimuth folder has no data\\titles\\1A96545B folder yet: Azimuth makes it the first "
                                    "time the game starts."));
    }
    std::thread(watch).detach();
}

} // namespace fh2platform

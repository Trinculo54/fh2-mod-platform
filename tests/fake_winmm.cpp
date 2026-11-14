// Stands in for Extra Options' winmm.dll in the hook test: the platform
// recognises it by its Fh2TweaksStatus export, and reads FH2_ExtraOptions.log
// next to it, and its version from the text every Extra Options release has in
// its image.
//
// In the hook test's "eo-packs" phases (azimuth.exe's first argument) it also
// does to the game's file lookup what Extra Options does, for car packs with
// Extra Options (src/media_overrides.cpp, watch_eo). Its status is "Not
// initialized" until, 300 ms after the game is mapped, it
//   eo-packs          (and eo-packs-changed) puts its own lookups in the game's two slots
//                     (game+0x2E6EF28, and game+0x2E6EEC8 for a file's path on
//                     disk), then says "OK: ...";
//   eo-packs-refused  says "REFUSED: ..." and leaves the slots alone;
//   eo-packs-late     as eo-packs, but it has set up its folder first, as if a
//                     lookup had reached it before the platform chained.
// Like Extra Options, on its first lookup (once the file system has its root
// volume) it mounts a folder volume of its own at 0x7FFFFFFE, whose vtable is
// in this DLL's image, has the file system sort, and asks the game's own
// resolver (game+0x11F49B0) directly; for a .bank's path on disk it does that
// without going through the slot. The file system and the game functions are
// the hook test's stand-ins.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

[[gnu::used]] static const char kVersion[] = "Forza Horizon 2 Extra Options 0.6.0";

static char* const kGame = reinterpret_cast<char*>(0x140000000ull);

struct GameString {   // an MSVC std::string
    union {
        const char* data;
        char in_place[16];
    };
    std::size_t length, capacity;
};
struct Volume {   // as the game's folder volume: references at +0x20, its folder at +0x28
    void** vtable;
    char pad[0x18];
    volatile LONG refs;
    LONG pad2;
    GameString folder;
};
using Resolve  = unsigned char (*)(void*, void**, void*, const void*, unsigned char, unsigned char, unsigned char, void**, unsigned char);
using DiskPath = void* (*)(void*, const GameString*, GameString*);
using Mount    = unsigned char (*)(void*, const GameString*, void**, unsigned, unsigned char, unsigned char, const void*);
using Method   = void (*)(void*);

static char g_status[96]          = "OK: hook test stand-in";
static void* g_vtable[1 + 0x1F0 / sizeof(void*)];   // in this image: how the platform tells Extra Options' folders
static Volume g_volume;
static volatile LONG g_mounted = 0;
static void* g_filesystem      = nullptr;

static const char* text_of(const GameString* s) { return s->capacity < 16 ? s->in_place : s->data; }

static LONG add_reference(Volume* v) { return InterlockedIncrement(&v->refs); }
static LONG release(Volume* v) { return InterlockedDecrement(&v->refs); }
// mediaoverride, in the test's folder.
static unsigned char exists(Volume*, const GameString* path) {
    char file[512] = "FH2\\Mount\\mediaoverride\\";
    if (path->length >= sizeof file - 32) return 0;
    std::memcpy(file + std::strlen(file), text_of(path), path->length);
    const DWORD a = GetFileAttributesA(file);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void mount(void* filesystem) {
    void* root = nullptr;
    std::memcpy(&root, static_cast<char*>(filesystem) + 0x68, sizeof root);
    if (!root || InterlockedExchange(&g_mounted, 1)) return;
    static const char folder[] = "game:\\mediaoverride\\", media[] = "game:\\media\\";
    g_vtable[1 + 0x10 / 8] = reinterpret_cast<void*>(&add_reference);
    g_vtable[1 + 0x18 / 8] = reinterpret_cast<void*>(&release);
    g_vtable[1 + 0xE8 / 8] = reinterpret_cast<void*>(&exists);
    g_volume.vtable        = g_vtable + 1;
    g_volume.folder.data   = folder;
    g_volume.folder.length = sizeof folder - 1, g_volume.folder.capacity = 31;
    GameString target{};
    target.data   = media;
    target.length = sizeof media - 1, target.capacity = 31;
    void* volume   = &g_volume;
    void** methods = *static_cast<void***>(filesystem);
    reinterpret_cast<Mount>(methods[0x1E8 / 8])(filesystem, &target, &volume, 0x7FFFFFFEu, 0, 0, nullptr);
    reinterpret_cast<Method>(methods[0x1F8 / 8])(filesystem);
}

static unsigned char resolve(void* filesystem, void** volume, void* relative, const void* path, unsigned char a, unsigned char b,
                             unsigned char c, void** mounted, unsigned char d) {
    mount(filesystem);
    return reinterpret_cast<Resolve>(kGame + 0x11F49B0)(filesystem, volume, relative, path, a, b, c, mounted, d);
}

static void* disk_path(void* filesystem, const GameString* path, GameString* output) {
    if (path->length >= 5 && !_strnicmp(text_of(path) + path->length - 5, ".bank", 5)) {
        mount(filesystem);
        void* volume = nullptr;
        GameString relative{};
        relative.capacity = 15;
        reinterpret_cast<Resolve>(kGame + 0x11F49B0)(filesystem, &volume, &relative, path, 0, 0, 0, nullptr, 0);
    }
    return reinterpret_cast<DiskPath>(kGame + 0x11F1460)(filesystem, path, output);
}

static DWORD WINAPI as_extra_options(LPVOID which) {
    const int phase  = static_cast<int>(reinterpret_cast<intptr_t>(which));   // 1 eo-packs, 2 refused, 3 late
    void** slot      = reinterpret_cast<void**>(kGame + 0x2E6EF28);
    void** path_slot = reinterpret_cast<void**>(kGame + 0x2E6EEC8);
    // The game mapped: its headers are copied in last.
    for (;;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(kGame, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT && kGame[0] == 'M' && kGame[1] == 'Z' &&
            *slot == kGame + 0x11F49B0)
            break;
        Sleep(1);
    }
    Sleep(300);
    if (phase == 2) {
        std::strcpy(g_status, "REFUSED: hook test stand-in. No patches applied.");
        return 0;
    }
    if (phase == 3 && g_filesystem) mount(g_filesystem);
    *slot      = reinterpret_cast<void*>(&resolve);
    *path_slot = reinterpret_cast<void*>(&disk_path);
    Sleep(20);   // the rest of its patches
    std::strcpy(g_status, "OK: hook test stand-in, lookups taken.");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    // The phase is azimuth.exe's first argument.
    const wchar_t* line = GetCommandLineW();
    const intptr_t phase = std::wcsstr(line, L" eo-packs-refused") ? 2 : std::wcsstr(line, L" eo-packs-late") ? 3 : std::wcsstr(line, L" eo-packs") ? 1 : 0;
    if (phase) {
        std::strcpy(g_status, "Not initialized");
        CloseHandle(CreateThread(nullptr, 0, as_extra_options, reinterpret_cast<LPVOID>(phase), 0, nullptr));
    }
    return TRUE;
}

extern "C" __declspec(dllexport) const char* Fh2TweaksStatus() { return g_status; }

// The hook test's file system, for the phase in which a lookup has reached Extra Options before the platform chained.
extern "C" __declspec(dllexport) void FakeEoFilesystem(void* filesystem) { g_filesystem = filesystem; }

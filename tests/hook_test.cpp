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
// Options, and the log says how to fix it. "changed-code": as "plain", but
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
#include <windows.h>

#include <cstdio>
#include <cstring>
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

// Maps the exe the way Azimuth does, enough for its bytes and protections:
// sections at their addresses, code read/execute, read-only data read-only.
static char* map_game(const wchar_t* path, bool change_volume_code, bool other_game = false) {
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
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    return base;
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
        std::printf("usage: azimuth.exe plain|eo|wine-builtin|changed-code|crash|other-game|skip-launcher "
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
    if (phase == L"eo") CHECK(GetProcAddress(LoadLibraryW(L".\\winmm.dll"), "Fh2TweaksStatus")); // Extra Options' proxy
    if (phase == L"wine-builtin") {
        HMODULE w = LoadLibraryW(L".\\winmm.dll"); // Wine's own, though it says it's this one
        CHECK(w && !GetProcAddress(w, "Fh2TweaksStatus"));
    }
    CHECK(LoadLibraryW(L"d3dcompiler_47.dll") != nullptr);
    Sleep(500);
    char* game = map_game(argv[2], phase == L"changed-code", other_game);
    CHECK(game != nullptr); // "press Play"
    if (phase == L"eo") {
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
    CHECK(has(log, "FH2 Mod Platform 2.8.0 (API 9)"));
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
    if (phase == L"eo") {
        CHECK(has(log, "hooks: game code is ready (Extra Options has applied its patches)"));
        CHECK(waited >= 2000);
    } else {
        CHECK(has(log, "hooks: game code is ready (no Extra Options)"));
        CHECK(waited < 2000);
    }
    CHECK(has(log, "WINEDLLOVERRIDES=\"winmm=n,b\"") == (phase == L"wine-builtin"));
    // The game's file lookup: the platform's own function only without Extra Options.
    void* lookup = game ? *reinterpret_cast<void**>(game + 0x2E6EF28) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    const bool platforms = lookup && VirtualQuery(lookup, &mbi, sizeof mbi) == sizeof mbi &&
                           mbi.AllocationBase == GetModuleHandleW(L"d3dcompiler_47.dll");
    if (phase == L"eo") {
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

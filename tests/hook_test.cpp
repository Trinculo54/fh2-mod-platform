// Offline test of the hook API, run as "azimuth.exe" under Wine by
// tests/run_hook_test.sh with the real game exe:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\HookMod.dll
//   FH2\Mount\forza_durango_release_final.exe, FH2\Mount\media\Stripped\StringTables\
//   azimuth.exe <phase> <path to forza_durango_release_final.exe>
// phase "plain": no Extra Options. "eo": a stand-in winmm.dll is loaded and
// FH2_ExtraOptions.log only says it's done 3 seconds after the game is
// mapped (an old log from before this launch must not count). "crash": a
// hooked function crashes, and the report has to list the hooks.
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
static char* map_game(const wchar_t* path) {
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
    VirtualProtect(base + 0x199FBE0, 1, PAGE_EXECUTE_READWRITE, &old);
    base[0x199FBE0] = static_cast<char>(0xCC);
    VirtualProtect(base + 0x199FBE0, 1, old, &old);
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
        std::printf("usage: azimuth.exe plain|eo|crash <forza_durango_release_final.exe>\n");
        return 2;
    }
    const std::wstring phase = argv[1];
    if (phase == L"crash") {
        std::ofstream{"crash.txt"} << "1";
        SetUnhandledExceptionFilter(after_crash);
    }
    if (phase == L"eo") CHECK(LoadLibraryW(L".\\winmm.dll") != nullptr); // Extra Options' proxy
    CHECK(LoadLibraryW(L"d3dcompiler_47.dll") != nullptr);
    Sleep(500);
    CHECK(map_game(argv[2]) != nullptr); // "press Play"
    if (phase == L"eo") {
        Sleep(3000);
        std::ofstream{"FH2_ExtraOptions.log", std::ios::app} << "AZIMUTH: winmm proxy initialized; original TLS helper retained.\r\n";
    }
    std::string log;
    for (int i = 0; i < 300; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if (has(log, "hook tests done")) break;
    }
    if (phase == L"crash") {
        Sleep(10000);
        std::printf("FAIL no crash\n");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    Sleep(200);
    log = slurp(L"fh2-mods\\platform.log");
    std::printf("---- platform.log\n%s----\n", log.c_str());
    CHECK(has(log, "FH2 Mod Platform 2.2.1 (API 3)"));
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
    if (phase == L"eo") {
        CHECK(has(log, "hooks: game code is ready (Extra Options has applied its patches)"));
        const auto at = log.find("wait_for_code took ");
        CHECK(at != std::string::npos && std::stoul(log.substr(at + 19)) >= 2000);
    } else {
        CHECK(has(log, "hooks: game code is ready (no Extra Options)"));
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
    return fails;
}

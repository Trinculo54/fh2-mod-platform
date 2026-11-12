// The title gate (src/titles.cpp), run by tests/run_title_gate_test.sh as azimuth.exe in a folder with
// d3dcompiler_47.dll and fh2-mods\GateMod.dll. It plays Azimuth: sets up data\azimuth.ini, loads the platform,
// then maps a game's header at 0x140000000 and makes it executable through its own VirtualProtect import, as
// Azimuth does with a game's code.
//   title_gate_test other   FH2 and FH3 set up, FH3 picked: the mod's DLL is never loaded
//   title_gate_test fh2     FH2 and FH3 set up, FH2 picked: the mod loads then, and VirtualProtect returns only once
//                           its fh2mod_init has
//   title_gate_test alone   only FH2 set up: the mod loads as Azimuth starts, with no game mapped
//   title_gate_test told --skip-launcher   FH2 and FH3 set up, Azimuth told to start FH2: as alone
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

constexpr DWORD kFh2Stamp = 0x565501ED, kFh2Size = 0x3A18000;
constexpr DWORD kFh3Stamp = 0x5B16ECCA, kFh3Size = 0x5495000;

bool exists(const char* name) { return GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES; }

std::string platform_log() {
    std::ifstream f("fh2-mods\\platform.log", std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

bool wait_for(bool (*done)(), unsigned ms = 5000) {
    for (unsigned i = 0; i < ms / 10; ++i) {
        if (done()) return true;
        Sleep(10);
    }
    return done();
}

void game_header(unsigned char* page, DWORD stamp, DWORD size) {
    auto* dos     = reinterpret_cast<IMAGE_DOS_HEADER*>(page);
    dos->e_magic  = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt      = reinterpret_cast<IMAGE_NT_HEADERS64*>(page + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine         = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp   = stamp;
    nt->OptionalHeader.Magic       = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = size;
}

void write_exe(const char* folder, DWORD stamp, DWORD size) {
    CreateDirectoryA(folder, nullptr);
    const std::string mount = std::string(folder) + "\\Mount";
    CreateDirectoryA(mount.c_str(), nullptr);
    unsigned char page[0x1000]{};
    game_header(page, stamp, size);
    std::ofstream(mount + "\\forza_durango_release_final.exe", std::ios::binary)
        .write(reinterpret_cast<const char*>(page), sizeof page);
}

[[noreturn]] void finish(const char* which, int failed_at, const char* what) {
    if (failed_at) std::printf("FAIL %s: step %d\n---- platform.log\n%s----\n", which, failed_at, platform_log().c_str());
    else std::printf("ok %s: %s\n", which, what);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), failed_at);
    for (;;) Sleep(1000);
}

} // namespace

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const std::string which = argc > 1 ? argv[1] : "";
    const bool shared = which != "alone";
    CreateDirectoryA("data", nullptr);
    write_exe("FH2", kFh2Stamp, kFh2Size);
    write_exe("FH3", kFh3Stamp, kFh3Size);
    std::ofstream("data\\azimuth.ini")
        << "[launcher]\ngame=1a96545b\n[title-1a96545b]\nfolder=FH2\nname=Forza Horizon 2\n"
        << (shared ? "[title-4ce1dfab]\nfolder=FH3\nname=Forza Horizon 3\n" : "");
    if (!LoadLibraryA(".\\d3dcompiler_47.dll")) finish(which.c_str(), 1, "");

    if (which == "alone" || which == "told") {
        // no game is ever mapped: the mod has to start all the same
        if (!wait_for([] { return exists("gate-init.txt"); })) finish(which.c_str(), 2, "");
        if (platform_log().find("title gate:") != std::string::npos) finish(which.c_str(), 3, "");
        finish(which.c_str(), 0, "the mod started as Azimuth did, and the gate stayed out of it");
    }

    const bool fh2 = which == "fh2";
    if (!wait_for([] { return platform_log().find("so no mod is loaded until a game is picked") != std::string::npos; }))
        finish(which.c_str(), 4, "");
    Sleep(300); // long enough for a mod to have loaded, if the gate let one
    if (exists("gate-dllmain.txt")) finish(which.c_str(), 5, "");

    const DWORD size = fh2 ? kFh2Size : kFh3Size;
    auto* image      = static_cast<unsigned char*>(
        VirtualAlloc(reinterpret_cast<void*>(0x140000000ull), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!image) finish(which.c_str(), 6, "");
    game_header(image, fh2 ? kFh2Stamp : kFh3Stamp, size);
    DWORD old = 0;
    if (!VirtualProtect(image + 0x1000, 0x1000, PAGE_EXECUTE_READ, &old)) finish(which.c_str(), 7, "");

    if (fh2) {
        // the mod takes 300 ms to start, so it can only be done already if the call above waited for it
        if (!exists("gate-init.txt")) finish(which.c_str(), 8, "");
        const std::string log = platform_log();
        if (log.find("title gate: Azimuth is held at the game's start") == std::string::npos ||
            log.find("ms later; the game starts") == std::string::npos ||
            log.find("title gate: Forza Horizon 2 was picked; loading the mods") == std::string::npos)
            finish(which.c_str(), 9, "");
        finish(which.c_str(), 0, "the mod loaded once FH2 was picked, and Azimuth was held until it had started");
    }
    if (!wait_for([] { return platform_log().find("not Forza Horizon 2: no mods are loaded") != std::string::npos; }))
        finish(which.c_str(), 10, "");
    Sleep(300);
    if (exists("gate-dllmain.txt") || exists("gate-init.txt") || GetModuleHandleA("GateMod.dll"))
        finish(which.c_str(), 11, "");
    if (platform_log().find("Forza Horizon 3 (title 4ce1dfab)") == std::string::npos) finish(which.c_str(), 12, "");
    finish(which.c_str(), 0, "another game was picked, and the mod's DLL was never loaded");
}

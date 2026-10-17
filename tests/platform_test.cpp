// Offline test of the platform, run as "azimuth.exe" under Wine by
// tests/run_platform_test.sh, which sets up the folder:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\{AaaMod,HelloMod,TestMod,Legacy}.dll
//   FH2\Mount\media\Stripped\StringTables\EN.zip, FH2\Mount\media\Stripped\gamedbRC.slt
//   FH2\Mount\mediaoverride\UI\handmade.xml (an override placed by hand)
//   azimuth.exe <phase> [fh2.exe]
// phase "full": everything installed; "removed": TestMod.dll deleted;
// "crash": AaaMod crashes on purpose.
#include "strings.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace fh2platform;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

static std::string slurp(const fs::path& f) {
    std::ifstream in{f, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}

static std::wstring str(const char* table, uint16_t key) {
    auto d = slurp(fs::path{L"FH2\\Mount\\mediaoverride\\Stripped\\StringTables\\EN"} / table);
    if (d.empty()) return L"<none>";
    std::wstring found = L"<missing>";
    edit_lsb2(std::vector<char>(d.begin(), d.end()), [&](uint16_t k, const std::wstring& t) {
        if (k == key) found = t;
        return t;
    });
    return found;
}

static bool ai_has(const wchar_t* name) {
    auto d = slurp(L"FH2\\Mount\\mediaoverride\\Stripped\\StringTables\\EN\\AIPlayers.str");
    bool hit = false;
    edit_lsb2(std::vector<char>(d.begin(), d.end()), [&](uint16_t, const std::wstring& t) {
        if (t == name) hit = true;
        return t;
    });
    return hit;
}

// AIPlayers.XP of `id` in the database override, or -1.
static int db_xp(int id) {
    HMODULE h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) return -2;
    auto open  = reinterpret_cast<int (*)(const char*, void**, int, const char*)>(GetProcAddress(h, "sqlite3_open_v2"));
    auto prep  = reinterpret_cast<int (*)(void*, const char*, int, void**, const char**)>(GetProcAddress(h, "sqlite3_prepare_v2"));
    auto step  = reinterpret_cast<int (*)(void*)>(GetProcAddress(h, "sqlite3_step"));
    auto col   = reinterpret_cast<int (*)(void*, int)>(GetProcAddress(h, "sqlite3_column_int"));
    auto fin   = reinterpret_cast<int (*)(void*)>(GetProcAddress(h, "sqlite3_finalize"));
    auto close = reinterpret_cast<int (*)(void*)>(GetProcAddress(h, "sqlite3_close"));
    if (!fs::exists("FH2\\Mount\\mediaoverride\\Stripped\\gamedbRC.slt")) return -1;
    void* db = nullptr;
    void* st = nullptr;
    int xp   = -1;
    if (open("FH2\\Mount\\mediaoverride\\Stripped\\gamedbRC.slt", &db, 1, nullptr) == 0 &&
        prep(db, ("SELECT XP FROM AIPlayers WHERE Id = " + std::to_string(id)).c_str(), -1, &st, nullptr) == 0 &&
        step(st) == 100)
        xp = col(st, 0);
    if (st) fin(st);
    close(db);
    return xp;
}

static bool has(const std::string& log, const char* s) { return log.find(s) != std::string::npos; }

static LONG WINAPI after_crash(EXCEPTION_POINTERS*) {
    // Runs after the platform's crash handler, which passes crashes on.
    std::string report;
    bool dump = false;
    for (const auto& e : fs::directory_iterator("fh2-mods\\crashes")) {
        if (e.path().extension() == ".txt") report = slurp(e.path());
        if (e.path().extension() == ".dmp") dump = fs::file_size(e.path()) > 0;
    }
    std::printf("---- crash report\n%s----\n", report.c_str());
    CHECK(has(report, "CRASH: access violation (0xC0000005) at AaaMod.dll+0x"));
    CHECK(has(report, "writing address 0x0"));
    CHECK(has(report, "the crash came from AaaMod.dll"));
    CHECK(dump);
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
    return EXCEPTION_EXECUTE_HANDLER;
}

static char* map_image(const wchar_t* path) {
    std::ifstream in{path, std::ios::binary};
    std::vector<char> file((std::istreambuf_iterator<char>(in)), {});
    if (file.size() < 4096) return nullptr;
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    auto* base = static_cast<char*>(VirtualAlloc(reinterpret_cast<void*>(nt->OptionalHeader.ImageBase),
                                                 nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_READWRITE));
    if (base) std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    return base;
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring phase = argc > 1 ? argv[1] : L"full";
    if (phase == L"crash") {
        std::ofstream{"crash.txt"} << "1";
        SetUnhandledExceptionFilter(after_crash); // Windows' (or Azimuth's) handler, to the platform
    }
    HMODULE p = LoadLibraryW(L"d3dcompiler_47.dll"); // what azimuth.exe does at startup
    CHECK(p != nullptr);
    if (argc > 2) map_image(argv[2]); // "press Play"
    std::string log;
    for (int i = 0; i < 250; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if (has(log, "mod(s) loaded") && has(log, "TestMod version") &&
            (phase == L"full" ? has(log, "late edit applied")
                              : has(log, "removed AIPlayers.str") && has(log, "keep.xml override")))
            break;
    }
    if (phase == L"crash") {
        Sleep(10000);
        std::printf("FAIL no crash\n");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    Sleep(300);
    log = slurp(L"fh2-mods\\platform.log");
    std::printf("---- platform.log\n%s----\n", log.c_str());

    if (phase == L"full") {
        CHECK(has(log, "FH2 Mod Platform 2.2.0 (API 3)"));
        CHECK(log.find("loaded HelloMod.dll") != std::string::npos);
        CHECK(log.find("Legacy.dll has no fh2mod_init") != std::string::npos);
        CHECK(fs::exists("legacy_ran.txt"));
        CHECK(log.find("[TestMod] built for FH2 Mod Platform 9.9.9") != std::string::npos);
        CHECK(log.find("overrides another mod's string") != std::string::npos);
        // TestMod loads after HelloMod, so its edit of the same key wins.
        CHECK(str("LoadingMessages.str", 0x6875) == L"from TestMod");
        CHECK(ai_has(L"Buddy1") && ai_has(L"Buddy2") && !ai_has(L"Nocomply76") && ai_has(L"Womgoose"));
        const auto title = str("Main.str", 0x18F0);
        std::printf("title: [%ls]\n", title.c_str());
        CHECK(title == L"\n\n\n\nTO START\nTest notice line\nsecond line\nOld FH2 Mod Platform: reinstall TestMod");
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") != std::string::npos);
        CHECK(log.find("the game is mapped at") != std::string::npos);

        // Files
        const fs::path ov = L"FH2\\Mount\\mediaoverride";
        CHECK(slurp(ov / "drivatarglobalcache" / "DefaultCache.bin") == "from AaaMod");
        CHECK(has(log, "[TestMod] can't replace media\\drivatarglobalcache\\DefaultCache.bin: AaaMod already does"));
        CHECK(has(log, "media\\Stripped\\StringTables\\EN\\Main.str is managed by the platform"));
        CHECK(has(log, "\"UI/../../evil.txt\" isn't a path under media"));
        CHECK(slurp(ov / "UI" / "keep.xml") == "<xml/>");
        CHECK(slurp(ov / "UI" / "handmade.xml") == "<xml/>");
        CHECK(slurp(ov / "UI" / "handmade.xml.bak") == "made by hand");
        CHECK(!fs::exists(ov / "UI" / "temp.xml"));
        CHECK(has(log, "[TestMod] stopped replacing media\\UI\\temp.xml"));
        const auto files = slurp("fh2-mods\\platform-files.txt");
        CHECK(has(files, "UI\\keep.xml\tTestMod") && has(files, "drivatarglobalcache\\DefaultCache.bin\tAaaMod") &&
              !has(files, "temp.xml"));

        // Database
        CHECK(has(log, "[AaaMod] database: FAILED, so none of its changes were made: no such table: NoSuchTable"));
        CHECK(has(log, "[TestMod] database: 2 row(s) changed"));
        CHECK(db_xp(1) == 12345 && db_xp(2) == 777 && db_xp(3) == 120000);

        // Other mods
        CHECK(has(log, "[AaaMod] wait_for_mods from init returned 0") ||
              has(log, "a mod called wait_for_mods from fh2mod_init"));
        CHECK(has(log, "[TestMod] published TestMod.Math v1"));
        CHECK(has(log, "[AaaMod] TestMod.Math says 5"));
        CHECK(has(log, "[AaaMod] no TestMod.Math v2"));
        CHECK(has(log, "[AaaMod] TestMod version 1.0.0"));
    } else {
        // TestMod gone: its tables and notice are cleaned up, HelloMod's stays.
        CHECK(str("LoadingMessages.str", 0x6875) == L"Fast Traveling to Destination (hello from HelloMod!)");
        CHECK(str("AIPlayers.str", 0) == L"<none>");
        CHECK(str("Main.str", 0x18F0) == L"<none>");
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") == std::string::npos);
        const fs::path ov = L"FH2\\Mount\\mediaoverride";
        CHECK(!fs::exists(ov / "UI" / "keep.xml") && !fs::exists(ov / "UI" / "handmade.xml"));
        CHECK(slurp(ov / "drivatarglobalcache" / "DefaultCache.bin") == "from AaaMod");
        CHECK(has(log, "removed the media\\UI\\keep.xml override (TestMod no longer replaces it)"));
        CHECK(!fs::exists(ov / "Stripped" / "gamedbRC.slt"));
        CHECK(has(log, "[AaaMod] TestMod version (none)"));
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails); // don't wait on the mods' threads
    return fails;
}

// Offline test of the platform, run as "azimuth.exe" under Wine by
// tests/run_platform_test.sh, which sets up the folder:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\{AaaMod,HelloMod,TestMod,Legacy}.dll
//   FH2\Mount\media\Stripped\StringTables\EN.zip, FH2\Mount\media\Stripped\gamedbRC.slt
//   FH2\Mount\mediaoverride\UI\handmade.xml (an override placed by hand)
//   azimuth.exe <phase> [fh2.exe]
// phase "full": everything installed; "removed": TestMod.dll deleted;
// "crash": AaaMod crashes on purpose; "crash-chained": a crash handler installed after the
// platform's passes crashes on to it; then, with an FH2MP.dll, OfflineMod.dll and FH2MP's
// session file: "online-first", "online", "online-chosen" (the player's choices) and
// "fh2mp-off" (FH2MP turned off on the MODS tab); then (platform 2.9.0, issue #22) "online-root"
// (the session next to azimuth.exe, no data\bin), "online-late" (FakeFH2MP.dll as ZzFH2MP.dll
// writes the session while the mods start, so OfflineMod is stopped, its text, SQL and file taken
// out), "online-renamed" (the same DLL, known as FH2MP now), "online-register" (FakeFH2MP.dll as
// MpClient.dll, new, finds its session when it registers: the mods before it that shouldn't run
// online are stopped, the ones after it aren't started) and "online-ready" (no session; FH2MP's
// going_online, once every mod has started, stops OfflineMod and its text, SQL and file are taken
// out again); last, "hud": HudTestMod.dll adds a HUD layer (the HUD layer preview,
// include/fh2mod_hud.h).
// The UI templates' player controls (platform 2.9.0) are set up by this program before it loads the
// platform: in "full", AaaMod declares pop-ups (aaa_popups.txt) and fh2-mods\platform-popups.ini has
// the player's switch for them off; in "newmod", platform-popups.ini has Developer=1 and AaaMod
// declares pop-ups again; in "home", Developer=1 with mod pop-ups and tips off. Both go again
// ("removed", and the ends of "newmod" and "home").
// The runner goes through the phases twice, once per layout (platform 2.9.0): first with no
// data\titles folder (Azimuth before 1.0, or Extra Options loaded), where the platform's files go
// to mediaoverride; then with one (Azimuth 1.0 or later without Extra Options), where they go to
// the folder's own layer, data\titles\1A96545B\tmp\fh2-platform\media (car_packs.cpp).
#include "strings.hpp"
#include "updates.hpp"

#include <windows.h>

#include "sqlite3.h"

#include <algorithm>
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

// Where the platform's files go in this layout: the folder's own layer on Azimuth 1.0 or later
// (a data\titles folder) without Extra Options, else mediaoverride.
static bool own_layer() { return fs::is_directory(L"data\\titles"); }
static fs::path layer() {
    return own_layer() ? fs::path{L"data\\titles\\1A96545B\\tmp\\fh2-platform\\media"} : fs::path{L"FH2\\Mount\\mediaoverride"};
}

// `key` in `table` as the platform wrote it for the language `lang` (a zip's name: EN, PL, br).
static std::wstring str_in(const wchar_t* lang, const char* table, uint16_t key) {
    auto d = slurp(layer() / L"Stripped\\StringTables" / lang / table);
    if (d.empty()) return L"<none>";
    std::wstring found = L"<missing>";
    edit_lsb2(std::vector<char>(d.begin(), d.end()), [&](uint16_t k, const std::wstring& t) {
        if (k == key) found = t;
        return t;
    });
    return found;
}
static std::wstring str(const char* table, uint16_t key) { return str_in(L"EN", table, key); }

static bool ai_has(const wchar_t* name) {
    auto d = slurp(layer() / L"Stripped\\StringTables\\EN\\AIPlayers.str");
    bool hit = false;
    edit_lsb2(std::vector<char>(d.begin(), d.end()), [&](uint16_t, const std::wstring& t) {
        if (t == name) hit = true;
        return t;
    });
    return hit;
}

// AIPlayers.XP of `id` in the database override, or -1.
static int db_xp(int id) {
    const fs::path f = layer() / L"Stripped\\gamedbRC.slt";
    if (!fs::exists(f)) return -1;
    sqlite3* db = nullptr;
    sqlite3_stmt* st = nullptr;
    int xp = -1;
    if (sqlite3_open_v2(f.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(db, ("SELECT XP FROM AIPlayers WHERE Id = " + std::to_string(id)).c_str(), -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        xp = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return xp;
}

static bool has(const std::string& log, const char* s) { return log.find(s) != std::string::npos; }

// Whether XAML the platform wrote is well-formed: every element closed in order, comments and the declaration
// skipped, attribute values quoted (a page that isn't crashes the game when it opens). `why` says where not.
static bool well_formed(const std::string& x, std::string& why) {
    std::vector<std::string> open;
    for (std::size_t i = x.find('<'); i != std::string::npos; i = x.find('<', i)) {
        if (x.compare(i, 4, "<!--") == 0) {
            const std::size_t e = x.find("-->", i);
            if (e == std::string::npos) return why = "a comment isn't closed", false;
            i = e + 3;
            continue;
        }
        if (x.compare(i, 2, "<?") == 0) {
            i = x.find("?>", i);
            if (i == std::string::npos) return why = "the declaration isn't closed", false;
            continue;
        }
        std::size_t e = i + 1;
        char quote = 0;
        for (; e < x.size() && (quote || x[e] != '>'); ++e)
            if (x[e] == '"' || x[e] == '\'') quote = quote == x[e] ? 0 : quote ? quote : x[e];
        if (e >= x.size()) return why = "a tag isn't closed at " + std::to_string(i), false;
        const std::string tag = x.substr(i + 1, e - i - 1);
        const bool closing = !tag.empty() && tag[0] == '/', empty = !tag.empty() && tag.back() == '/';
        const std::string name = tag.substr(closing ? 1 : 0, tag.find_first_of(" \t\r\n/", 1) - (closing ? 1 : 0));
        if (closing) {
            if (open.empty() || open.back() != name) return why = "</" + name + "> doesn't match <" + (open.empty() ? "" : open.back()) + "> at " + std::to_string(i), false;
            open.pop_back();
        } else if (!empty) {
            open.push_back(name);
        }
        i = e + 1;
    }
    if (!open.empty()) return why = "<" + open.back() + "> isn't closed", false;
    return true;
}

// The HUD layer preview's start thread has finished, one way or another ("hud" phase).
static bool hud_started(const std::string& log) {
    return has(log, "[HUD layer] hooks: 4 of 4 made and on; the HUD layer is ready") || has(log, "[HUD layer] the Mods page's hooks didn't go in") ||
           has(log, "[HUD layer] the HUD layer is off: ") || has(log, " refused (see the line above); the HUD layer is off") ||
           has(log, "[HUD layer] the game's code never became ready");
}

// fh2mod.exe with these arguments: its exit code and what it printed (platform 2.8.0's commands).
static int run_cli(const std::wstring& args, std::string& out) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0)) return -1;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = w;
    si.hStdError  = w;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"fh2mod.exe " + args;
    if (!CreateProcessW(L"fh2mod.exe", cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(r);
        CloseHandle(w);
        return -1;
    }
    CloseHandle(w);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(r, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(r);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

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
    // the dump has the mods' data and the range AaaMod added (platform 2.8.0)
    const std::string plog_text = slurp("fh2-mods\\platform.log");
    CHECK(has(plog_text, "[AaaMod] add_dump_memory: 4096 bytes go in crash dumps") &&
          has(plog_text, "[AaaMod] add_dump_memory: needs memory of 1 byte to 64 MB; not added"));
    CHECK(has(plog_text, "with a small minidump (the data of ") && has(plog_text, "and 1 range(s) mods added)") &&
          !has(plog_text, "the data of 0 of the mods' DLLs"));
    // the session's status says so, with the report's name (platform 2.8.0)
    const std::string status = slurp("fh2-mods\\platform-session.json");
    CHECK(has(status, "\"state\": \"crashed\"") && has(status, "\"crash_report\": \"crashes\\\\crash-") &&
          has(status, "\"AaaMod\": \"0.5.0\""));
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

// All of the exe, as the hook test maps it: sections at their addresses, code read/execute, read-only data
// read-only, the headers last so the platform only finds the game once it's all there. The "hud" phase, so
// the HUD layer's code checks and hooks run against it.
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
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        std::memcpy(base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData,
                    std::min(sec[i].SizeOfRawData, sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD c = sec[i].Characteristics;
        DWORD prot    = c & IMAGE_SCN_MEM_EXECUTE ? PAGE_EXECUTE_READ : c & IMAGE_SCN_MEM_WRITE ? PAGE_READWRITE : PAGE_READONLY;
        DWORD old     = 0;
        VirtualProtect(base + sec[i].VirtualAddress, std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData), prot, &old);
    }
    DWORD old = 0;
    std::memcpy(base, file.data(), nt->OptionalHeader.SizeOfHeaders);
    VirtualProtect(base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    return base;
}

// The "crash-chained" phase (platform 2.8.1): a crash handler installed after the platform's that
// passes crashes on to the one it replaced, as the C runtime's does. 10 s after the game is found
// the platform puts its own back in front, and a crash must then go through each of them once,
// not back and forth between them until the stack overflows.
static LPTOP_LEVEL_EXCEPTION_FILTER g_replaced = nullptr;
static int g_chained_calls                     = 0;
static bool g_report_first                     = false;
static std::vector<fs::path> g_old_reports;

static int count(const std::string& s, const char* what) {
    int n = 0;
    for (std::size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
    return n;
}

static LONG WINAPI chained_handler(EXCEPTION_POINTERS* ep) {
    if (++g_chained_calls == 1)
        g_report_first = has(slurp("fh2-mods\\platform.log"), "crash report saved in fh2-mods\\crashes");
    if (g_chained_calls > 50) {
        std::printf("FAIL the crash handlers called each other %d times, on their way to a stack overflow\n",
                    g_chained_calls);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return g_replaced ? g_replaced(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI after_chained_crash(EXCEPTION_POINTERS*) {
    // The handler that was there before the platform's: the crash ends here.
    std::vector<fs::path> reports;
    for (const auto& e : fs::directory_iterator("fh2-mods\\crashes"))
        if (e.path().extension() == ".txt" &&
            std::find(g_old_reports.begin(), g_old_reports.end(), e.path()) == g_old_reports.end())
            reports.push_back(e.path());
    const std::string report = reports.size() == 1 ? slurp(reports[0]) : "";
    std::printf("---- crash report\n%s----\n", report.c_str());
    CHECK(g_chained_calls == 1);   // the other handler ran once,
    CHECK(g_report_first);         // after the platform's, which was back in front
    CHECK(reports.size() == 1);
    CHECK(has(report, "CRASH: access violation (0xC0000005) at azimuth.exe+0x") && has(report, "writing address 0x0"));
    CHECK(count(slurp("fh2-mods\\platform.log"), "crash report saved in fh2-mods\\crashes") == 1);
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails);
    return EXCEPTION_EXECUTE_HANDLER;
}

// FH2's exe header where Azimuth maps it: enough for the platform to find "the game".
static void fake_game() {
    auto* base = static_cast<char*>(VirtualAlloc(reinterpret_cast<void*>(0x140000000ull), 0x3A18000,
                                                 MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) return;
    auto* dos     = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    dos->e_magic  = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x100;
    auto* nt                       = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    nt->Signature                  = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine         = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp   = 0x565501ED; // 24 Nov 2015
    nt->OptionalHeader.SizeOfImage = 0x3A18000;
}

static int crash_chained() {
    std::string log;
    for (int i = 0; i < 250 && !has(log, "mod(s) loaded"); ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
    }
    g_replaced = SetUnhandledExceptionFilter(chained_handler);   // after the platform's
    fake_game();   // and 10 s from now the platform puts its own back in front
    const char* back = "crash reports: another crash handler was installed after the platform's; both run";
    for (int i = 0; i < 300 && !has(log, back); ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
    }
    CHECK(has(log, "mod(s) loaded") && has(log, back));
    if (fails) {
        std::printf("---- platform.log\n%s----\n\n%d FAILED\n", log.c_str(), fails);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), fails);
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("fh2-mods\\crashes", ec)) g_old_reports.push_back(e.path());
    *static_cast<volatile int*>(nullptr) = 1;
    Sleep(10000);
    std::printf("FAIL no crash\n");
    TerminateProcess(GetCurrentProcess(), 1);
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring phase = argc > 1 ? argv[1] : L"full";
    // Update-check cache: 6 hours, for the same version, never older than it.
    const long long t = 1'800'000'000, h = 3600;
    CHECK(cache_still_good(t, t + 5 * h, "v1.5.0", "1.4.0", "1.4.0"));
    CHECK(!cache_still_good(t, t + 7 * h, "v1.5.0", "1.4.0", "1.4.0"));  // too old
    CHECK(!cache_still_good(t, t + h, "v1.1.0", "1.1.0", "1.4.0"));      // made before 1.4.0 went in
    CHECK(!cache_still_good(t, t + h, "v1.1.0", "", "1.4.0"));           // a check from platform 2.2.0
    CHECK(!cache_still_good(t, t + h, "v1.3.0", "1.4.0", "1.4.0"));      // older than what's installed
    CHECK(cache_still_good(t, t + h, "v1.4.0", "1.4.0", "1.4.0"));
    CHECK(!cache_still_good(t, t - h, "v1.4.0", "1.4.0", "1.4.0"));      // clock went back
    CHECK(!cache_still_good(t, t + h, "", "1.4.0", "1.4.0"));
    if (phase == L"crash") {
        std::ofstream{"crash.txt"} << "1";
        SetUnhandledExceptionFilter(after_crash); // Windows' (or Azimuth's) handler, to the platform
    }
    if (phase == L"crash-chained") SetUnhandledExceptionFilter(after_chained_crash);
    if (phase == L"full") {
        // The page crash guard (platform 2.9.0): as if the game had crashed while TestMod's page Crashy was opening
        // (TestMod.dll as it is now: a crash report newer than the mark, made in 2015), had stopped while Other was
        // opening with no crash report since (marked in 2286), and while Stats was opening in an older TestMod.dll; and
        // a note about a mod that's gone. The stamp is page_guard.cpp's: the DLL's size, then its last-write time in
        // hex. The "removed" phase takes the crash report away again.
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(L"fh2-mods\\TestMod.dll", GetFileExInfoStandard, &a);
        char stamp[64];
        std::snprintf(stamp, sizeof stamp, "%llu-%08lX%08lX", (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
                      a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
        std::ofstream{"fh2-mods\\platform-pages.txt", std::ios::binary}
            << "# written by the test\r\nopening\tTestMod.dll\tCrashy\t" << stamp << "\t2026-11-07\t1\t01D0000000000000\r\n"
            << "opening\tTestMod.dll\tOther\t" << stamp << "\t2026-11-07\t1\t0300000000000000\r\n"
            << "opening\ttestmod.dll\tStats\t1-0000000000000000\t2026-11-06\r\nskip\tGone.dll\tList\t1-1\t2026-11-01\r\n";
        fs::create_directories("fh2-mods\\crashes");
        std::ofstream{"fh2-mods\\crashes\\crash-page-guard-test.txt"} << "a crash report, as far as the page crash guard can tell\n";
    }
    if (phase == L"removed") fs::remove("fh2-mods\\crashes\\crash-page-guard-test.txt");
    if (phase == L"full") {
        std::ofstream{"aaa_popups.txt"} << "1";
        std::ofstream{"fh2-mods\\platform-popups.ini", std::ios::binary} << "; written by the test\r\n[mods]\r\nAaaMod.dll=0\r\n";
    }
    if (phase == L"removed") {
        fs::remove("aaa_popups.txt");
        fs::remove("fh2-mods\\platform-popups.ini");
    }
    if (phase == L"newmod") {
        std::ofstream{"fh2-mods\\platform-popups.ini", std::ios::binary} << "[popups]\r\nDeveloper=1\r\n";
        std::ofstream{"aaa_popups.txt"} << "1";   // for "ui set" on AaaMod's Pop-ups row
    }
    if (phase == L"home")
        std::ofstream{"fh2-mods\\platform-popups.ini", std::ios::binary} << "[popups]\r\nDeveloper=1\r\n[player]\r\nModPopups=0\r\nTips=0\r\n";
    if (phase == L"full" && own_layer()) {   // a save, for the snapshot TestMod asks for (platform 2.8.0)
        fs::create_directories(L"data\\titles\\1A96545B\\saves\\1A96545B");
        std::ofstream{"data\\titles\\1A96545B\\saves\\1A96545B\\test.sav", std::ios::binary} << "save data";
    }
    HMODULE p = LoadLibraryW(L"d3dcompiler_47.dll"); // what azimuth.exe does at startup
    CHECK(p != nullptr);
    if (phase == L"crash-chained") return crash_chained();
    if (argc > 2) phase == L"hud" ? map_game(argv[2]) : map_image(argv[2]); // "press Play"
    // the online phases (platform 2.8.0): run after "removed", with OfflineMod and an FH2MP.dll
    const bool online_phase = phase.rfind(L"online", 0) == 0 || phase == L"fh2mp-off";
    // the HUD layer preview's phase: run last, with HudTestMod.dll added
    const bool hud_phase = phase == L"hud";
    // the cues about where mod settings are (platform 2.9.0): run after "removed", with platform-seen.txt edited
    const bool cue_phase = phase == L"upgrade" || phase == L"newmod" || phase == L"home";
    // the game's UI.zip is here (FH2_MEDIA), so the MODS menus are written: the title screen's line points at them
    const bool menus = fs::exists(L"FH2\\Mount\\media\\UI.zip");
    std::string log;
    for (int i = 0; i < 250; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if (cue_phase ? has(log, "mod(s) loaded")
            : hud_phase ? has(log, "mod(s) loaded") && has(log, "[HudTestMod] after init:") && has(log, "[HUD layer] page:") &&
                            (argc <= 2 || hud_started(log))
            : online_phase ? has(log, "mod(s) loaded") &&
                                 ((phase != L"online-late" && phase != L"online-ready") || has(log, "[OfflineMod] after start:")) &&
                                 ((phase != L"online-late" && phase != L"online-renamed" && phase != L"online-register" &&
                                   phase != L"online-ready") ||
                                  has(log, "[FH2MP] going_online:"))
                           : has(log, "mod(s) loaded") && has(log, "TestMod version") &&
                                 (phase == L"full" ? has(log, "late edit applied") && has(log, "save snapshot:")
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
        CHECK(has(log, "FH2 Mod Platform 2.9.0 (API 10)"));
        CHECK(has(log, "media overrides: Extra Options isn't loaded, so the platform mounts mediaoverride"));
        CHECK(log.find("loaded HelloMod.dll") != std::string::npos);
        CHECK(log.find("Legacy.dll has no fh2mod_init") != std::string::npos);
        // A mod from before the platform loads once the game is known to be FH2 (platform 2.8.0).
        CHECK(fs::exists("legacy_ran.txt") == (argc > 2));
        CHECK(has(log, "game: Azimuth started Forza Horizon 2 (the 24 Nov 2015 exe") == (argc > 2));
        CHECK(log.find("[TestMod] built for FH2 Mod Platform 9.9.9") != std::string::npos);
        CHECK(log.find("overrides another mod's string") != std::string::npos);
        // TestMod loads after HelloMod, so its edit of the same key wins.
        CHECK(str("LoadingMessages.str", 0x6875) == L"from TestMod");
        CHECK(ai_has(L"Buddy1") && ai_has(L"Buddy2") && !ai_has(L"Nocomply76") && ai_has(L"Womgoose"));
        const auto title = str("Main.str", 0x18F0);
        std::printf("title: [%ls]\n", title.c_str());
        // the platform's own line about where mod settings are comes last (fh2-mods\platform-title.txt), until
        // YOUR MODS has been opened (platform 2.9.0); with no MODS menus to point at (no UI.zip here), there's none
        if (menus) {
            CHECK(title == L"\n\n\n\n\nTO START\nTest notice line\nsecond line\nOld FH2 Mod Platform: reinstall TestMod\nMod settings: Pause > MODS (the last tab)");
            // TestMod's notice is tr()'s text (API 10): each language's Main.str has it in that language's words, with
            // the platform's line after it; English where there's no translation (TestMod's Brazilian Portuguese has
            // none). The platform's own line is in each language's words too (tr_in).
            CHECK(str_in(L"PL", "Main.str", 0x18F0) ==
                  L"\n\n\n\n\nTO START\nTestowa linia og\u0142oszenia\ndruga linia\nOld FH2 Mod Platform: reinstall TestMod\n"
                  L"Ustawienia mod\u00f3w: Pauza > MODS (ostatnia karta)");
            CHECK(str_in(L"br", "Main.str", 0x18F0) ==
                  L"\n\n\n\n\nTO START\nTest notice line\nsecond line\nOld FH2 Mod Platform: reinstall TestMod\n"
                  L"Configura\u00e7\u00f5es dos mods: Pausa > MODS (a \u00faltima aba)");
            // kept out of fh2-mods\notices, which older platforms read too (they'd show it in English at every start)
            CHECK(has(slurp(L"fh2-mods\\platform-title.txt"), "\nMod settings: Pause > MODS (the last tab)\n"));
            CHECK(has(log, "[MODS] title screen: where mod settings are, until YOUR MODS is opened"));
        } else {
            CHECK(title == L"\n\n\n\nTO START\nTest notice line\nsecond line\nOld FH2 Mod Platform: reinstall TestMod");
            CHECK(str_in(L"PL", "Main.str", 0x18F0) ==
                  L"\n\n\n\nTO START\nTestowa linia og\u0142oszenia\ndruga linia\nOld FH2 Mod Platform: reinstall TestMod");
            CHECK(!fs::exists(L"fh2-mods\\platform-title.txt") && !has(log, "[MODS] title screen:"));
        }
        CHECK(!fs::exists(L"fh2-mods\\notices\\~platform.txt"));
        CHECK(has(log, "this is the platform's first start in this Azimuth folder\n") ||
              has(log, "this is the platform's first start in this Azimuth folder\r\n"));
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") != std::string::npos);
        if (argc > 2) CHECK(log.find("the game is mapped at") != std::string::npos);

        // Files
        const fs::path ov = layer();
        CHECK(slurp(ov / "drivatarglobalcache" / "DefaultCache.bin") == "from AaaMod");
        CHECK(has(log, "[TestMod] can't replace media\\drivatarglobalcache\\DefaultCache.bin: AaaMod already does"));
        CHECK(has(log, "media\\Stripped\\StringTables\\EN\\Main.str is managed by the platform"));
        // the platform's own UI files: refused, however they're spelt (platform 2.9.0)
        CHECK(has(log, "[TestMod] set_file: \"UI\\Scenes\\Anthem\\A_PauseMenu.xaml\" is the FH2 Mod Platform's own UI\\Scenes\\Anthem\\A_PauseMenu.xaml "
                       "(the pause menu's MODS tab), which no mod can replace; it wasn't changed"));
        CHECK(has(log, "[TestMod] set_file: \"media\\UI.zip\\Scenes\\Anthem\\a_modpage_80.XAML.\" is the FH2 Mod Platform's own "
                       "UI\\Scenes\\Anthem\\A_ModPage_NN.xaml (the MODS tab's pages and the mods' pages)"));
        CHECK(has(log, "[TestMod] set_file: \"stateflow\\pause.xml\" is the FH2 Mod Platform's own stateflow\\pause.xml"));
        CHECK(has(log, "[TestMod] the platform's own files were refused"));
        CHECK(!has(slurp("fh2-mods\\platform-files.txt"), "A_PauseMenu.xaml\tTestMod") && !has(slurp("fh2-mods\\platform-files.txt"), "pause.xml\tTestMod"));
        CHECK(slurp(ov / "UI" / "Scenes" / "Anthem" / "A_PauseMenu.xaml") != "<xml/>" && slurp(ov / "stateflow" / "pause.xml") != "<xml/>");
        CHECK(has(log, "\"UI/../../evil.txt\" isn't a path under media"));
        CHECK(slurp(ov / "UI" / "keep.xml") == "<xml/>");
        CHECK(slurp(ov / "UI" / "handmade.xml") == "<xml/>");
        if (own_layer()) {   // the hand-placed file in mediaoverride stays as it was: the layer is read first
            CHECK(slurp(L"FH2\\Mount\\mediaoverride\\UI\\handmade.xml") == "made by hand");
            CHECK(!fs::exists(ov / "UI" / "handmade.xml.bak"));
        } else {
            CHECK(slurp(ov / "UI" / "handmade.xml.bak") == "made by hand");
        }
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

        // Pages (API 8)
        CHECK(has(log, "[TestMod] page: Test stats, on the Achievements tile"));
        CHECK(has(log, "[TestMod] add_page: there's no tile called Nope; Other is only on its mod's page under MODS > YOUR MODS"));
        CHECK(has(log, "[TestMod] add_page: there's already a page called Other"));
        CHECK(has(log, "[TestMod] set_page_rows: there's no page called Missing"));
        CHECK(has(log, "[TestMod] set_page_xaml: Stats's content has no element named FirstItem"));
        CHECK(has(log, "[TestMod] page slots ok"));
        CHECK(has(log, "[TestMod] set_page_xaml: Stats's content isn't well-formed (</Grid> doesn't match <Button>)"));
        CHECK(has(log, "[TestMod] set_page_rows: row 1 of Stats isn't the size of the first"));
        // pages of a mod's own design are checked against the game's UI.zip and textures before they're taken (platform
        // 2.9.0): without a UI.zip here, nothing to check against (and no page is written either)
        if (fs::exists(L"FH2\\Mount\\media\\UI.zip")) {
            CHECK(has(log, "set_page_xaml: pages of a mod's own design are checked against 1096 resource keys from UI.zip's 43 application dictionaries"));
            CHECK(has(log, "[TestMod] set_page_xaml: Design was refused: the resource key Test_NoSuchStyle, in the content, isn't one of the game's "
                           "shared resources, the page's own or the fragment's"));
            CHECK(has(log, "[TestMod] Design's XAML was refused") &&
                  has(log, "copy them into the fragment's resources). The page shows \"This page couldn't be shown\" until its XAML is set again"));
            // a page that already had XAML that passed keeps it when a later one is refused
            CHECK(has(log, "[TestMod] set_page_xaml: Stats was refused: the resource key Test_NoSuchKey, in the content,") &&
                  has(log, "The page keeps the XAML it had"));
        } else {
            CHECK(has(log, "set_page_xaml: pages of a mod's own design aren't checked this session, because AnthemApp.xaml couldn't be read from UI.zip"));
            CHECK(has(log, "[TestMod] Design's XAML was taken"));
        }
        // the page crash guard: the game crashed while Crashy was opening, so it's left out; it stopped while Other was
        // opening, with no crash report since, so that stays in; Stats was opening too, but in a TestMod.dll that has
        // changed since, so it gets another try
        CHECK(has(log, "pages: the game crashed while TestMod.dll's page Crashy was opening last session (crash report crash-page-guard-test.txt); "
                       "the page is left out until TestMod.dll changes (an update or a reinstall)"));
        CHECK(has(log, "pages: the game stopped while TestMod.dll's page Other was opening last session, but there's no crash report since, so "
                       "the page stays in"));
        CHECK(has(log, "pages: testmod.dll has changed since testmod.dll's page Stats was opening when the game stopped, so the page gets another try"));
        CHECK(has(log, "pages: Gone.dll isn't in fh2-mods any more, so the note about Gone.dll's page List is dropped"));
        const std::string guard = slurp("fh2-mods\\platform-pages.txt");
        CHECK(has(guard, "\r\nskip\tTestMod.dll\tCrashy\t") && has(guard, "\t2026-11-07\t1\t01D0000000000000\r\n") && !has(guard, "Stats") &&
              !has(guard, "Gone.dll") && !has(guard, "\tOther\t"));
        // title-screen entries (API 9)
        CHECK(has(log, "[TestMod] title-screen entry: (Y) DRIVE THE TEST GARAGE") &&
              has(log, "[TestMod] title-screen entry: (X) SECOND ENTRY (a list of 2)"));
        CHECK(has(log, "[TestMod] add_title_entry: Second can have 1 to 100 choices, not 101"));
        CHECK(has(log, "[TestMod] add_title_entry: Second's choice 1 has no label"));
        CHECK(has(log, "[TestMod] add_title_entry: Second has choices but no on_choose"));
        CHECK(has(log, "[TestMod] add_title_entry: there's already an entry called Garage"));
        CHECK(has(log, "[TestMod] add_title_entry: Y is TestMod's Garage; Third gets RB"));
        // cut at 32 characters, not 32 bytes (which left 27 of these), and in capitals beyond a-z (platform 2.9.0)
        CHECK(has(log, "[TestMod] add_title_entry: Third's label is cut to \"\xC5\xBB\xC3\xB3\xC5\x82ta trasa z bardzo d\xC5\x82ug\xC4\x85 nazw\xC4\x85\""));
        CHECK(has(log, "[TestMod] title-screen entry: (RB) \xC5\xBB\xC3\x93\xC5\x81TA TRASA Z BARDZO D\xC5\x81UG\xC4\x84 NAZW\xC4\x84\n") ||
              has(log, "[TestMod] title-screen entry: (RB) \xC5\xBB\xC3\x93\xC5\x81TA TRASA Z BARDZO D\xC5\x81UG\xC4\x84 NAZW\xC4\x84\r\n"));
        CHECK(has(log, "[TestMod] title-screen entry: (LB) FOURTH & <LAST> (a list of 20, in 3 pages of 8)"));
        CHECK(has(log, "[TestMod] add_title_entry: no button left for Fifth (four entries at most, from every mod)"));
        CHECK(has(log, "[TestMod] add_title_entry: \"bad key\" isn't a usable key (letters, digits, _)"));
        CHECK(has(log, "[TestMod] no title start yet"));
        CHECK(has(log, "[TestMod] tile text ok") && has(log, "[TestMod] set_page_tile_text: Other isn't on a tile") &&
              has(log, "[TestMod] set_page_tile_colors: Other isn't on a tile"));
        // choice rows and online play (API 9)
        CHECK(has(log, "[TestMod] choice row ok"));
        CHECK(has(log, "[TestMod] add_setting: One is a choice row but needs 2 to 32 choices"));
        CHECK(has(log, "[TestMod] add_setting: Lots is a choice row but needs 2 to 32 choices"));
        CHECK(has(log, "[TestMod] playing offline") && !has(log, "started from the FH2MP launcher"));
        // text in the game's language (API 10)
        CHECK(has(log, "[TestMod] no game language yet") && has(log, "[TestMod] setting text ok"));
        // translations (API 10): Polish last time (the runner's platform-language.ini), Polish and Brazilian Portuguese built
        // into TestMod (tests/l10n), and a Brazilian Portuguese file in fh2-mods\lang\TestMod that's used instead
        CHECK(has(log, "Language: mods' text starts in Polish (pl), the game's language last time, and follows the game's own once it has picked it"));
        CHECK(has(log, "[TestMod] language pl"));
        CHECK(has(log, "[TestMod] translation: Polish (pl), 6 strings"));
        CHECK(has(log, "[TestMod] translation: Brazilian Portuguese (pt-BR) from fh2-mods\\lang\\TestMod\\pt_BR.po, 2 strings; it's used instead "
                       "of any the mod has built in"));
        CHECK(has(log, "[TestMod] add_translation: pt-BR comes from fh2-mods\\lang\\TestMod\\pt_BR.po this time, not from the mod"));
        CHECK(has(log, "[TestMod] translation pt-BR (pt_BR.po): line 10, \"row.Bad.label\": its placeholders ({n\u00famero}) don't match the "
                       "English's ({count}), so it's left out and English shows there"));
        CHECK(has(log, "[TestMod] MODS row: Testowe powitanie \u2013 cze\u015b\u0107"));
        CHECK(has(log, "[TestMod] plural: 5 samochod\u00f3w"));
        CHECK(has(log, "[TestMod] the pl text for \"row.Stale.label\" was translated from different English, so it isn't used until it's "
                       "translated again") &&
              has(log, "[TestMod] stale: New English"));
        CHECK(has(log, "[TestMod] fuzzy: Not sure yet"));
        CHECK(has(log, "[TestMod] number: 12\u00a0" "345,5, date: 08.11.2026"));   // Polish groups from five digits
        // set_string_tr: each language's table in its own words
        CHECK(str("LoadingMessages.str", 0x5507) == L"A loading tip from TestMod");
        CHECK(str_in(L"PL", "LoadingMessages.str", 0x5507) == L"Wskaz\u00f3wka z TestMod");
        CHECK(str_in(L"br", "LoadingMessages.str", 0x5507) == L"Uma dica do TestMod");
        CHECK(str_in(L"PL", "LoadingMessages.str", 0x6875) == L"from TestMod");   // set_string's: the same in every language
        CHECK(has(log, "wrote LoadingMessages.str for 3 languages (2 key edits, 1 of them in each language's own words, "));
        CHECK(has(log, "[TestMod] set_setting_text: Button has 3 choices, not 2") &&
              has(log, "[TestMod] set_setting_text: there's no row called Nope"));
        // the UI templates (API 10): every member there; ui_uses records what a mod uses, from fh2mod_init only; pop-ups and
        // tips queue and log with no game (ui_status: nothing works yet, since their hooks wait for the game); with no
        // game and from no callback, no sound plays and no box shows; pages take their help and hints
        CHECK(has(log, "[TestMod] ui templates: all 12 members there"));
        CHECK(has(log, "[TestMod] ui_uses: 0x100 isn't a bit this platform knows, so it's ignored"));
        CHECK(has(log, "[TestMod] UI templates: it uses pop-ups, tips\n") || has(log, "[TestMod] UI templates: it uses pop-ups, tips\r\n"));
        CHECK(has(log, "[TestMod] UI templates: it uses pop-ups, tips, a pop-ups row of its own"));
        CHECK(has(log, "[TestMod] ui templates: ui_uses 1, ui_status 0, show_toast 1 then 1, show_tip 1, tip_seen 0 then 1, show_tip again 0, "
                       "the others 0"));
        CHECK(has(log, "[TestMod] ui_uses wasn't called from fh2mod_init, so it's ignored"));
        CHECK(has(log, "[TestMod] pop-up queued: Test pop-up / From TestMod (key Test)") &&
              has(log, "[TestMod] pop-up taken down while waiting (key Test)"));
        CHECK(has(log, "[TestMod] pop-up queued: key SignIn") && !has(log, "TestGamertag"));   // private: the key only
        CHECK(has(log, "[TestMod] show_toast: \"no spaces\" isn't a usable key (up to 32 letters, digits and _)"));
        CHECK(has(log, "[TestMod] tip FirstTip queued, to show after 60 s of driving: Press {button} to test") &&
              has(log, "[TestMod] tip FirstTip marked seen; it was waiting, so it won't show") &&
              has(log, "[TestMod] tip FirstTip not shown: it was seen already"));
        CHECK(has(log, "[TestMod] show_tip: tip NoSlot has a button, but its text has no {button} to put it in"));
        CHECK(has(log, "[TestMod] pop-up queued: Test pop-up / from the helper") && has(log, "[TestMod] ui helpers ok"));
        CHECK(!has(log, "[TestMod] shows pop-ups without saying so") && !has(log, "[Pop-ups]"));   // declared; no hooks without a game
        CHECK(!has(log, "isn't in this build yet"));   // every member is built
        // play_ui_sound: from fh2mod_init and from a thread of the mod's own, with no game, nothing plays (said once)
        {
            const std::string line = "[TestMod] play_ui_sound: the game isn't running yet, so SELECT wasn't played";
            CHECK(has(log, line.c_str()) && log.find(line, log.find(line) + 1) == std::string::npos);
            CHECK(has(log, "[TestMod] sound from a thread: 0") && !has(log, "so SHUTTER wasn't played") && !has(log, "[UI sounds]"));
        }
        // show_message_box: only from the mod's page, setting and title callbacks
        CHECK(has(log, "[TestMod] show_message_box: boxes show only from a callback of this mod's pages, settings or title-screen entries "
                       "(on_open, on_select, on_button, on_slot, on_change, a title entry's on_press or on_choose); send the player to its page "
                       "instead, so box Reset isn't shown"));
        // pages' help bars and hint lines
        CHECK(has(log, "[TestMod] ui templates: set_page_help 32, set_page_hints 2, show_page_hint 1, the wrong ones 0"));
        CHECK(has(log, "[TestMod] set_page_help: Stats's prompt 3 (LT): no callback of a page can answer that button, so it's left out") &&
              has(log, "[TestMod] set_page_help: Stats: its help fragment from set_page_xaml isn't used while it has these") &&
              has(log, "[TestMod] set_page_help: Stats's help bar: (A) Select (the game's word), (X) Hint, (Y) Ask {first}, from the next time it opens"));
        CHECK(has(log, "[TestMod] set_page_help: Tools's prompt 2 (X): a page of rows answers only A and B, so it's left out") &&
              has(log, "[TestMod] set_page_help: Tools's help bar: (A) Run it, (B) Back (the game's word), from the next time it opens"));
        CHECK(has(log, "[TestMod] set_page_help: there's no page called Missing") && has(log, "[TestMod] set_page_hints: there's no page called Missing"));
        CHECK(has(log, "[TestMod] set_page_hints: Stats has 2 hints") && has(log, "[TestMod] set_page_hints: Tools: hint 2 is cut to 80 characters") &&
              has(log, "[TestMod] set_page_hints: Tools has 3 hints"));
        CHECK(has(log, "[TestMod] page Tools's hint line: Ready. (attention), from the next time it opens"));
        CHECK(has(log, "[TestMod] show_page_hint: Tools has no hint 9 (it has 3; set them with set_page_hints)") &&
              has(log, "[TestMod] show_page_hint: 7 isn't a hint kind (FH2_HINT_INFO, _ATTENTION or _FAULT)"));
        // the tip TestMod marked seen is in platform-seen.txt, by its DLL's name (written on the store's own thread)
        {
            std::string seen;
            for (int i = 0; i < 50 && !has(seen = slurp(L"fh2-mods\\platform-seen.txt"), "TestMod.dll\ttip.FirstTip\tseen\t"); ++i) Sleep(100);
            CHECK(has(seen, "TestMod.dll\ttip.FirstTip\tseen\t"));
            // the platform's first launch here (no platform-mods.txt before it), and no MODS page opened yet
            CHECK(has(seen, "*\tfirst\tnew\t") && !has(seen, "\thome\t") && !has(seen, "mod.new"));
        }
        // commands (API 9): through fh2mod.exe, as a tool would
        {
            const std::wstring pid = L"--pid " + std::to_wstring(GetCurrentProcessId()) + L" ";
            std::string out;
            CHECK(run_cli(pid + L"TestMod echo hello world", out) == 0 && out == "hello world\n");
            out.clear();
            CHECK(run_cli(pid + L"list", out) == 0 && has(out, "TestMod echo  echo <text>: answers with the text\n") &&
                  has(out, "TestMod fail\n"));
            out.clear();
            CHECK(run_cli(pid + L"testmod ECHO case", out) == 0 && out == "case\n");   // names in any case
            out.clear();
            CHECK(run_cli(pid + L"TestMod fail", out) == 1 && out == "error: as asked\n");
            out.clear();
            CHECK(run_cli(pid + L"Nobody here", out) == 1 && has(out, "error: no mod called \"Nobody\" has commands"));
            out.clear();
            CHECK(run_cli(pid + L"TestMod nothing", out) == 1 && has(out, "error: TestMod has no command called \"nothing\""));
            const std::string pipe = "\\\\.\\pipe\\fh2mod-" + std::to_string(GetCurrentProcessId());
            log = slurp(L"fh2-mods\\platform.log");
            CHECK(has(log, ("commands: other programs on this PC (this Windows user's) can run mods' commands through " + pipe).c_str()));
            CHECK(has(log, "[TestMod] command: echo (echo <text>: answers with the text)") && has(log, "[TestMod] command: echo hello world"));
            // a pop-up sent from the command is a reply (API 10): the platform had just called it
            CHECK(has(log, "[TestMod] reply pop-up queued: Echo / hello world (key Echo)"));
            CHECK(has(log, "[TestMod] add_command: \"bad name\" isn't a usable name (letters, digits, _)") &&
                  has(log, "[TestMod] add_command: there's already a command called Echo"));
            CHECK(has(slurp("fh2-mods\\platform-session.json"), "\"pipe\": \"\\\\\\\\.\\\\pipe\\\\fh2mod-"));
        }
        // the UI templates' player controls (platform 2.9.0): AaaMod declared pop-ups, and the player turned its pop-ups off
        // (fh2-mods\platform-popups.ini), so its pop-up isn't shown. "fh2mod Platform ui" is there (TestMod's command
        // started the pipe): what's waiting and the player's choices, Recent pop-ups, and its tests need Developer=1.
        {
            for (int i = 0; i < 50 && !has(log = slurp(L"fh2-mods\\platform.log"), "[AaaMod] pop-up not queued"); ++i) Sleep(100);
            CHECK(has(log, "[AaaMod] UI templates: it uses pop-ups") &&
                  has(log, "[AaaMod] its pop-ups are off: the player turned them off on its page (fh2-mods\\platform-popups.ini)"));
            CHECK(has(log, "[AaaMod] pop-up not shown: the player turned this mod's pop-ups off (Hello from AaaMod / A test pop-up (key Hello))") &&
                  has(log, "[AaaMod] pop-up not queued"));
            CHECK(has(log, "[Platform] command: ui (ui [recent]: ") && !has(log, "Developer=1"));
            CHECK(!has(log, "[Pop-ups] the player turned"));   // mod pop-ups and tips are on
            const std::wstring pid = L"--pid " + std::to_wstring(GetCurrentProcessId()) + L" ";
            std::string out;
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "pop-ups: ") && has(out, "TestMod: pop-up waiting ") &&
                  has(out, "the player's choices (fh2-mods\\platform-popups.ini): mod pop-ups same as Notifications, tips on\n") &&
                  has(out, "aaamod.dll's pop-ups: off\n") && has(out, "the test commands (toast, tip, keyboard, pad, auto, demo, set) need Developer=1"));
            CHECK(!has(out, "TestGamertag"));   // private text stays out of it
            std::printf("---- fh2mod Platform ui\n%s----\n", out.c_str());
            out.clear();
            CHECK(run_cli(pid + L"Platform ui recent", out) == 0 &&
                  out == "No pop-ups yet  The pop-ups and tips mods show while you drive are listed here, newest first, with the mod that sent each.\n");
            out.clear();
            CHECK(run_cli(pid + L"platform UI toast Not now", out) == 1 && has(out, "error: ui toast needs Developer=1 in fh2-mods\\platform-popups.ini's [popups]"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui nonsense", out) == 1 && has(out, "error: ui has no \"nonsense\": ui, ui recent, "));
            out.clear();
            CHECK(run_cli(pid + L"list", out) == 0 && has(out, "Platform ui  ui [recent]: what pop-ups are on screen and waiting"));
        }
        // hotkeys (API 9): bindings named, rows on the mod's page, the refusals
        CHECK(has(log, "[TestMod] hotkey Mark (Mark this spot): F9, controller LB+RB+Y"));
        CHECK(has(log, "[TestMod] hotkey Toggle (Toggle the test): Ctrl+Shift+M, controller Off"));
        CHECK(has(log, "[TestMod] add_hotkey: there's already a hotkey called Mark") &&
              has(log, "[TestMod] add_hotkey: Broken needs a label and an on_press"));
        CHECK(has(log, "[TestMod] MODS row: Mark this spot: key") && has(log, "[TestMod] MODS row: Toggle the test: controller"));
        // sessions (API 9): the id in the log, the log's copy, our own log, the status file
        {
            const std::size_t at = log.find("[TestMod] session id ");
            std::string id       = at == std::string::npos ? "" : log.substr(at + 21, log.find('\n', at) - at - 21);
            while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
            CHECK(id.size() >= 17 && id[8] == '-' && id[15] == '-' && id.substr(16) == std::to_string(GetCurrentProcessId()));
            CHECK(has(log, ("session " + id + ": started ").c_str()));
            const std::string copy = slurp(fs::path(L"fh2-mods\\platform-logs") / ("platform-" + id + ".log"));
            CHECK(has(copy, "FH2 Mod Platform 2.9.0 (API 10)") && has(copy, "[TestMod] session id "));
            CHECK(slurp(fs::path(L"TestModData\\logs") / (id + ".log")) == "test mod's own log\n");
            CHECK(has(log, ("[TestMod] session log: TestModData\\logs\\" + id + ".log (the newest 3 kept)").c_str()));
            CHECK(has(log, "[TestMod] bad log folder refused") &&
                  has(log, "[TestMod] session_log_path: \"..\\Elsewhere\" isn't a folder name"));
            const std::string status = slurp("fh2-mods\\platform-session.json");
            // save snapshots (API 9): one copy, and the saves left as they were. Before Azimuth 1.0 there's
            // no data\titles, so no saves folder: the mod is told so.
            if (own_layer()) {
                CHECK(has(log, "[TestMod] asked for a copy of the saves before the game reads them: the platform test") &&
                      has(log, "[TestMod] save snapshot asked for"));
                CHECK(has(log, ("save snapshot: 1 file(s), 0 MB, copied to fh2-mods\\save-snapshots\\" + id +
                                " (the newest 2 kept), for TestMod (the platform test)").c_str()));
                CHECK(slurp(fs::path(L"fh2-mods\\save-snapshots") / id / L"1A96545B" / L"test.sav") == "save data");
                CHECK(slurp(L"data\\titles\\1A96545B\\saves\\1A96545B\\test.sav") == "save data");
            } else {
                CHECK(has(log, "[TestMod] request_save_snapshot: there's no saves folder at data\\titles\\1A96545B\\saves") &&
                      !has(log, "[TestMod] save snapshot asked for"));
            }
            CHECK(has(status, ("\"session\": \"" + id + "\"").c_str()) && has(status, "\"state\": \"running\"") &&
                  has(status, "\"platform\": \"2.9.0\"") && has(status, "\"TestMod\": \"1.0.0\""));
        }
        // The game's menus were given: the pages are written. (Not with the game mapped: this
        // test's copy of it doesn't pass the menus' code checks, so the platform takes them out.)
        if (fs::exists(L"FH2\\Mount\\media\\UI.zip") && argc <= 2) {
            const auto scenes    = ov / "UI" / "Scenes" / "Anthem";
            const auto installed = slurp(scenes / "A_ModPage_01.xaml");
            const auto tile      = slurp(scenes / "A_ModPage_80.xaml");
            CHECK(has(installed, "local:BindingProxy.A=\"Test stats &amp; more\"") && has(installed, "x:Name=\"PART_Button_FH2Mod_Page_0\""));
            CHECK(has(installed, "Content=\"TEST STATS\"") && has(installed, "{Binding FH2Mod_ViewIn_") && has(installed, "{Binding FH2Mod_Bar_3"));
            CHECK(has(installed, "Pause &gt; PROGRESS &gt; ACHIEVEMENTS opens it too."));
            CHECK(has(installed, "FH2Mod_InfoRow_Heading") && has(installed, "FH2Mod_InfoRow_Dim"));
            // TestMod's pages that can't be shown say why on its page under Installed mods, in red (the platform's own text
            // is English until the game has its language)
            CHECK(has(installed, "Icon=\"This page from TestMod has a mistake that would crash the game, so it's left out. fh2-mods\\platform.log has more.\""));
            CHECK(has(installed, "Icon=\"The game crashed opening this page last time, so it's left out until TestMod is updated. fh2-mods\\platform.log has more.\""));
            CHECK(has(installed, "Heading=\"Footer &lt;line&gt; &#183; dot\"") && has(installed, "local:BindingProxy.B=\"Alert\""));
            // the tile: the page of TestMod's own design, in the options page's frame
            CHECK(has(tile, "<Grid x:Name=\"TestContent\">") && !has(tile, "BasicOptions") && has(tile, "x:Key=\"FH2Mod_ShowIf\"") &&
                  !has(tile, "Test_NoSuchKey"));
            CHECK(has(tile, "<Color x:Key=\"Test_Color\">"));
            // its help bar is set_page_help's (API 10), not its help fragment: A in the game's word, X and Y as given (braces made
            // round), B Back; and its hint line under the content, in its three looks
            CHECK(!has(tile, "<Grid>\r\n<ContentControl x:Name=\"DefaultHelpPanel\" Style=") &&
                  has(tile, "local:ControllerButtonPanel.ADescription=\"{local:Loc HelpButtons.IDS_Select}\"") &&
                  has(tile, "local:ControllerButtonPanel.XDescription=\"Hint\"") && has(tile, "local:ControllerButtonPanel.YDescription=\"Ask (first)\"") &&
                  has(tile, "local:ControllerButtonPanel.BDescription=\"{local:Loc HelpButtons.IDS_Back}\"") && !has(tile, "LBumperDescription"));
            // (in a Grid: the content in its "*" row keeps a finite height, the hint line in the "Auto" row under it)
            CHECK(has(tile, "<RowDefinition Height=\"*\" />\r\n<RowDefinition Height=\"Auto\" />\r\n</Grid.RowDefinitions>\r\n<Grid Grid.Row=\"0\">\r\n"
                            "<Grid x:Name=\"TestContent\">") && has(tile, "<Grid Grid.Row=\"1\">\r\n                    <local:FZMaterial") &&
                  !has(tile, "<StackPanel>\r\n<Grid x:Name=\"TestContent\">") &&
                  has(tile, "{Binding FH2Mod_PanelShown, Converter={StaticResource FH2Mod_ShowIfHidden}}") &&
                  has(tile, "{Binding FH2Mod_PanelKind_2, Converter={StaticResource FH2Mod_ShowIf}}"));
            CHECK(has(tile, "x:Name=\"BottomBar\"") && has(tile, "x:Class =\"ForzaUI.A_HudOptions\""));
            // the Achievements tiles show the page's tile text
            const auto pause = slurp(scenes / "A_PauseMenu.xaml");
            const std::string text_tag = "Tag=\"{Binding FH2Mod_TileText_0, RelativeSource={RelativeSource AncestorType=Page, Mode=FindAncestor}}\"";
            const std::size_t first = pause.find(text_tag);
            CHECK(first != std::string::npos && pause.find(text_tag, first + 1) != std::string::npos);
            CHECK(has(pause, "<ControlTemplate x:Key=\"FH2Mod_Template_IconTile_0\"") &&
                  has(pause, "Template=\"{StaticResource FH2Mod_Template_IconTile_0}\""));
            CHECK(has(pause, "Color=\"{Binding FH2Mod_TileBack_0, RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}}\"") &&
                  has(pause, "{Binding FH2Mod_TileTitle_0,") && has(pause, "{Binding FH2Mod_TileLine_0,") && has(pause, "{Binding FH2Mod_TileIcon_0,"));
            // the MODS tab's own tiles: their names come from FH2Mod_HubTile_<k> (in the game's language), shown as their Tag
            bool hub_tags = true;
            for (int k = 0; k < 5; ++k)
                hub_tags = hub_tags && has(pause, ("Tag=\"{Binding FH2Mod_HubTile_" + std::to_string(k) +
                                                   ", RelativeSource={RelativeSource AncestorType=Page, Mode=FindAncestor}}\"").c_str());
            // and the YOUR MODS tile's subtitle and four lines of names, GET MODS' line (platform 2.9.0)
            for (int k = 5; k < 11; ++k)
                hub_tags = hub_tags && has(pause, ("{Binding FH2Mod_HubTile_" + std::to_string(k) + ", RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}").c_str());
            CHECK(hub_tags && !has(pause, "FH2Mod_HubTile_11") &&
                  has(pause, "<ControlTemplate x:Key=\"FH2Mod_Tile_Installed_Template\"") &&
                  has(pause, "<local:FZTextBlock Text=\"{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}\" Style=\"{StaticResource A_Style_FZTextBlock_TileTitle}\""));
            // the title screen: the entries' lines under PRESS (A) TO START, fading with it in all five states
            const auto splash = slurp(scenes / "A_Splash.xaml");
            std::size_t refs = 0;
            for (std::size_t at = splash.find("\"FH2Mod_TitleEntries\""); at != std::string::npos; at = splash.find("\"FH2Mod_TitleEntries\"", at + 1)) ++refs;
            CHECK(refs == 6);
            CHECK(has(splash, "{Binding FH2Mod_TitleReady}") && has(splash, "<Setter Property=\"Visibility\" Value=\"Collapsed\" />"));
            CHECK(has(splash, "A_BitmapImage_ControllerButton_Y}") && has(splash, "Text=\"DRIVE THE TEST GARAGE\""));
            CHECK(has(splash, "A_BitmapImage_ControllerButton_LB}") && has(splash, "Text=\"FOURTH &amp; &lt;LAST&gt;\""));
            CHECK(has(splash, "Text=\"&#379;&#211;&#321;TA TRASA Z BARDZO D&#321;UG&#260; NAZW&#260;\""));   // Polish capitals
            // the choice row on Settings > Mods: a slider shown like the game's option rows, a name per value
            const auto mods = slurp(scenes / "A_ModOptions.xaml");
            CHECK(has(mods, "Style=\"{StaticResource FH2Mod_Choice_") && has(mods, "<Setter Property=\"Maximum\" Value=\"2\" />"));
            CHECK(has(mods, "Text=\"LB\"") && has(mods, "Text=\"A &amp; &lt;B&gt;\" Style=\"{StaticResource A_Style_SliderControlText}\" "
                                                         "HorizontalAlignment=\"Center\" Visibility=\"Hidden\""));
            CHECK(has(mods, "<Trigger Property=\"Value\" Value=\"2\">") && has(mods, "A_Style_Arrow_Left") &&
                  has(mods, "<Setter TargetName=\"RightArrow\" Property=\"Visibility\" Value=\"Hidden\" />"));
            CHECK(!has(installed, "Load when playing online"));   // no FH2MP here: no such row
            // text given after add_setting and register_mod (API 10)
            CHECK(has(mods, "Test button, renamed") && !has(mods, "\"Test button\"") && has(mods, "Text=\"RB &amp; &lt;renamed&gt;\""));
            // a row from tr() (API 10): in Polish, the language last time
            CHECK(has(mods, "Testowe powitanie &#8211; cze&#347;&#263;") && has(mods, "Wiersz, kt&#243;rego tekst jest w j&#281;zyku gry."));
            CHECK(has(installed, "set later."));
            // the platform's own lines as whole sentences, one for each state (platform 2.9.0)
            CHECK(has(installed, "Heading=\"Version 0.5.0 (doesn't check for updates)\""));
            CHECK(has(installed, "It isn't working: It needs FH2 Mod Platform 9.9.9 or newer; this is 2.9.0. Reinstall it to get the "
                                 "platform it came with. fh2-mods\\platform.log has more."));
            // "One home" (platform 2.9.0): YOUR MODS, "MODS / YOUR MODS", a row per mod ending in ">" with what A shows
            CHECK(has(installed, "Tag=\"MODS\" Content=\"YOUR MODS\""));
            CHECK(has(installed, "local:BindingProxy.A=\"TestMod\" local:BindingProxy.C=\"\" local:BindingProxy.B=\"! Not working\"") &&
                  has(installed, "  &gt;\" local:BindingProxy.B=\"\""));
            CHECK(has(installed, "Press A for its ") && has(installed, "Press A to see what's wrong."));
            CHECK(has(installed, "{Binding FH2Mod_PanelText}") && has(installed, "{Binding FH2Mod_PanelShown,"));
            // the hint line (API 10): the platform's own text as it was, a mod's in orange with the game's info icon or in red
            // with its alert icon
            CHECK(has(installed, "Color=\"{StaticResource A_Color_SubHeader_Text}\" Style=\"{StaticResource A_Style_FZTextBlock_Body_Tight}\" "
                                 "TextWrapping=\"Wrap\" Visibility=\"{Binding FH2Mod_PanelKind_0, Converter={StaticResource FH2Mod_ShowView}}\"") &&
                  has(installed, "Color=\"#FFFE5723\"") && has(installed, "Color=\"#FFE0141E\"") &&
                  has(installed, "ImageSource=\"/ForzaUI;component/Textures/Anthem/HUD/InfoIconSmall.png\" Visibility=\"{Binding FH2Mod_PanelKind_1,") &&
                  has(installed, "ImageSource=\"/ForzaUI;component/Textures/Anthem/Icons/Alert_Icon.png\" Visibility=\"{Binding FH2Mod_PanelKind_2,"));
            // pages with help bars of their own (API 10): Tools' (A) Run it and (B) Back, Stats' A in the game's word (a view
            // of rows keeps only A and B), each shown with its view instead of its kind's bar
            {
                const std::size_t tools = installed.find("local:ControllerButtonPanel.ADescription=\"Run it\"");
                CHECK(tools != std::string::npos && installed.rfind("x:Name=\"FH2Mod_ViewHelpPanel_P", tools) != std::string::npos &&
                      has(installed, "{Binding FH2Mod_HelpBar_") && !has(installed, "XDescription=\"Hint\""));
                std::size_t own = 0;
                for (std::size_t at = installed.find("x:Name=\"FH2Mod_ViewHelpPanel_P"); at != std::string::npos;
                     at = installed.find("x:Name=\"FH2Mod_ViewHelpPanel_P", at + 1))
                    ++own;
                CHECK(own == 2);
            }
            CHECK(has(installed, "FH2Mod_InfoRow_Update") && has(installed, "FH2Mod_Color_Yellow"));
            // each mod's page: "MODS / TESTMOD"; its own rows, its hotkeys under "Keys and buttons", its page, then About,
            // What it does and Load this mod (marked *: it takes a restart) at the bottom
            {
                // (the rows' data items, in the page's resources in the order of each view's rows)
                const std::size_t own = installed.find("Heading=\"Test button, renamed\""), keys = installed.find("Heading=\"Keys and buttons\"");
                const std::size_t key_row = installed.find("Heading=\"Mark this spot: key\""), page = installed.find("Heading=\"Test stats &amp; more\"");
                const std::size_t about = installed.find("Heading=\"About TestMod\"");
                const std::size_t what = installed.find("Heading=\"What it does\"", about), load = installed.find("Heading=\"Load this mod *\"", about);
                CHECK(own != std::string::npos && own < keys && keys < key_row && key_row < page && page < about && about < what && what < load &&
                      load != std::string::npos);
                CHECK(has(installed, "Tag=\"MODS\" Content=\"TESTMOD\"") && has(installed, "Label=\"Load this mod *\""));
                CHECK(has(installed, "Description=\"Off leaves it in fh2-mods, but the game doesn't load it. Takes effect the next time you start the game.\""));
                CHECK(!has(installed, "(TestMod)\"") && !has(installed, "Load TestMod"));   // no "(Mod)" after descriptions; the old switch
                CHECK(has(installed, "x:Name=\"FH2Mod_R1_1\"") && has(installed, "Heading=\"Not working\""));
            }
            // the UI templates' player controls (platform 2.9.0): "FH2 Mod Platform" after the mods on YOUR MODS, opening the
            // platform's page: Mod pop-ups (Same as Notifications / Off), Tips, Show tips again and Recent pop-ups (its own
            // view, with one row while nothing has shown); no UI templates page without Developer=1. AaaMod's page has the
            // platform's Pop-ups row; TestMod has a row of its own (FH2_UI_USES_OWN_SWITCH), so it has none.
            {
                CHECK(has(installed, "local:BindingProxy.A=\"FH2 Mod Platform\" local:BindingProxy.C=\"2.9.0  &gt;\""));
                CHECK(has(installed, "Heading=\"FH2 Mod Platform\" Description=\"Pop-ups and tips from every mod, and the last ones shown. Press A to open it.\""));
                CHECK(has(installed, "Tag=\"MODS\" Content=\"FH2 MOD PLATFORM\"") && has(installed, "Content=\"RECENT POP-UPS\""));
                const std::size_t heading = installed.find("Heading=\"Pop-ups and tips\""), choice = installed.find("Heading=\"Mod pop-ups\"");
                const std::size_t tips = installed.find("Heading=\"Tips\""), again = installed.find("Heading=\"Show tips again\"");
                const std::size_t recent = installed.find("Heading=\"Recent pop-ups\""), none = installed.find("Heading=\"No pop-ups yet\"");
                CHECK(heading != std::string::npos && heading < choice && choice < tips && tips < again && again < recent && recent < none &&
                      none != std::string::npos);
                CHECK(has(installed, "Text=\"Same as Notifications\"") && has(installed, "Label=\"Tips\""));
                CHECK(has(installed, "Description=\"Every mod's tips show again, once each, as if they were new. Press A to show them again.\""));
                CHECK(!has(installed, "UI templates") && !has(installed, "Everything in turn"));
                CHECK(has(installed, "Description=\"Off: AaaMod shows no pop-ups. Mod pop-ups, on the FH2 Mod Platform's page under YOUR MODS, turns every "
                                     "mod's off.\"") &&
                      !has(installed, "Off: TestMod shows no pop-ups."));
                // the list that stands in for the MODS tab has the platform's two rows, and AaaMod's
                CHECK(has(mods, "Label=\"Tips\"") && has(mods, "Text=\"Same as Notifications\"") && has(mods, "Off: AaaMod shows no pop-ups."));
            }
            // HELP in ALL SETTINGS' place: information only, so no A on its help bar
            const auto help = slurp(scenes / "A_ModPage_06.xaml");
            CHECK(has(help, "Content=\"HELP\"") && has(help, "Heading=\"Where mod settings are\"") && has(help, "Heading=\"Turning a mod off\"") &&
                  !has(help, "ADescription") && !has(help, "Heading=\"Online play\""));
            // the list that stands in for the MODS tab when its hooks don't go in: "SETTINGS / MODS", a heading per mod
            CHECK(has(mods, "Tag=\"{local:Loc ScreenTitles.IDS_MyProfile}\"") && has(mods, "Content=\"MODS\"") &&
                  has(mods, "local:BindingProxy.A=\"TestMod\" local:BindingProxy.C=\"\" local:BindingProxy.B=\"\" Template=\"{StaticResource FH2Mod_InfoRow_Heading}\"") &&
                  has(mods, "Heading=\"More for each mod\"") && has(mods, "* Takes effect the next time you start the game.") && !has(mods, "ADescription"));
            // the tab: HELP's tile, the NEW badge (the player hasn't opened YOUR MODS), and the badges on the tiles
            CHECK(has(pause, "x:Key=\"FH2Mod_Tile_Help\"") && has(pause, "{Binding FH2Mod_NewBadge}") && has(pause, "{Binding FH2Mod_TileNewBadge,") &&
                  has(pause, "{Binding FH2Mod_TileUpdateBadge,") && has(pause, "x:Key=\"FH2Mod_Style_TileBadge_Red\""));
            // the Settings door: its MODS tile, with the line under its name
            const auto options = slurp(scenes / "A_PauseOptions.xaml");
            CHECK(has(options, "x:Name=\"PART_Button_Mods\"") && has(options, "Template=\"{StaticResource FH2Mod_Template_ModsTile}\"") &&
                  has(options, "{Binding FH2Mod_HubTile_11,"));
            // every page the platform wrote is well-formed XAML
            for (const auto& e : fs::directory_iterator(scenes)) {
                std::string why;
                const bool ok = well_formed(slurp(e.path()), why);
                std::printf("%s %ls is well-formed%s%s\n", ok ? "ok  " : "FAIL", e.path().filename().c_str(), ok ? "" : ": ", why.c_str());
                if (!ok) ++fails;
            }
        }
        // No mod adds a HUD layer here, so the preview behaves as 2.8.0: nothing of the layer in the log,
        // no file of its own, no layer in any A_Hud.xaml written; only line 1 names the build.
        CHECK(!has(log, "[HUD layer]"));
        bool hud_files = false;
        for (const auto& e : fs::directory_iterator("fh2-mods")) hud_files = hud_files || e.path().filename().wstring().rfind(L"platform-hudlayer", 0) == 0;
        CHECK(!hud_files);
        bool hud_page = false;
        for (const auto& e : fs::recursive_directory_iterator("."))
            if (e.path().filename() == L"A_Hud.xaml") hud_page = hud_page || has(slurp(e.path()), "FH2 Mod Platform: HUD layer");
        CHECK(!hud_page);
        CHECK(has(log.substr(0, log.find('\n')), "; this is the 2.9.0 dev build, not a release"));
    } else if (hud_phase) {
        // HudTestMod adds a layer with every part; a second one and one after fh2mod_init are refused
        CHECK(has(log, "[HudTestMod] interface found"));
        CHECK(has(log, "[HudTestMod] HUD layer added: BAR, CHIPS, TIP, PANEL, COUNTDOWN; lease 2000 ms") && has(log, "[HudTestMod] add_hud_layer: 1"));
        CHECK(has(log, "[HudTestMod] add_hud_layer: this mod already has one; no HUD layer") && has(log, "[HudTestMod] add_hud_layer again: 0"));
        CHECK(has(log, "[HudTestMod] add_hud_layer: it was called after fh2mod_init: the HUD page is written once every mod's fh2mod_init has "
                       "returned; no HUD layer") &&
              has(log, "[HudTestMod] after init: 0"));
        CHECK(has(log, "[HudTestMod] set_hud_state: 1"));
        CHECK(has(log, "[HUD layer] 1 mod(s) asked for a HUD layer: HudTestMod; the HUD page gets BAR, CHIPS, TIP, PANEL, COUNTDOWN"));
        CHECK(has(log, "[HUD layer] button prompts: Automatic (fh2-mods\\platform-hudlayer.ini)"));
        CHECK(has(log, "[HudTestMod] MODS row: Button prompts"));   // settings_add's own line, as the hotkeys' rows
        CHECK(has(log, "[HUD layer] crash marker: none from last session"));
        const std::string props  = slurp("fh2-mods\\platform-hudlayer-props.txt");
        const std::string marker = slurp("fh2-mods\\platform-hudlayer-marker.txt");
        std::size_t lines = 0;
        for (std::size_t at = props.find("\r\n"); at != std::string::npos; at = props.find("\r\n", at + 2)) ++lines;
        CHECK(lines == 316 && props.rfind("FH2Mod_Hud_Shown\tbool\tfalse\t\r\n", 0) == 0);
        if (fs::exists(L"FH2\\Mount\\media\\UI.zip")) {
            CHECK(has(log, "[HUD layer] check: 40 resource keys used, all in UI.zip's application dictionaries (43 dictionaries, 1096 keys)"));
            CHECK(has(log, "[HUD layer] page: wrote A_Hud.xaml with the layer (BAR, CHIPS, TIP, PANEL, COUNTDOWN): 316 properties, 588 bindings (at "
                           "most 640), 290 DataTriggers, ") &&
                  has(log, " bytes; a copy is in fh2-mods\\platform-hudlayer-A_Hud.xaml"));
            const std::string copy = slurp("fh2-mods\\platform-hudlayer-A_Hud.xaml");
            CHECK(has(copy, "<!-- FH2 Mod Platform: HUD layer -->") && has(copy, "FH2HL_Root"));
            // The page as the game reads it: where set_file writes in this layout (layer()). With the game mapped, only if the Mods
            // page's hooks went in: the platform removes its pages when they don't.
            if (argc <= 2 || has(log, "[HUD layer] hooks: 4 of 4 made and on; the HUD layer is ready")) {
                const std::string page = slurp(layer() / L"UI\\Scenes\\Anthem\\A_Hud.xaml");
                CHECK(has(page, "<!-- FH2 Mod Platform: HUD layer -->") && has(page, "FH2HL_Root") && page == copy);
            }
            CHECK(has(marker, "state written"));
            CHECK(has(log, "[HUD layer] crash marker: written"));
        } else {
            CHECK(has(log, "[HUD layer] page: the layer is left out: A_Hud.xaml couldn't be read from the game's UI.zip"));
            CHECK(!fs::exists("fh2-mods\\platform-hudlayer-A_Hud.xaml") && marker.empty());
        }
        CHECK(has(slurp("fh2-mods\\platform-session.json"), "\"hud_layer\": \"preview 3: HudTestMod\""));
        if (argc > 2) {
            // the game mapped: the start thread checks the code and makes its hooks, or says why not
            const bool ready = has(log, "[HUD layer] hooks: 4 of 4 made and on; the HUD layer is ready");
            const bool no_props = has(log, "[HUD layer] the Mods page's hooks didn't go in, so the game can't read the layer's values; the HUD layer is off");
            std::printf("HUD layer start: %s\n", ready ? "ready (L18)" : no_props ? "the Mods page's hooks didn't go in (L17)" : hud_started(log) ? "off (see the log)" : "not finished");
            CHECK(ready || no_props);
        }
        // no fault of the layer's in any case
        CHECK(!has(log, "crashed in the game's code") && !has(log, "[HUD layer] notify crashed"));
    } else if (online_phase) {
        // no mod here declares pop-ups or tips, so the platform leaves the game's notification alone (2.9.0)
        CHECK(has(log, "[Pop-ups] pop-ups and tips are off this session: no mod declared pop-ups or tips in fh2mod_init"));
        const auto known = slurp("fh2-mods\\platform-mods.txt");
        const bool pages = fs::exists(L"FH2\\Mount\\media\\UI.zip") && argc <= 2;
        const auto installed = pages ? slurp(layer() / L"UI\\Scenes\\Anthem\\A_ModPage_01.xaml") : std::string();
        if (phase == L"online-first") {
            // OfflineMod is new: it loads, register_mod turns it away, and it's remembered as offline-only
            CHECK(has(log, "started from the FH2MP launcher (online play)"));
            CHECK(has(log, "FH2MP.dll (FH2MP) is turned on and FH2MP's session file is there (data\\bin\\fh2mp-session.txt), "
                           "so this game counts as online play"));
            CHECK(has(log, "loaded OfflineMod.dll") &&
                  has(log, "[OfflineMod] 1.0.0 doesn't run in online play (its author says so), so it isn't started"));
            CHECK(!has(log, "OfflineMod.dll: fh2mod_init returned") && !has(log, "[OfflineMod] running"));
            CHECK(has(known, "OfflineMod.dll\tOfflineMod\t1.0.0\tMade for offline play.\t\t1"));
            if (pages) {
                CHECK(has(installed, "local:BindingProxy.C=\"Off online  1.0.0  &gt;\""));
                // each mod's page is a view of YOUR MODS: OfflineMod's has the switch, FH2MP's doesn't
                CHECK(has(installed, "Heading=\"Load when playing online\"") && has(installed, "Its author made it for offline play"));
                CHECK(has(installed, "the game doesn't load OfflineMod, so it can't change online play. Its author made it for offline play, so "
                                     "it's off unless you turn it on. Takes effect the next time you start the game from the launcher."));
                CHECK(!has(installed, "the game doesn't load FH2MP"));
                CHECK(!has(slurp(layer() / L"UI\\Scenes\\Anthem\\A_Hud.xaml"), "OfflineMod"));   // not an error
            }
        } else if (phase == L"online") {
            // ... and from then on isn't loaded at all online
            CHECK(has(log, "OfflineMod.dll is set to stay off when playing online; not loaded") && !has(log, "loaded OfflineMod.dll"));
            CHECK(has(log, "loaded HelloMod.dll") && has(log, "FH2MP.dll has no fh2mod_init"));
            if (pages) CHECK(has(installed, "local:BindingProxy.C=\"Off online  1.0.0  &gt;\""));
            // OfflineMod is new since the launch before: "New" on its row until its page is opened, the HUD says so
            CHECK(has(slurp("fh2-mods\\platform-seen.txt"), "OfflineMod.dll\tmod.new\t"));
        } else if (phase == L"online-chosen") {
            // the player's choices (platform-online.txt) win over the author's
            CHECK(has(log, "[OfflineMod] running online"));
            CHECK(has(log, "HelloMod.dll is set to stay off when playing online; not loaded"));
        } else if (phase == L"online-root") {
            // issue #22: in an Azimuth folder without data\bin, FH2MP's session is next to azimuth.exe
            CHECK(has(log, "FH2MP.dll (FH2MP) is turned on and FH2MP's session file is there (fh2mp-session.txt next to "
                           "azimuth.exe), so this game counts as online play"));
            CHECK(has(log, "started from the FH2MP launcher (online play)"));
            CHECK(has(log, "OfflineMod.dll is set to stay off when playing online; not loaded") && !has(log, "loaded OfflineMod.dll"));
        } else if (phase == L"online-late") {
            // issue #22: FH2MP under another name (ZzFH2MP.dll, new here), and no session when the loader looks: offline,
            // so OfflineMod starts and hooks. FH2MP's session shows up while it starts (it writes it itself here, as a
            // Play pressed meanwhile would): online once its fh2mod_init returns, and OfflineMod is stopped.
            CHECK(!has(log, "started from the FH2MP launcher") && !has(log, "counts as online play"));
            CHECK(has(log, "[OfflineMod] running offline") && has(log, "[OfflineMod] enabled 1 hook"));
            CHECK(has(log, "[FH2MP] wrote the session file in fh2mod_init") && has(log, "[FH2MP] in fh2mod_init: offline"));
            CHECK(has(log, "online play after all: FH2MP's session showed up while the mods were starting (FH2MP's session file "
                           "is there (data\\bin\\fh2mp-session.txt))"));
            CHECK(has(log, "[OfflineMod] disabled 1 hook") && has(log, "[OfflineMod] stopped for online play"));
            // it stays stopped: the original runs, enable_hooks is refused
            CHECK(has(log, "[OfflineMod] can't enable hooks: it was stopped for online play"));
            CHECK(has(log, "[OfflineMod] after start: online=1, the original ran, enable_hooks gave 0, set_file gave 0"));
            CHECK(!has(log, "[HelloMod] stopped for online play") && !has(log, "[FH2MP] stopped for online play"));
            // its text, SQL and file (FH2_TEST_OFFLINE_EDITS) are out before the loader's first write, and new ones refused
            CHECK(has(log, "[OfflineMod] replaced media\\UI\\OfflineMod.txt") &&
                  has(log, "[OfflineMod] stopped replacing media\\UI\\OfflineMod.txt"));
            CHECK(has(log, "[OfflineMod] set_file: media\\UI\\OfflineMod.txt isn't replaced: this mod was stopped for online play"));
            CHECK(!fs::exists(layer() / L"UI\\OfflineMod.txt"));
            CHECK(str("LoadingMessages.str", 0x6875) != L"from OfflineMod" && db_xp(1) != 4242);
            CHECK(!has(log, "[OfflineMod] database:"));
            // what FH2MP is told before it connects
            CHECK(has(log, "[FH2MP] going_online: 1 started (OfflineMod; cut short \"Off\"), playing_online 1"));
            CHECK(has(known, "ZzFH2MP.dll\tFH2MP\t0.0.1"));
            if (pages) CHECK(has(installed, "local:BindingProxy.C=\"Off online  1.0.0  &gt;\"") && has(installed, "Stopped for online play"));
        } else if (phase == L"online-renamed") {
            // ... and from then on it's known as FH2MP: online from the start, OfflineMod isn't loaded
            CHECK(has(log, "ZzFH2MP.dll (FH2MP) is turned on and FH2MP's session file is there (data\\bin\\fh2mp-session.txt), "
                           "so this game counts as online play"));
            CHECK(has(log, "OfflineMod.dll is set to stay off when playing online; not loaded") && !has(log, "loaded OfflineMod.dll"));
            CHECK(has(log, "[FH2MP] in fh2mod_init: online") && !has(log, "online play after all"));
            CHECK(has(log, "[FH2MP] going_online: 0 started (; cut short \"\"), playing_online 1"));
            if (pages) CHECK(!has(installed, "the game doesn't load FH2MP"));   // no "Load when playing online" for FH2MP
        } else if (phase == L"online-register") {
            // FH2MP as MpClient.dll, which the loader doesn't know, with its session there all along: offline at load, online
            // when it registers. HelloMod (before it, the player's switch off) is stopped; TestMod (after it, switch off, not
            // offline-only) and OfflineMod (after it, remembered as offline-only) aren't started.
            CHECK(!has(log, "started from the FH2MP launcher") && !has(log, "counts as online play"));
            CHECK(has(log, "online play after all: FH2MP started from MpClient.dll, and FH2MP's session file is there "
                           "(data\\bin\\fh2mp-session.txt)"));
            CHECK(has(log, "[FH2MP] in fh2mod_init: online") && !has(log, "[FH2MP] wrote the session file"));
            CHECK(has(log, "[HelloMod] stopped for online play") && !has(log, "[AaaMod] stopped for online play") &&
                  !has(log, "[FH2MP] stopped for online play"));
            CHECK(has(log, "loaded TestMod.dll") &&
                  has(log, "TestMod.dll is set to stay off when playing online; not started (FH2MP went online after it was loaded)"));
            CHECK(!has(log, "[TestMod] ") && !has(log, "TestMod.dll: fh2mod_init returned"));
            CHECK(has(log, "OfflineMod.dll is set to stay off when playing online; not started (FH2MP went online after it was loaded)"));
            CHECK(!has(log, "[OfflineMod] running"));
            CHECK(has(log, "[FH2MP] going_online: 1 started (HelloMod; cut short \"Hel\"), playing_online 1"));
            CHECK(has(known, "MpClient.dll\tFH2MP\t0.0.1"));
        } else if (phase == L"online-ready") {
            // FH2MP (MpClient.dll, known now) has no session: offline, so OfflineMod starts, and its text, SQL and file are
            // written. FH2MP's going_online, once every mod has started, makes the game online: OfflineMod and HelloMod (the
            // player's switch) are stopped, and the tables and the database are written again without them.
            CHECK(!has(log, "started from the FH2MP launcher") && !has(log, "counts as online play"));
            CHECK(has(log, "[OfflineMod] running offline") && has(log, "[FH2MP] in fh2mod_init: offline"));
            CHECK(has(log, "[OfflineMod] replaced media\\UI\\OfflineMod.txt") && has(log, "[OfflineMod] database: "));
            CHECK(has(log, "online play after all: FH2MP said it's going online (FH2Platform.Online)"));
            CHECK(has(log, "[OfflineMod] stopped for online play") && has(log, "[HelloMod] stopped for online play"));
            CHECK(has(log, "[OfflineMod] stopped replacing media\\UI\\OfflineMod.txt"));
            CHECK(has(log, "[FH2MP] going_online: 2 started (HelloMod, OfflineMod; cut short \"Hel\"), playing_online 1"));
            CHECK(!fs::exists(layer() / L"UI\\OfflineMod.txt"));
            const auto text = str("LoadingMessages.str", 0x6875);
            std::printf("LoadingMessages 0x6875 after going_online: [%ls], AIPlayers 1 XP %d\n", text.c_str(), db_xp(1));
            CHECK(text != L"from OfflineMod" && db_xp(1) != 4242);
        } else {
            // fh2mp-off: FH2MP turned off on the MODS tab, so a session file doesn't make it online
            CHECK(!has(log, "started from the FH2MP launcher") && has(log, "FH2MP.dll is turned off on the MODS tab"));
            CHECK(has(log, "[OfflineMod] running offline") && has(log, "loaded HelloMod.dll"));
        }
    } else if (cue_phase) {
        // The cues about where mod settings are, kept in platform-seen.txt (platform 2.9.0), so a second start of the
        // game in the same launch (Azimuth can do that) still has them: platform-mods.txt is written again at once.
        const std::string seen  = slurp("fh2-mods\\platform-seen.txt");
        const std::wstring title = str("Main.str", 0x18F0);
        std::printf("title: [%ls]\n", title.c_str());
        const bool pages = fs::exists(L"FH2\\Mount\\media\\UI.zip") && argc <= 2;
        const std::string hud = pages ? slurp(layer() / L"UI\\Scenes\\Anthem\\A_Hud.xaml") : std::string();
        if (pages) {
            std::string why;
            const bool ok = well_formed(hud, why);
            std::printf("%s A_Hud.xaml is well-formed%s%s\n", ok ? "ok  " : "FAIL", ok ? "" : ": ", why.c_str());
            if (!ok) ++fails;
        }
        CHECK(!fs::exists(L"fh2-mods\\notices\\~platform.txt"));
        if (phase == L"upgrade") {
            // platform-mods.txt there and platform-seen.txt not: an older platform ran here, so it's an upgrade
            CHECK(has(seen, "*\tfirst\tupgrade\t"));
            CHECK(has(log, "this is the platform's first start in this Azimuth folder; an older platform ran here before"));
            if (menus) {
                CHECK(title == L"\n\nTO START\nMod settings have moved: Pause > MODS" &&
                      str_in(L"PL", "Main.str", 0x18F0) == L"\n\nTO START\nUstawienia modów są teraz tu: Pauza > MODS");
                CHECK(has(slurp(L"fh2-mods\\platform-title.txt"), "\nMod settings have moved: Pause > MODS\n"));
                CHECK(has(log, "[MODS] title screen: mod settings have moved"));
            }
        } else if (phase == L"newmod") {
            // OfflineMod is new, and the welcome has had its three sessions: the HUD's notice is NEW MOD. The upgrade
            // is still told (the HUD hasn't been shown since).
            CHECK(has(seen, "OfflineMod.dll\tmod.new\t") && has(seen, "*\tfirst\tupgrade\t"));
            CHECK(has(slurp("fh2-mods\\platform-mods.txt"), "OfflineMod.dll\t"));
            if (menus) CHECK(title == L"\n\nTO START\nMod settings have moved: Pause > MODS");
            if (pages)
                CHECK(has(hud, "NEW MOD: OFFLINEMOD") && has(hud, "Its settings are under Pause ") &&
                      !has(hud, "YOUR MODS ARE READY"));
            // Developer=1 (platform 2.9.0): "fh2mod Platform ui"'s tests queue the platform's own pop-ups and tip, with no
            // game; a pop-up from the command counts as a reply, since the platform had just called it
            const std::wstring pid = L"--pid " + std::to_wstring(GetCurrentProcessId()) + L" ";
            std::string out;
            CHECK(run_cli(pid + L"Platform ui toast Snapshot written | WreckingBall-defs.txt, next to the game | photo reply", out) == 0 &&
                  out == "queued: it shows once you're driving\n");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui toast \\\"Two words\\\" \\\"and a line\\\" boards", out) == 0);
            out.clear();
            CHECK(run_cli(pid + L"Platform ui tip Press {button} to try it | X", out) == 0 && has(out, "queued: it shows after 5 s of driving"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui toast Bad | icon | sparkles", out) == 1 && has(out, "error: \"sparkles\" isn't an icon"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui demo", out) == 0 &&
                  has(out, "demo: 4 of 4 pop-ups queued (urgent first, then the reply, then the others), the tip queued"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "Demo: urgent / 3 of 5: a fault, first in the queue (key Demo3)") &&
                  has(out, "key Demo4") && !has(out, "the log has its key only") && has(out, "Platform: a tip waiting"));
            std::printf("---- fh2mod Platform ui\n%s----\n", out.c_str());
            const std::string after = slurp(L"fh2-mods\\platform.log");
            CHECK(has(after, "[Platform] reply pop-up queued: Snapshot written / WreckingBall-defs.txt, next to the game (key Test)"));
            CHECK(has(after, "[Platform] pop-up replaced while waiting: Two words / and a line (key Test)"));
            CHECK(has(after, "[Platform] urgent pop-up queued: Demo: urgent / 3 of 5: a fault, first in the queue (key Demo3)") &&
                  has(after, "[Platform] pop-up queued: key Demo4") && !has(after, "4 of 5: the log has its key only"));
            // "ui set" (Developer=1) on a mod's Pop-ups row, as the player would turn it off on the mod's page: AaaMod
            // declared pop-ups here (aaa_popups.txt), OfflineMod didn't. platform-popups.ini gets [mods] beside the test's
            // [popups], and [player] as it is.
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set aaamod off", out) == 0 && out == "AaaMod's Pop-ups: Off, and kept in fh2-mods\\platform-popups.ini\n");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set OfflineMod off", out) == 1 && has(out, "error: OfflineMod has no Pop-ups row"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set Nobody on", out) == 1 && has(out, "error: \"Nobody\" isn't ModPopups, Tips or an installed mod's name"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set Tips maybe", out) == 1 && out == "error: ui set ModPopups|Tips|<mod> on|off\n");
            std::string ini;
            for (int i = 0; i < 50 && !has(ini = slurp(L"fh2-mods\\platform-popups.ini"), "aaamod.dll=0"); ++i) Sleep(100);
            CHECK(has(ini, "[popups]\r\nDeveloper=1\r\n") && has(ini, "[mods]\r\naaamod.dll=0\r\n") && has(ini, "ModPopups=1") && has(ini, "Tips=1"));
            CHECK(has(slurp(L"fh2-mods\\platform.log"), "[AaaMod] the player turned its pop-ups off on its page"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "aaamod.dll's pop-ups: off\n"));
            fs::remove("fh2-mods\\platform-popups.ini");
            fs::remove("aaa_popups.txt");
        } else {
            // home: YOUR MODS has been opened, so no line on the title screen. OfflineMod is still told as new, though
            // platform-mods.txt has listed it since the launch before: its notice hasn't played yet.
            CHECK(title.find(L"Mod settings") == std::wstring::npos && !fs::exists(L"fh2-mods\\platform-title.txt"));
            CHECK(!has(log, "[MODS] title screen:"));
            CHECK(has(seen, "OfflineMod.dll\tmod.new\t"));
            if (pages) CHECK(has(hud, "NEW MOD: OFFLINEMOD") && !has(hud, "YOUR MODS ARE READY"));
            // the UI templates' player controls and developer tools (platform 2.9.0), from fh2-mods\platform-popups.ini:
            // Developer=1 starts the command pipe (no mod here has commands) and the pop-up hooks (no mod here declared
            // pop-ups); the player's choices from last time hold: mod pop-ups and tips are off.
            CHECK(has(log, "Developer=1 (fh2-mods\\platform-popups.ini): the UI templates test page is on the FH2 Mod Platform's page under YOUR MODS"));
            CHECK(has(log, "commands: other programs on this PC (this Windows user's) can run mods' commands through "));
            CHECK(has(log, "no mod declared pop-ups or tips, but Developer=1 (fh2-mods\\platform-popups.ini) has the hooks go in for the UI templates' tests") &&
                  !has(log, "pop-ups and tips are off this session"));
            CHECK(has(log, "[Pop-ups] the player turned mod pop-ups off (Mod pop-ups, in fh2-mods\\platform-popups.ini)") &&
                  has(log, "[Pop-ups] the player turned tips off (Tips, in fh2-mods\\platform-popups.ini)"));
            const std::wstring pid = L"--pid " + std::to_wstring(GetCurrentProcessId()) + L" ";
            std::string out;
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "the player turned mod pop-ups off\n") && has(out, "the player turned tips off\n") &&
                  has(out, "mod pop-ups off, tips off\n") && has(out, "Developer=1: the UI templates test page and the test commands are on\n"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui toast Snapshot written | WreckingBall-defs.txt, next to the game | photo reply", out) == 1 &&
                  has(out, "error: not queued; fh2-mods\\platform.log says why"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui tip \\\"Press {button} to try it\\\" A", out) == 1 && has(out, "error: not queued"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui keyboard", out) == 0 && out == "buttons in tips are written for the keyboard now\n");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "buttons in tips are written for the keyboard\n"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui auto", out) == 0 && out == "buttons in tips follow the device used last again\n");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui demo", out) == 0 && has(out, "demo: 0 of 4 pop-ups queued") && has(out, "the tip not queued"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui toast", out) == 1 && has(out, "error: ui toast Title | Line | [icon] [urgent] [reply] [private]"));
            log = slurp(L"fh2-mods\\platform.log");
            CHECK(has(log, "[Platform] pop-up not shown: the player turned mod pop-ups off (SNAPSHOT WRITTEN / WreckingBall-defs.txt, next to the game") ||
                  has(log, "[Platform] pop-up not shown: the player turned mod pop-ups off (Snapshot written / WreckingBall-defs.txt, next to the game"));
            CHECK(has(log, "[Platform] a tip not shown: the player turned tips off; it isn't marked seen"));
            CHECK(has(log, "[Platform] buttons in tips are written for the keyboard from now on, whatever was used last"));
            // the platform's page has the UI templates page with Developer=1, a row per template
            if (pages) {
                const auto installed = slurp(layer() / L"UI\\Scenes\\Anthem\\A_ModPage_01.xaml");
                CHECK(has(installed, "Heading=\"UI templates\"") && has(installed, "Content=\"UI TEMPLATES\"") &&
                      has(installed, "local:BindingProxy.A=\"Everything in turn\"") && has(installed, "local:BindingProxy.A=\"Message box\""));
                std::string why;
                const bool ok = well_formed(installed, why);
                std::printf("%s A_ModPage_01.xaml is well-formed with the UI templates page%s%s\n", ok ? "ok  " : "FAIL", ok ? "" : ": ", why.c_str());
                if (!ok) ++fails;
            }
            // "ui set" (Developer=1) on the platform's own rows, as the player would set them, with no platform-popups.ini:
            // the platform writes a new one, its comments first, then the player's choices
            fs::remove("fh2-mods\\platform-popups.ini");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set Tips on", out) == 0 && out == "Tips: On, and kept in fh2-mods\\platform-popups.ini\n");
            out.clear();
            CHECK(run_cli(pid + L"Platform ui set modpopups on", out) == 0 &&
                  out == "Mod pop-ups: Same as Notifications, and kept in fh2-mods\\platform-popups.ini\n");
            std::string ini;
            for (int i = 0; i < 50 && !has(ini = slurp(L"fh2-mods\\platform-popups.ini"), "ModPopups=1"); ++i) Sleep(100);
            CHECK(ini.rfind("; FH2 Mod Platform: pop-ups and tips (platform 2.9.0).", 0) == 0 && has(ini, "; Developer=1 adds the UI templates test page") &&
                  has(ini, "ModPopups=1") && has(ini, "Tips=1") && !has(ini, "ModPopups=0") && !has(ini, "Tips=0"));
            CHECK(ini.find("ModPopups=1") > ini.find("\n[player]\r\n") && ini.find("ModPopups=1") < ini.find("\n[mods]\r\n") &&
                  ini.find("\n[mods]\r\n") != std::string::npos);   // in [player], not after the comments' own "[player] and [mods]"
            log = slurp(L"fh2-mods\\platform.log");
            CHECK(has(log, "[Pop-ups] the player turned tips on") && has(log, "[Pop-ups] the player set Mod pop-ups to Same as Notifications"));
            out.clear();
            CHECK(run_cli(pid + L"Platform ui", out) == 0 && has(out, "mod pop-ups same as Notifications, tips on\n"));
            fs::remove("fh2-mods\\platform-popups.ini");   // the phases after this one run without it
        }
    } else {
        // TestMod gone: its tables and notice are cleaned up, HelloMod's stays. The title screen keeps only the
        // platform's line about where mod settings are (YOUR MODS hasn't been opened), in each language's words.
        CHECK(str("LoadingMessages.str", 0x6875) == L"Fast Traveling to Destination (hello from HelloMod!)");
        CHECK(str("AIPlayers.str", 0) == L"<none>");
        if (menus)
            CHECK(str("Main.str", 0x18F0) == L"\n\nTO START\nMod settings: Pause > MODS (the last tab)" &&
                  str_in(L"PL", "Main.str", 0x18F0) == L"\n\nTO START\nUstawienia modów: Pauza > MODS (ostatnia karta)");
        else
            CHECK(str("Main.str", 0x18F0).find(L"Mod settings") == std::wstring::npos);
        CHECK(str("LoadingMessages.str", 0x5507) != L"A loading tip from TestMod" &&
              str_in(L"PL", "LoadingMessages.str", 0x5507) != L"Wskaz\u00f3wka z TestMod");
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") == std::string::npos);
        const fs::path ov = layer();
        CHECK(!fs::exists(ov / "UI" / "keep.xml") && !fs::exists(ov / "UI" / "handmade.xml"));
        CHECK(!fs::exists(ov / "UI" / "Scenes" / "Anthem" / "A_Splash.xaml"));   // no title entries left
        CHECK(slurp(ov / "drivatarglobalcache" / "DefaultCache.bin") == "from AaaMod");
        CHECK(has(log, "removed the media\\UI\\keep.xml override (TestMod no longer replaces it)"));
        CHECK(!fs::exists(ov / "Stripped" / "gamedbRC.slt"));
        CHECK(has(log, "[AaaMod] TestMod version (none)"));
        // no mod here has commands, so the pipe doesn't start: the platform's own "ui" doesn't start it (platform 2.9.0)
        CHECK(has(log, "[Platform] command: ui (") && !has(log, "commands: other programs on this PC"));
        CHECK(!has(log, "[AaaMod] UI templates:") && has(log, "pop-ups and tips are off this session"));
        // the page crash guard's note about TestMod's page goes with it, and with nothing left, so does the file
        CHECK(has(log, "pages: TestMod.dll isn't in fh2-mods any more, so the note about TestMod.dll's page Crashy is dropped") &&
              !fs::exists("fh2-mods\\platform-pages.txt"));
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), fails); // don't wait on the mods' threads
    return fails;
}

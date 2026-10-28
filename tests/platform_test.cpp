// Offline test of the platform, run as "azimuth.exe" under Wine by
// tests/run_platform_test.sh, which sets up the folder:
//   azimuth.exe d3dcompiler_47.dll fh2-mods\{AaaMod,HelloMod,TestMod,Legacy}.dll
//   FH2\Mount\media\Stripped\StringTables\EN.zip, FH2\Mount\media\Stripped\gamedbRC.slt
//   FH2\Mount\mediaoverride\UI\handmade.xml (an override placed by hand)
//   azimuth.exe <phase> [fh2.exe]
// phase "full": everything installed; "removed": TestMod.dll deleted;
// "crash": AaaMod crashes on purpose; then, with an FH2MP.dll, OfflineMod.dll and FH2MP's
// session file: "online-first", "online", "online-chosen" (the player's choices) and
// "fh2mp-off" (FH2MP turned off on the MODS tab).
#include "strings.hpp"
#include "updates.hpp"

#include <windows.h>

#include "sqlite3.h"

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
    if (!fs::exists("FH2\\Mount\\mediaoverride\\Stripped\\gamedbRC.slt")) return -1;
    sqlite3* db = nullptr;
    sqlite3_stmt* st = nullptr;
    int xp = -1;
    if (sqlite3_open_v2("FH2\\Mount\\mediaoverride\\Stripped\\gamedbRC.slt", &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(db, ("SELECT XP FROM AIPlayers WHERE Id = " + std::to_string(id)).c_str(), -1, &st, nullptr) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        xp = sqlite3_column_int(st, 0);
    if (st) sqlite3_finalize(st);
    sqlite3_close(db);
    return xp;
}

static bool has(const std::string& log, const char* s) { return log.find(s) != std::string::npos; }

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
    if (phase == L"full") {   // a saves folder, for the snapshot TestMod asks for (platform 2.8.0)
        fs::create_directories(L"data\\titles\\1A96545B\\saves\\1A96545B");
        std::ofstream{"data\\titles\\1A96545B\\saves\\1A96545B\\test.sav", std::ios::binary} << "save data";
    }
    HMODULE p = LoadLibraryW(L"d3dcompiler_47.dll"); // what azimuth.exe does at startup
    CHECK(p != nullptr);
    if (argc > 2) map_image(argv[2]); // "press Play"
    // the online phases (platform 2.8.0): run after "removed", with OfflineMod and an FH2MP.dll
    const bool online_phase = phase.rfind(L"online", 0) == 0 || phase == L"fh2mp-off";
    std::string log;
    for (int i = 0; i < 250; ++i) {
        Sleep(100);
        log = slurp(L"fh2-mods\\platform.log");
        if (online_phase ? has(log, "mod(s) loaded")
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
        CHECK(has(log, "FH2 Mod Platform 2.8.0 (API 9)"));
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
        CHECK(title == L"\n\n\n\nTO START\nTest notice line\nsecond line\nOld FH2 Mod Platform: reinstall TestMod");
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") != std::string::npos);
        if (argc > 2) CHECK(log.find("the game is mapped at") != std::string::npos);

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

        // Pages (API 8)
        CHECK(has(log, "[TestMod] page: Test stats, on the Achievements tile"));
        CHECK(has(log, "[TestMod] add_page: there's no tile called Nope; Other is only under MODS > Installed mods"));
        CHECK(has(log, "[TestMod] add_page: there's already a page called Other"));
        CHECK(has(log, "[TestMod] set_page_rows: there's no page called Missing"));
        CHECK(has(log, "[TestMod] set_page_xaml: Stats's content has no element named FirstItem"));
        CHECK(has(log, "[TestMod] page slots ok"));
        CHECK(has(log, "[TestMod] set_page_xaml: Stats's content isn't well-formed (</Grid> doesn't match <Button>)"));
        CHECK(has(log, "[TestMod] set_page_rows: row 1 of Stats isn't the size of the first"));
        // title-screen entries (API 9)
        CHECK(has(log, "[TestMod] title-screen entry: (Y) DRIVE THE TEST GARAGE") &&
              has(log, "[TestMod] title-screen entry: (X) SECOND ENTRY (a list of 2)"));
        CHECK(has(log, "[TestMod] add_title_entry: Second can have 1 to 100 choices, not 101"));
        CHECK(has(log, "[TestMod] add_title_entry: Second's choice 1 has no label"));
        CHECK(has(log, "[TestMod] add_title_entry: Second has choices but no on_choose"));
        CHECK(has(log, "[TestMod] add_title_entry: there's already an entry called Garage"));
        CHECK(has(log, "[TestMod] add_title_entry: Y is TestMod's Garage; Third gets RB"));
        CHECK(has(log, "[TestMod] add_title_entry: Third's label is cut to \"Third entry with a label that is\""));
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
            CHECK(has(log, "[TestMod] add_command: \"bad name\" isn't a usable name (letters, digits, _)") &&
                  has(log, "[TestMod] add_command: there's already a command called Echo"));
            CHECK(has(slurp("fh2-mods\\platform-session.json"), "\"pipe\": \"\\\\\\\\.\\\\pipe\\\\fh2mod-"));
        }
        // hotkeys (API 9): bindings named, rows on the Mods page, the refusals
        CHECK(has(log, "[TestMod] hotkey Mark (Mark this spot): F9, controller LB+RB+Y"));
        CHECK(has(log, "[TestMod] hotkey Toggle (Toggle the test): Ctrl+Shift+M, controller Off"));
        CHECK(has(log, "[TestMod] add_hotkey: there's already a hotkey called Mark") &&
              has(log, "[TestMod] add_hotkey: Broken needs a label and an on_press"));
        CHECK(has(log, "[TestMod] Mods page row: Mark this spot: key") && has(log, "[TestMod] Mods page row: Toggle the test: controller"));
        // sessions (API 9): the id in the log, the log's copy, our own log, the status file
        {
            const std::size_t at = log.find("[TestMod] session id ");
            std::string id       = at == std::string::npos ? "" : log.substr(at + 21, log.find('\n', at) - at - 21);
            while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
            CHECK(id.size() >= 17 && id[8] == '-' && id[15] == '-' && id.substr(16) == std::to_string(GetCurrentProcessId()));
            CHECK(has(log, ("session " + id + ": started ").c_str()));
            const std::string copy = slurp(fs::path(L"fh2-mods\\platform-logs") / ("platform-" + id + ".log"));
            CHECK(has(copy, "FH2 Mod Platform 2.8.0 (API 9)") && has(copy, "[TestMod] session id "));
            CHECK(slurp(fs::path(L"TestModData\\logs") / (id + ".log")) == "test mod's own log\n");
            CHECK(has(log, ("[TestMod] session log: TestModData\\logs\\" + id + ".log (the newest 3 kept)").c_str()));
            CHECK(has(log, "[TestMod] bad log folder refused") &&
                  has(log, "[TestMod] session_log_path: \"..\\Elsewhere\" isn't a folder name"));
            const std::string status = slurp("fh2-mods\\platform-session.json");
            // save snapshots (API 9): one copy, and the saves left as they were
            CHECK(has(log, "[TestMod] asked for a copy of the saves before the game reads them: the platform test") &&
                  has(log, "[TestMod] save snapshot asked for"));
            CHECK(has(log, ("save snapshot: 1 file(s), 0 MB, copied to fh2-mods\\save-snapshots\\" + id +
                            " (the newest 2 kept), for TestMod (the platform test)").c_str()));
            CHECK(slurp(fs::path(L"fh2-mods\\save-snapshots") / id / L"1A96545B" / L"test.sav") == "save data");
            CHECK(slurp(L"data\\titles\\1A96545B\\saves\\1A96545B\\test.sav") == "save data");
            CHECK(has(status, ("\"session\": \"" + id + "\"").c_str()) && has(status, "\"state\": \"running\"") &&
                  has(status, "\"platform\": \"2.8.0\"") && has(status, "\"TestMod\": \"1.0.0\""));
        }
        // The game's menus were given: the pages are written. (Not with the game mapped: this
        // test's copy of it doesn't pass the menus' code checks, so the platform takes them out.)
        if (fs::exists(L"FH2\\Mount\\media\\UI.zip") && argc <= 2) {
            const auto scenes    = ov / "UI" / "Scenes" / "Anthem";
            const auto installed = slurp(scenes / "A_ModPage_01.xaml");
            const auto tile      = slurp(scenes / "A_ModPage_80.xaml");
            CHECK(has(installed, "local:BindingProxy.A=\"Test stats &amp; more\"") && has(installed, "x:Name=\"PART_Button_FH2Mod_Page_0\""));
            CHECK(has(installed, "Content=\"TEST STATS\"") && has(installed, "{Binding FH2Mod_ViewIn_") && has(installed, "{Binding FH2Mod_Bar_3"));
            CHECK(has(installed, "The pause menu's Achievements tile opens it too."));
            CHECK(has(installed, "FH2Mod_InfoRow_Heading") && has(installed, "FH2Mod_InfoRow_Dim"));
            CHECK(has(installed, "Heading=\"Footer &lt;line&gt;\"") && has(installed, "local:BindingProxy.B=\"Alert\""));
            // the tile: the page of TestMod's own design, in the options page's frame
            CHECK(has(tile, "<Grid x:Name=\"TestContent\">") && !has(tile, "BasicOptions") && has(tile, "x:Key=\"FH2Mod_ShowIf\""));
            CHECK(has(tile, "<Color x:Key=\"Test_Color\">") && has(tile, "<Grid>\r\n<ContentControl x:Name=\"DefaultHelpPanel\" Style="));
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
            // the title screen: the entries' lines under PRESS (A) TO START, fading with it in all five states
            const auto splash = slurp(scenes / "A_Splash.xaml");
            std::size_t refs = 0;
            for (std::size_t at = splash.find("\"FH2Mod_TitleEntries\""); at != std::string::npos; at = splash.find("\"FH2Mod_TitleEntries\"", at + 1)) ++refs;
            CHECK(refs == 6);
            CHECK(has(splash, "{Binding FH2Mod_TitleReady}") && has(splash, "<Setter Property=\"Visibility\" Value=\"Collapsed\" />"));
            CHECK(has(splash, "A_BitmapImage_ControllerButton_Y}") && has(splash, "Text=\"DRIVE THE TEST GARAGE\""));
            CHECK(has(splash, "A_BitmapImage_ControllerButton_LB}") && has(splash, "Text=\"FOURTH &amp; &lt;LAST&gt;\""));
            // the choice row on Settings > Mods: a slider shown like the game's option rows, a name per value
            const auto mods = slurp(scenes / "A_ModOptions.xaml");
            CHECK(has(mods, "Style=\"{StaticResource FH2Mod_Choice_") && has(mods, "<Setter Property=\"Maximum\" Value=\"2\" />"));
            CHECK(has(mods, "Text=\"LB\"") && has(mods, "Text=\"A &amp; &lt;B&gt;\" Style=\"{StaticResource A_Style_SliderControlText}\" "
                                                         "HorizontalAlignment=\"Center\" Visibility=\"Hidden\""));
            CHECK(has(mods, "<Trigger Property=\"Value\" Value=\"2\">") && has(mods, "A_Style_Arrow_Left") &&
                  has(mods, "<Setter TargetName=\"RightArrow\" Property=\"Visibility\" Value=\"Hidden\" />"));
            CHECK(!has(installed, "Load when playing online"));   // no FH2MP here: no such row
        }
    } else if (online_phase) {
        const auto known = slurp("fh2-mods\\platform-mods.txt");
        const bool pages = fs::exists(L"FH2\\Mount\\media\\UI.zip") && argc <= 2;
        const auto installed = pages ? slurp(L"FH2\\Mount\\mediaoverride\\UI\\Scenes\\Anthem\\A_ModPage_01.xaml") : std::string();
        if (phase == L"online-first") {
            // OfflineMod is new: it loads, register_mod turns it away, and it's remembered as offline-only
            CHECK(has(log, "started from the FH2MP launcher (online play)"));
            CHECK(has(log, "loaded OfflineMod.dll") &&
                  has(log, "[OfflineMod] 1.0.0 doesn't run in online play (its author says so), so it isn't started"));
            CHECK(!has(log, "OfflineMod.dll: fh2mod_init returned") && !has(log, "[OfflineMod] running"));
            CHECK(has(known, "OfflineMod.dll\tOfflineMod\t1.0.0\tMade for offline play.\t\t1"));
            if (pages) {
                CHECK(has(installed, "local:BindingProxy.C=\"Off online  1.0.0\""));
                // each mod's page is a view of Installed mods: OfflineMod's has the switch, FH2MP's doesn't
                CHECK(has(installed, "Heading=\"Load when playing online\"") && has(installed, "Its author made it for offline play"));
                CHECK(!has(installed, "the game doesn't load FH2MP"));
                CHECK(!has(slurp(L"FH2\\Mount\\mediaoverride\\UI\\Scenes\\Anthem\\A_Hud.xaml"), "OfflineMod"));   // not an error
            }
        } else if (phase == L"online") {
            // ... and from then on isn't loaded at all online
            CHECK(has(log, "OfflineMod.dll is set to stay off when playing online; not loaded") && !has(log, "loaded OfflineMod.dll"));
            CHECK(has(log, "loaded HelloMod.dll") && has(log, "FH2MP.dll has no fh2mod_init"));
            if (pages) CHECK(has(installed, "local:BindingProxy.C=\"Off online  1.0.0\""));
        } else if (phase == L"online-chosen") {
            // the player's choices (platform-online.txt) win over the author's
            CHECK(has(log, "[OfflineMod] running online"));
            CHECK(has(log, "HelloMod.dll is set to stay off when playing online; not loaded"));
        } else {
            // fh2mp-off: FH2MP turned off on the MODS tab, so a session file doesn't make it online
            CHECK(!has(log, "started from the FH2MP launcher") && has(log, "FH2MP.dll is turned off on the MODS tab"));
            CHECK(has(log, "[OfflineMod] running offline") && has(log, "loaded HelloMod.dll"));
        }
    } else {
        // TestMod gone: its tables and notice are cleaned up, HelloMod's stays.
        CHECK(str("LoadingMessages.str", 0x6875) == L"Fast Traveling to Destination (hello from HelloMod!)");
        CHECK(str("AIPlayers.str", 0) == L"<none>");
        CHECK(str("Main.str", 0x18F0) == L"<none>");
        CHECK(slurp(L"fh2-mods\\platform-tables.txt").find("AIPlayers.str") == std::string::npos);
        const fs::path ov = L"FH2\\Mount\\mediaoverride";
        CHECK(!fs::exists(ov / "UI" / "keep.xml") && !fs::exists(ov / "UI" / "handmade.xml"));
        CHECK(!fs::exists(ov / "UI" / "Scenes" / "Anthem" / "A_Splash.xaml"));   // no title entries left
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

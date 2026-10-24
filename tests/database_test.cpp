// Offline test of the game database next to other mods' databases, run under
// Wine by tests/run_platform_test.sh in the folder it sets up, which has
// d3dcompiler_47.dll, DbMod.dll (tests/db_mod.cpp) and the original
// FH2\Mount\media\Stripped\gamedbRC.slt in it.
//
// Each case gets an Azimuth folder of its own in db-test, with a copy of this
// exe as azimuth.exe: "azimuth.exe launch" loads the platform as Azimuth does
// and exits once the platform has done the database. Between launches the
// cases do what players, other mods and FH2MP do to
// mediaoverride\Stripped\gamedbRC.slt, and check what the game would read.
#include <windows.h>

#include "sqlite3.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

static std::string slurp(const fs::path& f) {
    std::ifstream in{f, std::ios::binary};
    return {std::istreambuf_iterator<char>(in), {}};
}
static bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
static bool present(const fs::path& f) {
    std::error_code ec;
    return fs::exists(f, ec);
}

// Size and date, as the platform sees them.
struct Stamp {
    uint64_t size = 0, time = 0;
    bool operator==(const Stamp&) const = default;
};
static Stamp stamp(const fs::path& f) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a)) return {};
    return {uint64_t{a.nFileSizeHigh} << 32 | a.nFileSizeLow,
            uint64_t{a.ftLastWriteTime.dwHighDateTime} << 32 | a.ftLastWriteTime.dwLowDateTime};
}
static bool same_bytes(const fs::path& a, const fs::path& b) { return present(a) && present(b) && slurp(a) == slurp(b); }
// The very file: the same bytes and the same date.
static bool same_file(const fs::path& a, const fs::path& b) { return same_bytes(a, b) && stamp(a) == stamp(b); }

static std::string line1(const fs::path& f) {
    const auto s = slurp(f);
    return s.substr(0, s.find_first_of("\r\n"));
}
// A platform-db-fh2mp.txt, and the line in it for a file's size and date.
static std::string listed(const fs::path& list) {
    std::string s = slurp(list);
    std::erase(s, '\r');
    return s;
}
static std::string entry(const fs::path& f) { return std::to_string(stamp(f).size) + " " + std::to_string(stamp(f).time) + "\n"; }

// Dates as Windows keeps them, in 100 ns ticks.
static constexpr uint64_t kSecond = 10000000;
static uint64_t ticks(long long unix_time) { return (unix_time + 11644473600ll) * kSecond; }
static uint64_t now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return uint64_t{ft.dwHighDateTime} << 32 | ft.dwLowDateTime;
}
static void set_time(const fs::path& f, uint64_t t) {
    const FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}
// A file's date, as a mod's zip keeps it (Unix time).
static void set_date(const fs::path& f, long long unix_time) { set_time(f, ticks(unix_time)); }

// The first column of the first row, or "".
static std::string q(const fs::path& db, const char* sql) {
    std::string out;
    sqlite3* d       = nullptr;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &d, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(d, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        if (const auto* t = sqlite3_column_text(st, 0)) out = reinterpret_cast<const char*>(t);
    if (st) sqlite3_finalize(st);
    sqlite3_close(d);
    return out;
}

static void edit(const fs::path& db, const std::string& sql) {
    sqlite3* d = nullptr;
    char* err  = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &d, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        sqlite3_exec(d, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::printf("FAIL couldn't edit %s: %s\n", db.string().c_str(), err ? err : sqlite3_errmsg(d));
        ++fails;
    }
    sqlite3_free(err);
    sqlite3_close(d);
}

// Which database it is: the name in its Tag table, "" for the game's own, or "missing".
static std::string what(const fs::path& db) { return present(db) ? q(db, "SELECT v FROM Tag") : "missing"; }
// NitroTest's change: every car can backfire.
static bool backfire(const fs::path& db) {
    return q(db, "SELECT count(*) FROM BackfireLevels") == "16" &&
           q(db, "SELECT count(*) FROM BackfireLevels WHERE BackfireLevel <> 3") == "0";
}
// CountTest's change adds 1 to this (46000 in the game's own).
static int xp(const fs::path& db) { return std::atoi(q(db, "SELECT XP FROM AIPlayers WHERE Id = 1").c_str()); }
static int traffic_cars(const fs::path& db) {
    return std::atoi(q(db, "SELECT count(*) FROM TrafficCars WHERE CarId > 900000").c_str());
}
// application_id "FH2D": built by the platform.
static bool marked(const fs::path& db) {
    std::ifstream in{db, std::ios::binary};
    char h[72];
    return in.read(h, sizeof h) && std::string(h + 68, 4) == "FH2D";
}

// The game holds the database open while it runs.
static HANDLE hold(const fs::path& f) {
    return CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
}

static fs::path g_self;
static const fs::path kFiles = "db-test\\files";
static fs::path g_stock, g_traffic, g_traffic2, g_server, g_server2, g_server3, g_server_cars, g_carpack, g_other, g_fh2ml;

// Another mod's database: the game's own with a table naming it, and `sql`, dated like a file from a zip.
static fs::path make(const char* file, const char* name, const std::string& sql, long long date) {
    const fs::path f = kFiles / file;
    CopyFileW(g_stock.c_str(), f.c_str(), FALSE);
    edit(f, std::string("CREATE TABLE Tag (v TEXT); INSERT INTO Tag VALUES ('") + name + "');" + sql);
    set_date(f, date);
    return f;
}

struct Case {
    fs::path dir, T, M, S, B, P, H, F;
    std::string log; // platform.log of the last launch
    const int fails_before = fails;

    // Kept only when one of its checks failed: each holds several copies of the 6 MB database.
    ~Case() {
        std::error_code ec;
        if (fails == fails_before) fs::remove_all(dir, ec);
    }

    explicit Case(const char* name) : dir{fs::path{"db-test"} / name} {
        std::printf("\n---- %s\n", name);
        const fs::path mount = dir / "FH2" / "Mount";
        fs::create_directories(dir / "fh2-mods");
        fs::create_directories(mount / "media" / "Stripped" / "StringTables");
        fs::create_directories(mount / "mediaoverride" / "Stripped");
        CopyFileW(g_self.c_str(), (dir / "azimuth.exe").c_str(), FALSE);
        CopyFileW(L"d3dcompiler_47.dll", (dir / "d3dcompiler_47.dll").c_str(), FALSE);
        CopyFileW(g_stock.c_str(), (mount / "media" / "Stripped" / "gamedbRC.slt").c_str(), FALSE);
        T = mount / "mediaoverride" / "Stripped" / "gamedbRC.slt";
        M = mount / "mediapc" / "Stripped" / "gamedbRC.slt";
        S = dir / "fh2-mods" / "platform-db.txt";
        B = dir / "fh2-mods" / "platform-db-base.slt";
        P = dir / "fh2-mods" / "platform-db-base.previous.slt";
        H = dir / "fh2-mods" / "platform-db-fh2mp.txt";
        F = dir / "fh2mp" / "gamedbRC.slt";
    }

    // The mods in fh2-mods: DbMod.dll under their names.
    void mods(std::initializer_list<const char*> names) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir / "fh2-mods", ec))
            if (e.path().extension() == ".dll") fs::remove(e.path(), ec);
        for (const char* n : names) CopyFileW(L"DbMod.dll", (dir / "fh2-mods" / (std::string(n) + ".dll")).c_str(), FALSE);
    }

    // Puts a file in place as unzipping does: the same bytes and date.
    static void put(const fs::path& from, const fs::path& to) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
        CopyFileW(from.c_str(), to.c_str(), FALSE);
    }

    // Starts the game, and shows what the platform logged about the database.
    void launch() {
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"azimuth.exe launch";
        const auto exe = fs::absolute(dir / "azimuth.exe"), cwd = fs::absolute(dir);
        DWORD code = 1;
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
            if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        log = slurp(dir / "fh2-mods" / "platform.log");
        std::printf("  launch%s\n", code ? " FAILED" : "");
        if (code) ++fails;
        for (std::size_t at = 0; at < log.size();) {
            std::size_t end = log.find('\n', at);
            if (end == std::string::npos) end = log.size();
            std::string line = log.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (has(line, "database:") || has(line, "database override")) std::printf("    %s\n", line.c_str());
            at = end + 1;
        }
    }

    // Online with FH2MP: once the mods have started, it copies the server's
    // database over mediaoverride's (when they differ) and deletes
    // platform-db.txt.
    void online() {
        launch();
        std::printf("  FH2MP puts the server's database in place\n");
        if (!same_bytes(F, T)) put(F, T);
        DeleteFileW(S.c_str());
    }
};

// The report in issue #14: a traffic mod's database in mediaoverride, a mod
// that makes every car backfire, then that mod removed.
static void player() {
    Case c{"player"};
    Case::put(g_traffic, c.T);
    c.mods({"NitroTest"});
    c.launch();
    CHECK(what(c.T) == "traffic" && traffic_cars(c.T) == 7 && backfire(c.T) && marked(c.T));
    CHECK(same_file(c.B, g_traffic));
    CHECK(has(c.log, "database: mediaoverride\\Stripped\\gamedbRC.slt is another mod's; building on it"));
    const Stamp built = stamp(c.T), state = stamp(c.S);
    c.launch();
    CHECK(has(c.log, "database: same changes as last launch, reusing the copy"));
    CHECK(stamp(c.T) == built && stamp(c.S) == state && same_file(c.B, g_traffic));
    c.mods({});
    c.launch();
    CHECK(same_file(c.T, g_traffic) && !present(c.S));
    CHECK(has(c.log, "database: put back the gamedbRC.slt another mod had in mediaoverride (no mod changes the database any more)"));
    c.launch();
    CHECK(same_file(c.T, g_traffic) && !present(c.B) && !present(c.P));
    CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is the one the platform put back, so its copy"));
}

// Uninstalled as some mods' readmes say: the mod removed and
// mediaoverride\Stripped\gamedbRC.slt deleted.
static void uninstall() {
    {
        Case c{"uninstall"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        c.launch();
        c.mods({});
        DeleteFileW(c.T.c_str());
        c.launch();
        CHECK(same_file(c.T, g_traffic) && !present(c.S));
        CHECK(has(c.log, "database: put back the gamedbRC.slt another mod had in mediaoverride"));
    }
    {
        // After playing online, when FH2MP has deleted platform-db.txt.
        Case c{"uninstall-after-online"};
        Case::put(g_server, c.F);
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        c.launch();
        c.online();
        c.mods({});
        DeleteFileW(c.T.c_str());
        c.launch();
        CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic) && !present(c.P));
        CHECK(has(c.log, "database: put back the gamedbRC.slt another mod had in mediaoverride"));
    }
    {
        // With another mod whose changes all fail: as if no mod changed the database.
        Case c{"uninstall-with-failing-mod"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest", "BadSqlTest"});
        c.launch();
        c.mods({"BadSqlTest"});
        DeleteFileW(c.T.c_str());
        c.launch();
        CHECK(same_file(c.T, g_traffic) && !present(c.S) && !present(c.P));
    }
    {
        // The whole mediaoverride\Stripped folder deleted.
        Case c{"uninstall-folder"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        c.launch();
        c.mods({});
        std::error_code ec;
        fs::remove_all(c.T.parent_path(), ec);
        c.launch();
        CHECK(same_file(c.T, g_traffic) && !present(c.S));
    }
}

// A newer database from the other mod put over the platform's copy, then the
// last mod with SQL removed: the newer one stays, the older one is kept aside.
static void replaced() {
    Case c{"replaced"};
    Case::put(g_traffic, c.T);
    c.mods({"NitroTest"});
    c.launch();
    c.mods({});
    Case::put(g_traffic2, c.T);
    c.launch();
    CHECK(same_file(c.T, g_traffic2) && same_file(c.P, g_traffic) && !present(c.B) && !present(c.S));
    CHECK(has(c.log, "database: left mediaoverride\\Stripped\\gamedbRC.slt alone: it isn't the platform's"));
}

// The copy deleted while a mod still changes the database: built from the
// game's own, and the other mod's database it was built on kept aside.
static void deleted_with_sql() {
    Case c{"deleted-with-sql"};
    Case::put(g_traffic, c.T);
    c.mods({"NitroTest"});
    c.launch();
    DeleteFileW(c.T.c_str());
    c.launch();
    CHECK(what(c.T) == "" && backfire(c.T) && same_file(c.P, g_traffic) && !present(c.B));
    CHECK(has(c.log, "gamedbRC.slt was removed while a mod still changes the database, so the one another mod had there isn't built on"));
    c.mods({});
    c.launch();
    CHECK(!present(c.T) && !present(c.S));
}

// A plain copy of the game's database put over the platform's copy (the
// other mod uninstalled that way), or a car pack's: what the other mod had
// there isn't built on or put back any more.
static void game_copy() {
    {
        Case c{"game-copy"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        c.launch();
        Case::put(g_stock, c.T);
        c.launch();
        CHECK(what(c.T) == "" && backfire(c.T) && !present(c.B) && same_file(c.P, g_traffic));
        CHECK(has(c.log, "database: a plain copy of the game's gamedbRC.slt is in mediaoverride now, so the one another mod"));
        c.mods({});
        c.launch();
        CHECK(!present(c.T) && !present(c.S));
    }
    {
        Case c{"car-pack-copy"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        c.launch();
        c.mods({});
        Case::put(g_carpack, c.M);
        Case::put(g_carpack, c.T);
        c.launch();
        CHECK(same_file(c.T, g_carpack) && !present(c.B) && same_file(c.P, g_traffic) && !present(c.S));
    }
    {
        // A copy kept for FH2MP, with no mod changing the database.
        Case c{"game-copy-fh2mp"};
        Case::put(g_server, c.F);
        Case::put(g_traffic, c.T);
        c.launch();
        Case::put(g_stock, c.T);
        c.launch();
        CHECK(same_file(c.T, g_stock) && !present(c.B) && same_file(c.P, g_traffic));
    }
    {
        // Put over the platform's copy with nothing kept from another mod, then the
        // last mod removed: left alone (only 2.7.1's plain copy is removed).
        Case c{"game-copy-nothing-kept"};
        c.mods({"NitroTest"});
        c.launch();
        Case::put(g_stock, c.T);
        c.mods({});
        c.launch();
        CHECK(same_file(c.T, g_stock) && !present(c.S));
        CHECK(has(c.log, "database: left mediaoverride\\Stripped\\gamedbRC.slt alone: it isn't the platform's"));
    }
}

// Every mod's SQL fails: another mod's database is left as it is, and when
// the platform's copy is there, the database it was built on comes back.
static void all_fail() {
    Case c{"all-fail"};
    Case::put(g_traffic, c.T);
    c.mods({"BadSqlTest"});
    c.launch();
    CHECK(same_file(c.T, g_traffic) && !present(c.B) && !present(c.S));
    c.mods({"BadSqlTest", "NitroTest"});
    c.launch();
    CHECK(what(c.T) == "traffic" && backfire(c.T));
    c.mods({"BadSqlTest"});
    c.launch();
    CHECK(same_file(c.T, g_traffic));
    CHECK(has(c.log, "database: put back the gamedbRC.slt another mod had in mediaoverride (no mod's changes could be applied)"));
}

// A copy from 2.7.1, which kept no date for it: built again from the game's
// own, not on itself. Also one where SQLite wrote nothing, so it kept the
// date of the game's own.
static void from_2_7_1() {
    {
        Case c{"from-2.7.1"};
        c.mods({"NitroTest", "CountTest"});
        Case::put(g_stock, c.T);
        edit(c.T, "UPDATE BackfireLevels SET BackfireLevel = 3; UPDATE AIPlayers SET XP = XP + 1 WHERE Id = 1;");
        std::ofstream{c.S} << "0123456789abcdef\nNitroTest\t16 row(s) changed\nCountTest\t1 row(s) changed\n";
        c.launch();
        CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is from an older platform; building it again"));
        CHECK(backfire(c.T) && xp(c.T) == xp(g_stock) + 1 && marked(c.T) && !present(c.B) && !present(c.P));
        const auto l = line1(c.S);
        CHECK(std::count(l.begin(), l.end(), ' ') == 2);
    }
    {
        Case c{"from-2.7.1-no-rows"};
        c.mods({"NitroTest"});
        Case::put(g_stock, c.T);
        std::ofstream{c.S} << "0123456789abcdef\nNitroTest\t0 row(s) changed\n";
        c.launch();
        CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is a plain copy of the game's; building over it"));
        CHECK(backfire(c.T) && marked(c.T) && !present(c.B) && !present(c.P));
        c.mods({});
        c.launch();
        CHECK(!present(c.T) && !present(c.S));
    }
    {
        // The same with no mod changing the database at the first launch: removed
        // (the game's own has all of it), so a car pack's database in mediapc shows.
        Case c{"from-2.7.1-no-rows-removed"};
        Case::put(g_carpack, c.M);
        Case::put(g_stock, c.T);
        std::ofstream{c.S} << "0123456789abcdef\nNitroTest\t0 row(s) changed\n";
        c.launch();
        CHECK(!present(c.T) && !present(c.S) && same_file(c.M, g_carpack));
        CHECK(has(c.log, "database: removed mediaoverride's gamedbRC.slt, a plain copy of the game's from an older platform"));
    }
    {
        // Another mod's database put there after 2.7.1 last ran: dated long
        // before its platform-db.txt, so it isn't 2.7.1's copy.
        Case c{"from-2.7.1-then-traffic"};
        c.mods({"NitroTest"});
        std::ofstream{c.S} << "0123456789abcdef\nNitroTest\t16 row(s) changed\n";
        Case::put(g_traffic, c.T);
        c.launch();
        CHECK(has(c.log, "database: mediaoverride\\Stripped\\gamedbRC.slt is another mod's; building on it"));
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic));
    }
}

// FH2MP's copy of the server's database, which it leaves in mediaoverride
// after playing online: built over offline, and never kept, also once the
// server's database has changed.
static void fh2mp_copy() {
    Case c{"fh2mp-copy"};
    c.mods({"NitroTest"});
    Case::put(g_server, c.F);
    Case::put(c.F, c.T);
    c.launch();
    CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it"));
    CHECK(what(c.T) == "" && backfire(c.T) && !present(c.B) && !present(c.P));
    CHECK(has(slurp(c.H), (std::to_string(stamp(c.F).size) + " " + std::to_string(stamp(c.F).time)).c_str()));
    c.online();
    CHECK(same_file(c.T, g_server));
    Case::put(g_server2, c.F);
    c.launch();
    CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it"));
    CHECK(what(c.T) == "" && backfire(c.T) && !present(c.B) && !present(c.P));
    // The last 4 are listed, in fh2-mods and next to the copy. The first one
    // was found in mediaoverride after the second was listed, so the second
    // goes first.
    const auto line = [](const fs::path& f) { return std::to_string(stamp(f).size) + " " + std::to_string(stamp(f).time) + "\n"; };
    for (long long d : {1756684800ll, 1759276800ll, 1761955200ll}) {
        set_date(c.F, d);
        c.launch();
    }
    for (const auto& list : {c.H, c.T.parent_path() / "platform-db-fh2mp.txt"}) {
        std::string h = slurp(list);
        std::erase(h, '\r');
        CHECK(std::count(h.begin(), h.end(), '\n') == 4 && h.starts_with(line(g_server)) && !has(h, line(g_server2).c_str()) &&
              h.ends_with(line(c.F)));
    }
}

// FH2MP in a second Azimuth folder that plays on the same game folder, with
// no mod that changes the database: its copy of the server's database isn't
// taken for another mod's by the first folder's platform, which lists it in
// its own platform-db-fh2mp.txt too.
static void fh2mp_other_folder() {
    Case a{"fh2mp-other-folder"};
    Case a2{"fh2mp-other-folder-2"};
    std::error_code ec;
    fs::remove_all(a2.dir / "FH2", ec);
    fs::create_directories(a2.dir / "data", ec);
    std::ofstream{a2.dir / "data" / "azimuth.ini"} << "[title-1a96545b]\nfolder=" << fs::absolute(a.dir / "FH2").string() << "\n";
    a2.T = a.T;
    a2.mods({});
    Case::put(g_traffic, a.T);
    a.mods({"NitroTest"});
    a.launch();
    Case::put(g_server, a2.F);
    a2.online();
    a.launch();
    CHECK(what(a.T) == "traffic" && backfire(a.T) && same_file(a.B, g_traffic) && !present(a.P));
    CHECK(listed(a.H) == entry(g_server));
    Case::put(g_server2, a2.F);
    a2.online();
    a.launch();
    CHECK(what(a.T) == "traffic" && backfire(a.T) && same_file(a.B, g_traffic) && !present(a.P));
    a.mods({});
    a.launch();
    CHECK(same_file(a.T, g_traffic));
}

// A game folder on a drive that keeps dates to 2 s (FAT32) or 10 ms (exFAT),
// with the server's database on one that keeps them finely: FH2MP's copy is
// dated a little off the one listed. It's still FH2MP's, over two updates of
// the server's database.
static void fh2mp_coarse_dates() {
    for (const bool fat32 : {true, false}) {
        Case c{fat32 ? "fh2mp-coarse-dates" : "fh2mp-coarse-dates-exfat"};
        Case::put(g_traffic, c.T);
        c.mods({"NitroTest"});
        long long day = 1756684800;
        const auto download = [&](const fs::path& server) { // by FH2MP's launcher, dated to 100 ns
            Case::put(server, c.F);
            set_time(c.F, ticks(day += 86400) + 1234567);
        };
        const auto online = [&] { // FAT32 rounds the copy's date up to 2 s, exFAT down to 10 ms
            c.online();
            const uint64_t t = stamp(c.T).time;
            set_time(c.T, fat32 ? t - t % (2 * kSecond) + 2 * kSecond : t - t % (kSecond / 100));
        };
        download(g_server);
        c.launch();
        online();
        download(g_server2);
        c.launch();
        CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it"));
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic) && !present(c.P));
        online();
        download(g_server3);
        c.launch();
        CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it"));
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic) && !present(c.P));
    }
}

// Another mod's database the same size as the server's, put in mediaoverride
// 1.5 s before or after the server database's date on a drive that keeps
// dates to 100 ns (NTFS). That date isn't a whole 10 ms, as a FAT32 or exFAT
// drive's would be, so it isn't FH2MP's copy: it's built on, and put back.
static void fh2mp_near_date() {
    for (const long long ms : {1500ll, -1500ll}) {
        Case c{ms > 0 ? "fh2mp-near-date" : "fh2mp-near-date-before"};
        Case::put(g_traffic, c.T);
        set_time(c.T, now() / kSecond * kSecond + 1234567); // copied in without its date
        const Stamp traffic = stamp(c.T);
        Case::put(g_server_cars, c.F);
        set_time(c.F, static_cast<uint64_t>(static_cast<long long>(traffic.time) + ms * 10000));
        CHECK(stamp(c.F).size == traffic.size);
        c.mods({"NitroTest"});
        c.launch();
        CHECK(has(c.log, "database: mediaoverride\\Stripped\\gamedbRC.slt is another mod's; building on it"));
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_bytes(c.B, g_traffic) && stamp(c.B) == traffic);
        c.mods({});
        c.launch();
        CHECK(same_bytes(c.T, g_traffic) && stamp(c.T) == traffic);
    }
}

// FH2MP's copy in one list only (here mediaoverride's list was removed, with
// the game folder's mods, say): it's listed in the other again, by its own
// size and date. fh2mp_other_folder has it in mediaoverride's only.
static void fh2mp_one_list() {
    Case c{"fh2mp-one-list"};
    c.mods({"NitroTest"});
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.launch();
    c.online();
    const fs::path all = c.T.parent_path() / "platform-db-fh2mp.txt";
    DeleteFileW(all.c_str());
    Case::put(g_server2, c.F);
    c.launch();
    CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it"));
    CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic) && !present(c.P));
    CHECK(listed(all) == entry(g_server2) + entry(g_server) && listed(c.H) == entry(g_server2) + entry(g_server));
}

// FH2MP's copy with a date further off (a copy that didn't keep it), in a
// second Azimuth folder: found to be the same as the server's database there,
// so its own size and date are listed, and the first folder knows it too.
static void fh2mp_copy_dated_now() {
    Case a{"fh2mp-copy-dated-now"};
    Case a2{"fh2mp-copy-dated-now-2"};
    std::error_code ec;
    fs::remove_all(a2.dir / "FH2", ec);
    fs::create_directories(a2.dir / "data", ec);
    std::ofstream{a2.dir / "data" / "azimuth.ini"} << "[title-1a96545b]\nfolder=" << fs::absolute(a.dir / "FH2").string() << "\n";
    a2.T = a.T;
    a2.mods({});
    Case::put(g_traffic, a.T);
    a.mods({"NitroTest"});
    a.launch();
    Case::put(g_server, a2.F);
    a2.online();
    set_time(a.T, now());
    const Stamp copy = stamp(a.T);
    a2.launch();
    const auto lists = [&](const fs::path& f) {
        std::string h = slurp(f);
        std::erase(h, '\r');
        return has(h, (std::to_string(copy.size) + " " + std::to_string(copy.time) + "\n").c_str());
    };
    CHECK(lists(a2.H) && lists(a.T.parent_path() / "platform-db-fh2mp.txt"));
    a.launch();
    CHECK(what(a.T) == "traffic" && backfire(a.T) && same_file(a.B, g_traffic) && !present(a.P));
}

// Another mod's database installed after playing online.
static void fh2mp_new_database() {
    Case c{"fh2mp-then-traffic-2"};
    c.mods({"NitroTest"});
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.launch();
    c.online();
    Case::put(g_traffic2, c.T);
    c.launch();
    CHECK(what(c.T) == "traffic 2" && backfire(c.T) && same_file(c.B, g_traffic2) && same_file(c.P, g_traffic));
}

// The last mod with SQL removed, then online: the other mod's database is put
// back, FH2MP replaces it, and it's put back again offline.
static void fh2mp_after_release() {
    Case c{"fh2mp-after-release"};
    c.mods({"NitroTest"});
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.launch();
    c.mods({});
    c.online();
    CHECK(same_file(c.T, g_server) && same_file(c.B, g_traffic));
    c.launch();
    CHECK(same_file(c.T, g_traffic));
    CHECK(has(c.log, "database: put back the gamedbRC.slt another mod had in mediaoverride"));
    c.launch();
    CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic));
}

// No mod with SQL, and FH2MP installed: a copy of another mod's database is
// kept, to put back after playing online. Removed by the player after that,
// it stays removed.
static void fh2mp_keep() {
    Case c{"fh2mp-keep"};
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.launch();
    CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic) && !present(c.S));
    CHECK(has(c.log, "database: kept a copy of mediaoverride's gamedbRC.slt in case FH2MP replaces it online"));
    c.online();
    c.launch();
    CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic));
    DeleteFileW(c.T.c_str());
    c.launch();
    CHECK(!present(c.T) && !present(c.B) && same_file(c.P, g_traffic));
    c.launch();
    CHECK(!present(c.T));
}

// The same, and then a newer database from the other mod: the copy of the
// older one is kept aside, and one of the newer one kept.
static void fh2mp_keep_newer() {
    Case c{"fh2mp-keep-newer"};
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.launch();
    Case::put(g_traffic2, c.T);
    c.launch();
    CHECK(same_file(c.T, g_traffic2) && same_file(c.B, g_traffic2) && same_file(c.P, g_traffic));
}

// FH2MP copying the server's database in while the platform looks (a mod
// calling apply_database from its own thread): it can't be read yet, so
// nothing is done with it until FH2MP is done.
static void fh2mp_copying() {
    Case c{"fh2mp-copying"};
    Case::put(g_server, c.F);
    Case::put(g_traffic, c.T);
    c.mods({"NitroTest"});
    c.launch();
    c.mods({});
    const std::string state = slurp(c.S), server = slurp(c.F);
    // As CopyFileW does: the file open with no sharing, half written, dated now.
    HANDLE h = CreateFileW(c.T.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n  = 0;
    WriteFile(h, server.data(), static_cast<DWORD>(server.size() / 2), &n, nullptr);
    c.launch();
    CloseHandle(h);
    CHECK(has(c.log, "database: mediaoverride\\Stripped\\gamedbRC.slt is in use (error "));
    CHECK(slurp(c.S) == state && same_file(c.B, g_traffic) && !present(c.P));
    std::printf("  FH2MP is done\n");
    Case::put(c.F, c.T);
    DeleteFileW(c.S.c_str());
    c.launch();
    CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic));
}

// A car pack's database in mediapc, which the game reads before media's.
static void car_pack() {
    {
        Case c{"car-pack"};
        c.mods({"NitroTest"});
        Case::put(g_carpack, c.M);
        c.launch();
        CHECK(what(c.T) == "car pack" && backfire(c.T) && !present(c.B));
        CHECK(has(c.log, "database: building on mediapc\\Stripped\\gamedbRC.slt (a car pack's)"));
        c.mods({});
        c.launch();
        CHECK(!present(c.T) && same_file(c.M, g_carpack));
    }
    {
        // Installed after the copy was built: built again on it.
        Case c{"car-pack-added"};
        c.mods({"NitroTest"});
        c.launch();
        Case::put(g_carpack, c.M);
        c.launch();
        CHECK(what(c.T) == "car pack" && backfire(c.T));
    }
    {
        // One that ships a copy the platform built: built on, with a warning.
        Case c{"car-pack-marked"};
        c.mods({"CountTest"});
        Case::put(g_other, c.M);
        c.launch();
        CHECK(what(c.T) == "other install" && xp(c.T) == xp(g_stock) + 2);
        CHECK(has(c.log, "database: mediapc\\Stripped\\gamedbRC.slt has the platform's mark"));
    }
}

// A copy another install of the platform built (with the mark): built over
// from the game's own, and kept aside.
static void other_install() {
    {
        Case c{"other-install"};
        c.mods({"NitroTest", "CountTest"});
        Case::put(g_other, c.T);
        c.launch();
        CHECK(what(c.T) == "" && backfire(c.T) && xp(c.T) == xp(g_stock) + 1 && !present(c.B) && same_file(c.P, g_other));
        CHECK(has(c.log, "has the platform's mark, but isn't this install's copy; kept it as fh2-mods\\platform-db-base.previous.slt"));
    }
    {
        // Put there once no mod changes the database: kept aside, and the other mod's put back.
        Case c{"other-install-then-none"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        c.launch();
        c.mods({});
        Case::put(g_other, c.T);
        c.launch();
        CHECK(same_file(c.T, g_traffic) && same_file(c.P, g_other) && !present(c.S));
        CHECK(has(c.log, "database: kept mediaoverride's gamedbRC.slt (it has the platform's mark, but isn't this install's copy)"));
    }
}

// FH2ML's copy, built on another mod's database that FH2ML kept: built on that.
static void fh2ml() {
    {
        Case c{"fh2ml"};
        c.mods({"NitroTest"});
        Case::put(g_fh2ml, c.T);
        const fs::path ml = c.dir / "FH2ML" / "cache";
        Case::put(g_traffic, ml / "database-base.slt");
        std::ofstream{ml / "database.txt"} << "0123456789abcdef " << stamp(c.T).size << ' ' << stamp(c.T).time << "\nSomeMod\t1 row(s) changed\n";
        c.launch();
        CHECK(has(c.log, "database: mediaoverride's gamedbRC.slt is FH2ML's copy; building over it"));
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic) && !present(c.P));
        c.launch();
        CHECK(has(c.log, "database: same changes as last launch, reusing the copy"));
    }
    {
        // When the platform has kept one itself, that one is built on.
        Case c{"fh2ml-after-platform"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        c.launch();
        Case::put(g_fh2ml, c.T);
        const fs::path ml = c.dir / "FH2ML" / "cache";
        Case::put(g_traffic2, ml / "database-base.slt");
        std::ofstream{ml / "database.txt"} << "0123456789abcdef " << stamp(c.T).size << ' ' << stamp(c.T).time << "\nSomeMod\t1 row(s) changed\n";
        c.launch();
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic) && !present(c.P));
    }
}

// Files that can't be written: nothing is lost, and a later launch finishes the job.
static void failures() {
    {
        Case c{"cant-keep-a-copy"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        fs::create_directories(c.B); // a folder where the copy goes
        c.launch();
        CHECK(same_file(c.T, g_traffic) && !present(c.S));
        CHECK(has(c.log, "database: couldn't keep a copy of another mod's gamedbRC.slt (error "));
        fs::remove(c.B);
        c.launch();
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic));
    }
    // The first launch after 2.7.1, whose platform-db.txt names its copy by its
    // own date, with another mod's database installed just before (dated now).
    // platform-db.txt is put back with its date too, so the next launch doesn't
    // take that database for 2.7.1's copy, with the mod removed or still there.
    for (const bool still_on : {false, true}) {
        Case c{still_on ? "from-2.7.1-cant-keep-on" : "from-2.7.1-cant-keep"};
        c.mods({"NitroTest"});
        std::ofstream{c.S} << "0123456789abcdef\nNitroTest\t16 row(s) changed\n";
        set_time(c.S, now() - 2 * 86400 * kSecond);
        const std::string state = slurp(c.S);
        const Stamp state_date  = stamp(c.S);
        Case::put(g_traffic, c.T);
        set_time(c.T, now());
        const Stamp traffic = stamp(c.T);
        fs::create_directories(c.B); // a folder where the copy goes
        c.launch();
        CHECK(has(c.log, "database: couldn't keep a copy of another mod's gamedbRC.slt (error "));
        CHECK(slurp(c.S) == state && stamp(c.S) == state_date && same_bytes(c.T, g_traffic) && stamp(c.T) == traffic);
        fs::remove(c.B);
        if (still_on) {
            c.launch();
            CHECK(what(c.T) == "traffic" && backfire(c.T) && same_bytes(c.B, g_traffic) && stamp(c.B) == traffic);
        } else {
            c.mods({});
            c.launch();
            CHECK(same_bytes(c.T, g_traffic) && stamp(c.T) == traffic && !present(c.S));
        }
    }
    {
        Case c{"in-use"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        HANDLE h = hold(c.T);
        c.launch();
        CloseHandle(h);
        CHECK(same_file(c.T, g_traffic) && same_file(c.B, g_traffic) && line1(c.S).ends_with(" 0 0"));
        CHECK(has(c.log, "database: couldn't write "));
        c.launch();
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic));
        c.mods({});
        h = hold(c.T);
        c.launch();
        CloseHandle(h);
        CHECK(what(c.T) == "traffic" && backfire(c.T) && present(c.S) && same_file(c.B, g_traffic));
        CHECK(has(c.log, "database: couldn't put back the gamedbRC.slt another mod had in mediaoverride (error "));
        c.launch();
        CHECK(same_file(c.T, g_traffic));
    }
    {
        // The copy was never written, and the player removes the other mod's
        // database with the last mod: it isn't put back.
        Case c{"in-use-then-removed"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        HANDLE h = hold(c.T);
        c.launch();
        CloseHandle(h);
        c.mods({});
        DeleteFileW(c.T.c_str());
        c.launch();
        CHECK(!present(c.T) && !present(c.B) && same_file(c.P, g_traffic) && !present(c.S));
    }
    {
        Case c{"in-use-no-other-mod"};
        c.mods({"NitroTest"});
        c.launch();
        c.mods({});
        HANDLE h = hold(c.T);
        c.launch();
        CloseHandle(h);
        CHECK(backfire(c.T) && present(c.S) && has(c.log, "database: couldn't remove the game database override (error "));
        c.launch();
        CHECK(!present(c.T) && !present(c.S));
    }
    {
        // The other mod's database is put back, but platform-db-base.txt can't
        // be removed: platform-db.txt stays too, and the next launch finishes.
        Case c{"cant-remove-built"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        c.launch();
        c.mods({});
        const fs::path built = c.dir / "fh2-mods" / "platform-db-base.txt";
        HANDLE h = hold(built);
        c.launch();
        CloseHandle(h);
        CHECK(same_file(c.T, g_traffic) && present(c.S) && present(built));
        CHECK(has(c.log, "database: couldn't remove fh2-mods\\platform-db-base.txt (error "));
        c.launch();
        CHECK(same_file(c.T, g_traffic) && !present(c.S) && !present(built) && !present(c.B) && !present(c.P));
    }
    {
        Case c{"cant-move-aside"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        c.launch();
        DeleteFileW(c.T.c_str());
        fs::create_directories(c.P);
        c.launch();
        CHECK(!present(c.T) && same_file(c.B, g_traffic));
        CHECK(has(c.log, "database: couldn't move fh2-mods\\platform-db-base.slt aside (error "));
        fs::remove(c.P);
        c.launch();
        CHECK(what(c.T) == "" && backfire(c.T) && same_file(c.P, g_traffic) && !present(c.B));
    }
    {
        Case c{"read-only"};
        c.mods({"NitroTest"});
        Case::put(g_traffic, c.T);
        SetFileAttributesW(c.T.c_str(), FILE_ATTRIBUTE_READONLY);
        c.launch();
        CHECK(what(c.T) == "traffic" && backfire(c.T) && same_file(c.B, g_traffic));
        c.mods({});
        c.launch();
        CHECK(same_file(c.T, g_traffic));
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring{argv[1]} == L"launch") {
        // As azimuth.exe: the platform logs "N mod(s) loaded" once the database is done.
        if (!LoadLibraryW(L"d3dcompiler_47.dll")) return 2;
        for (int i = 0; i < 600; ++i) {
            Sleep(50);
            if (has(slurp(L"fh2-mods\\platform.log"), "mod(s) loaded")) TerminateProcess(GetCurrentProcess(), 0);
        }
        TerminateProcess(GetCurrentProcess(), 3);
        return 3;
    }
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_self = self;
    std::error_code ec;
    fs::remove_all("db-test", ec);
    fs::create_directories(kFiles, ec);
    // The game's own, dated as shipped.
    g_stock = kFiles / "stock.slt";
    if (!CopyFileW(L"FH2\\Mount\\media\\Stripped\\gamedbRC.slt", g_stock.c_str(), FALSE)) {
        std::printf("FAIL no FH2\\Mount\\media\\Stripped\\gamedbRC.slt\n");
        return 1;
    }
    SetFileAttributesW(g_stock.c_str(), FILE_ATTRIBUTE_NORMAL);
    set_date(g_stock, 1723994054);
    const std::string cars = "INSERT INTO TrafficCars (CarId, IsLaunchChunkCar) VALUES (900001, 0), (900002, 0), (900003, 0), "
                             "(900004, 0), (900005, 0), (900006, 0), (900007, 0);";
    g_traffic  = make("traffic.slt", "traffic", cars, 1746403200);
    g_traffic2 = make("traffic2.slt", "traffic 2", cars + "INSERT INTO TrafficCars VALUES (900008, 0);", 1748736000);
    g_server   = make("server.slt", "server", "", 1751328000);
    g_server2  = make("server2.slt", "server 2", "", 1754006400);
    g_server3  = make("server3.slt", "server 3", "", 1756684800);
    g_carpack  = make("carpack.slt", "car pack", "", 1740787200);
    const std::string built = "UPDATE BackfireLevels SET BackfireLevel = 3; PRAGMA application_id = 1179136580;";
    g_other = make("other.slt", "other install", built + "UPDATE AIPlayers SET XP = XP + 1 WHERE Id = 1;", 1738368000);
    g_fh2ml = make("fh2ml.slt", "FH2ML build", built, 1738454400);
    // A server's database with the traffic mod's cars: the same size as its database.
    g_server_cars = make("server-cars.slt", "server cars", cars, 1759276800);

    player();
    uninstall();
    replaced();
    deleted_with_sql();
    game_copy();
    all_fail();
    from_2_7_1();
    fh2mp_copy();
    fh2mp_other_folder();
    fh2mp_coarse_dates();
    fh2mp_near_date();
    fh2mp_one_list();
    fh2mp_copy_dated_now();
    fh2mp_new_database();
    fh2mp_after_release();
    fh2mp_keep();
    fh2mp_keep_newer();
    fh2mp_copying();
    car_pack();
    other_install();
    fh2ml();
    failures();

    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    return fails;
}

// Game database patches. media\Stripped\gamedbRC.slt is a SQLite database;
// mods queue SQL with add_sql, and the platform runs every mod's statements
// (in load order, each mod in its own transaction) against a copy written
// to mediaoverride\Stripped\gamedbRC.slt, which Extra Options' mount makes
// the game read instead.
//
// SQLite is built in (third_party/sqlite). Up to 2.7.0 the platform used
// Windows' own copy, System32\winsqlite3.dll, which Wine and Proton don't
// have, so Linux players got no database changes at all.
//
// The copy is rebuilt only when its inputs change: fh2-mods\platform-db.txt
// keeps a hash of the original database's size and date and every mod's
// statements.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include "sqlite3.h"

#include <cstdio>
#include <fstream>
#include <mutex>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::mutex g_db_mu;

fs::path state_file() { return azimuth_dir() / "fh2-mods" / "platform-db.txt"; }

// Line 1: the hash. Then "mod<TAB>result" for each mod, replayed into the
// log when the copy is reused.
std::vector<std::string> read_state_lines() {
    std::vector<std::string> out;
    std::ifstream in{state_file()};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out.push_back(line);
    }
    return out;
}
std::string read_state() {
    const auto l = read_state_lines();
    return l.empty() ? std::string{} : l[0];
}

void write_state(const std::string& s, const std::vector<std::string>& results = {}) {
    if (s.empty()) {
        DeleteFileW(state_file().c_str());
        return;
    }
    std::ofstream out{state_file(), std::ios::trunc};
    out << s << "\n";
    for (const auto& r : results) out << r << "\n";
}

struct Fnv {
    uint64_t h = 0xcbf29ce484222325ull;
    void add(const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 0x100000001b3ull;
    }
    void add(const std::string& s) { add(s.data(), s.size() + 1); }
};

} // namespace

void apply_database(const std::vector<FH2Mod*>& mods) {
    std::scoped_lock lk{g_db_mu};
    if (game_mount().empty()) return;
    const fs::path source = game_mount() / L"media" / L"Stripped" / L"gamedbRC.slt";
    const fs::path target = game_mount() / L"mediaoverride" / L"Stripped" / L"gamedbRC.slt";

    std::vector<FH2Mod*> with_sql;
    for (auto* m : mods)
        if (!m->sql.empty()) with_sql.push_back(m);

    if (with_sql.empty()) {
        if (!read_state().empty()) {
            SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
                plog("removed the game database override (no mod changes the database any more)");
            write_state({});
        }
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA src{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &src)) {
        plog("database: couldn't find " + narrow(source.wstring()) + "; no database changes this time");
        return;
    }
    Fnv f;
    f.add(&src.nFileSizeHigh, sizeof src.nFileSizeHigh);
    f.add(&src.nFileSizeLow, sizeof src.nFileSizeLow);
    f.add(&src.ftLastWriteTime, sizeof src.ftLastWriteTime);
    for (auto* m : with_sql) {
        f.add(m->name);
        for (const auto& s : m->sql) f.add(s);
    }
    char sig[17];
    std::snprintf(sig, sizeof sig, "%016llx", static_cast<unsigned long long>(f.h));
    std::error_code ec;
    if (const auto st = read_state_lines(); !st.empty() && st[0] == sig && fs::exists(target, ec)) {
        plog("database: same changes as last launch, reusing the copy");
        for (std::size_t i = 1; i < st.size(); ++i)
            if (const auto tab = st[i].find('\t'); tab != std::string::npos)
                log_line(st[i].substr(0, tab), "database: " + st[i].substr(tab + 1));
        return;
    }

    fs::create_directories(target.parent_path(), ec);
    auto tmp = target;
    tmp += L".platform-tmp";
    if (!CopyFileW(source.c_str(), tmp.c_str(), FALSE)) {
        plog("database: couldn't copy the game database (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(narrow(tmp.wstring()).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        plog(std::string("database: couldn't open the copy: ") + (db ? sqlite3_errmsg(db) : "out of memory"));
        if (db) sqlite3_close(db);
        DeleteFileW(tmp.c_str());
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=MEMORY", nullptr, nullptr, nullptr);
    int applied = 0;
    std::vector<std::string> results;
    for (auto* m : with_sql) {
        char* err      = nullptr;
        const int before = sqlite3_total_changes(db);
        int rc         = sqlite3_exec(db, "BEGIN", nullptr, nullptr, &err);
        for (std::size_t i = 0; rc == SQLITE_OK && i < m->sql.size(); ++i) rc = sqlite3_exec(db, m->sql[i].c_str(), nullptr, nullptr, &err);
        if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err);
        if (rc == SQLITE_OK) {
            ++applied;
            results.push_back(m->name + "\t" + std::to_string(sqlite3_total_changes(db) - before) + " row(s) changed");
        } else {
            std::string why = err ? err : sqlite3_errmsg(db);
            for (auto& c : why)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            results.push_back(m->name + "\tFAILED, so none of its changes were made: " + why);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        {
            const auto& r = results.back();
            log_line(r.substr(0, r.find('\t')), "database: " + r.substr(r.find('\t') + 1));
        }
        if (err) sqlite3_free(err);
    }
    sqlite3_close(db);

    if (!applied) {
        DeleteFileW(tmp.c_str());
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(target.c_str());
        write_state({});
        plog("database: no changes could be applied; the game uses its own database");
        return;
    }
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        plog("database: couldn't write " + narrow(target.wstring()) + " (error " + std::to_string(GetLastError()) +
             ")");
        DeleteFileW(tmp.c_str());
        return;
    }
    write_state(sig, results);
    plog("database: wrote the game database with changes from " + std::to_string(applied) + " mod(s)");
}

} // namespace fh2platform

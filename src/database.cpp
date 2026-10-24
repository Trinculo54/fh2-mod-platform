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
// keeps a hash of the database it's built on and every mod's statements,
// and the copy's size and date.
//
// Other mods put a gamedbRC.slt in mediaoverride too (traffic cars, for
// one), and FH2MP puts the server's there for online play. Up to 2.7.1 the
// platform built over such a file from media's, and deleted it once no mod
// had SQL, so it was lost. Now every copy the platform writes is marked
// (application_id "FH2D"), and platform-db.txt has its size and date, so
// the platform knows its own. Another mod's file is built on, and a copy is
// kept as fh2-mods\platform-db-base.slt and put back once no mod changes the
// database, or when the platform's copy is deleted along with the last mod
// that does. A car pack's database in mediapc is built on rather than
// media's. A file the platform replaces that nothing can make again is kept
// as fh2-mods\platform-db-base.previous.slt, and so is the kept copy once a
// plain copy of the game's database (or a car pack's) is put in its place.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include "sqlite3.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::mutex g_db_mu;

// application_id of every copy the platform writes ("FH2D"; FH2ML marks its
// copies the same way). The game's own SQLite (3.6.19) never reads it.
constexpr uint32_t kMarker = 0x46483244;

// 2.7.1 and older wrote platform-db.txt 4-7 ms after the copy, and kept no
// date: a copy dated from 10 s before the file to 2 s after is theirs.
constexpr uint64_t kOldBefore = 10 * 10'000'000ull, kOldAfter = 2 * 10'000'000ull; // 100 ns ticks
// FH2MP's copy of the server's database keeps its date, to 2 s on a game
// drive that keeps dates less finely (FAT32 keeps them to 2 s, exFAT to 10 ms).
// Such a drive's dates are whole 10 ms; NTFS keeps them to 100 ns.
constexpr uint64_t kNear = 2 * 10'000'000ull, kCoarse = 100'000ull;

struct Files {
    fs::path target;   // mediaoverride\Stripped\gamedbRC.slt, what the game reads
    fs::path media;    // media\Stripped\gamedbRC.slt, the game's own
    fs::path mediapc;  // mediapc\Stripped\gamedbRC.slt, a car pack's (read before media)
    fs::path state;    // fh2-mods\platform-db.txt
    fs::path base;     // fh2-mods\platform-db-base.slt, another mod's file, to put back
    fs::path built;    // fh2-mods\platform-db-base.txt, there while the copy is built on base
    fs::path previous; // fh2-mods\platform-db-base.previous.slt
    fs::path seen;     // fh2-mods\platform-db-fh2mp.txt
    fs::path seen_all; // mediaoverride\Stripped\platform-db-fh2mp.txt, the same for every Azimuth folder
    fs::path fh2mp;    // fh2mp\gamedbRC.slt, FH2MP's copy of the server's
    fs::path ml_state; // FH2ML\cache\database.txt and database-base.slt: FH2ML's own
    fs::path ml_base;
};

Files files() {
    const fs::path m = game_mount(), mods = azimuth_dir() / "fh2-mods", ml = azimuth_dir() / "FH2ML" / "cache";
    return {m / L"mediaoverride" / L"Stripped" / L"gamedbRC.slt",
            m / L"media" / L"Stripped" / L"gamedbRC.slt",
            m / L"mediapc" / L"Stripped" / L"gamedbRC.slt",
            mods / "platform-db.txt",
            mods / "platform-db-base.slt",
            mods / "platform-db-base.txt",
            mods / "platform-db-base.previous.slt",
            mods / "platform-db-fh2mp.txt",
            m / L"mediaoverride" / L"Stripped" / L"platform-db-fh2mp.txt",
            azimuth_dir() / "fh2mp" / "gamedbRC.slt",
            ml / "database.txt",
            ml / "database-base.slt"};
}

struct Stamp {
    uint64_t size = 0, time = 0; // time: last written, in 100 ns ticks
    bool operator==(const Stamp&) const = default;
};

// `t` is listed as `s`: the same size, and the same date, or one at most 2 s
// off when t's is whole 10 ms. A finer date has to be the same, so another
// mod's database the same size, put there a moment from the server's date on
// an NTFS drive, isn't taken for FH2MP's copy.
bool nearly(const Stamp& s, const Stamp& t) {
    return s.size == t.size &&
           (s.time == t.time || (t.time % kCoarse == 0 && (s.time > t.time ? s.time - t.time : t.time - s.time) <= kNear));
}

std::optional<Stamp> stamp_of(const fs::path& f) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return std::nullopt;
    return Stamp{uint64_t{a.nFileSizeHigh} << 32 | a.nFileSizeLow,
                 uint64_t{a.ftLastWriteTime.dwHighDateTime} << 32 | a.ftLastWriteTime.dwLowDateTime};
}
bool is_file(const fs::path& f) { return stamp_of(f).has_value(); }

// Same size and the same bytes.
bool same_bytes(const fs::path& a, const fs::path& b) {
    const auto sa = stamp_of(a), sb = stamp_of(b);
    if (!sa || !sb || sa->size != sb->size) return false;
    std::ifstream fa{a, std::ios::binary}, fb{b, std::ios::binary};
    std::vector<char> x(1 << 16), y(1 << 16);
    while (fa && fb) {
        fa.read(x.data(), static_cast<std::streamsize>(x.size()));
        fb.read(y.data(), static_cast<std::streamsize>(y.size()));
        if (fa.gcount() != fb.gcount() || !std::equal(x.begin(), x.begin() + fa.gcount(), y.begin())) return false;
    }
    return fa.eof() && fb.eof();
}

bool has_marker(const fs::path& f) {
    unsigned char h[72];
    std::ifstream in{f, std::ios::binary};
    if (!in.read(reinterpret_cast<char*>(h), sizeof h)) return false;
    return std::memcmp(h, "SQLite format 3", 16) == 0 &&
           (uint32_t{h[68]} << 24 | uint32_t{h[69]} << 16 | uint32_t{h[70]} << 8 | h[71]) == kMarker;
}

// The file operations below return 0, or the Windows error. A file that was
// just written is often open for a moment in another program (a virus
// scanner, say), so a busy file is tried again for up to a second.
bool busy(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION; }

DWORD move_over(const fs::path& from, const fs::path& to) {
    for (int i = 0;; ++i) {
        SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        const DWORD e = GetLastError();
        if (!busy(e) || i == 9) return e;
        Sleep(100);
    }
}

// A copy with the same date, put in place in one step (by way of a .platform-tmp
// file next to it), so `to` is never half written. Works across drives, and
// makes the folder if it was deleted.
DWORD copy_over(const fs::path& from, const fs::path& to) {
    auto tmp = to;
    tmp += L".platform-tmp";
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(from.c_str(), tmp.c_str(), FALSE)) return GetLastError();
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (const DWORD e = move_over(tmp, to)) {
        DeleteFileW(tmp.c_str());
        return e;
    }
    return 0;
}

DWORD remove_file(const fs::path& f) {
    for (int i = 0;; ++i) {
        SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(f.c_str())) return 0;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
        if (!busy(e) || i == 9) return e;
        Sleep(100);
    }
}

// 0 when `f` can be read or isn't there. A file another program is writing
// (FH2MP copying the server's database in, say) can't be read until it's done.
DWORD unreadable(const fs::path& f) {
    if (!is_file(f)) return 0;
    for (int i = 0;; ++i) {
        const HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return 0;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
        if (!busy(e) || i == 9) return e;
        Sleep(100);
    }
}

std::vector<std::string> read_lines(const fs::path& f) {
    std::vector<std::string> out;
    std::ifstream in{f};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        out.push_back(line);
    }
    return out;
}

bool to_u64(std::string_view s, uint64_t& v) {
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return !s.empty() && ec == std::errc{} && end == s.data() + s.size();
}

// platform-db.txt. Line 1: "<hash> <size> <date>", the copy's size and date
// when it was written, or "<hash> 0 0" while it's being written (that
// matches no file). 2.7.1 and older wrote the hash alone. Then
// "mod<TAB>result" for each mod, replayed into the log when the copy is
// reused. FH2MP reads the mods' names from it, and deletes it once it has put
// the server's database in place.
struct State {
    std::vector<std::string> lines;
    std::string sig;
    std::optional<Stamp> copy; // none: from 2.7.1 or older
    bool empty() const { return lines.empty() || lines[0].empty(); }
    bool old() const { return !empty() && !copy; }
    // Written once the copy was in place, not while it was being written.
    bool stamped() const { return copy && copy->time; }
    bool names(const Stamp& t) const { return stamped() && *copy == t; }
};

State read_state(const fs::path& f) {
    State s;
    s.lines = read_lines(f);
    if (s.empty()) return s;
    const std::string& l = s.lines[0];
    s.sig                = l;
    const auto a = l.find(' '), b = a == std::string::npos ? a : l.find(' ', a + 1);
    Stamp c;
    if (a == 0 || b == std::string::npos || l.find(' ', b + 1) != std::string::npos ||
        !to_u64(std::string_view{l}.substr(a + 1, b - a - 1), c.size) || !to_u64(std::string_view{l}.substr(b + 1), c.time))
        return s;
    s.sig  = l.substr(0, a);
    s.copy = c;
    return s;
}

bool write_state(const fs::path& f, const std::string& line1, const std::vector<std::string>& results) {
    std::ofstream out{f, std::ios::trunc};
    out << line1 << "\n";
    for (const auto& r : results) out << r << "\n";
    out.close();
    return !out.fail();
}

// platform-db.txt as it was, text and date, to put back when a save fails.
// The date matters: 2.7.1's platform-db.txt names its copy by being written
// just after it, so with today's date it would name another mod's database
// put in mediaoverride just now.
struct Saved {
    std::string text;
    FILETIME time{};
};

std::optional<Saved> read_raw(const fs::path& f) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    std::ifstream in{f, std::ios::binary};
    if (!in || !GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a)) return std::nullopt;
    return Saved{std::string{std::istreambuf_iterator<char>(in), {}}, a.ftLastWriteTime};
}

bool restore_raw(const fs::path& f, const std::optional<Saved>& was) {
    if (!was) return remove_file(f) == 0;
    {
        std::ofstream out{f, std::ios::binary | std::ios::trunc};
        out << was->text;
        out.close();
        if (out.fail()) return false;
    }
    const HANDLE h = CreateFileW(f.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const bool dated = SetFileTime(h, nullptr, nullptr, &was->time);
    CloseHandle(h);
    return dated;
}

// Logs a file that couldn't be removed.
void remove_or_log(const fs::path& f, const char* shown) {
    if (const DWORD e = remove_file(f)) plog(std::string("database: couldn't remove ") + shown + " (error " + std::to_string(e) + ")");
}

// platform-db-fh2mp.txt: "<size> <date>" of FH2MP's copy of the server's
// database at the last 4 launches it was new, newest last. FH2MP copies it
// over mediaoverride's with its date, so a copy of an older one is still
// FH2MP's, and nothing to keep. A date up to 2 s off counts when it's whole
// 10 ms (a FAT32 or exFAT game drive), and a copy found to be FH2MP's is
// listed by its own where a list doesn't have it. FH2MP doesn't delete this
// file. There's one in fh2-mods and one next to mediaoverride's
// gamedbRC.slt, which every Azimuth folder using this game folder lists its
// FH2MP's copies in: FH2MP in another Azimuth folder doesn't make the
// server's database look like another mod's.
std::vector<Stamp> read_seen(const fs::path& f) {
    std::vector<Stamp> out;
    for (const auto& l : read_lines(f)) {
        const auto sp = l.find(' ');
        Stamp s;
        if (sp != std::string::npos && to_u64(std::string_view{l}.substr(0, sp), s.size) &&
            to_u64(std::string_view{l}.substr(sp + 1), s.time))
            out.push_back(s);
    }
    return out;
}

void write_seen(const fs::path& f, std::vector<Stamp> seen) {
    if (seen.size() > 4) seen.erase(seen.begin(), seen.end() - 4);
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    std::ofstream out{f, std::ios::trunc};
    for (const auto& s : seen) out << s.size << ' ' << s.time << "\n";
    out.close();
    if (out.fail()) plog("database: couldn't write " + narrow(f.wstring()));
}

// Adds `s` to both lists, where it isn't there yet.
void list_fh2mp_copy(const Files& f, const Stamp& s) {
    for (const auto* list : {&f.seen, &f.seen_all}) {
        auto seen = read_seen(*list);
        if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
        seen.push_back(s);
        write_seen(*list, seen);
    }
}

void note_fh2mp_copy(const Files& f) {
    if (const auto s = stamp_of(f.fh2mp)) list_fh2mp_copy(f, *s);
}

// Whether mediaoverride's file, dated `t`, is FH2MP's copy: in a
// platform-db-fh2mp.txt, or the same as fh2mp's. It's then last in both
// lists, by its entry where a list has one, and by its own size and date where
// it hasn't (copied with another date, or listed by another Azimuth folder
// only), so it's still known once the server's database changes.
bool fh2mp_copy(const Files& f, const Stamp& t) {
    struct List {
        const fs::path& file;
        std::vector<Stamp> seen;
        std::vector<Stamp>::iterator match;
    };
    List lists[] = {{f.seen, read_seen(f.seen), {}}, {f.seen_all, read_seen(f.seen_all), {}}};
    bool listed = false;
    for (auto& l : lists) {
        l.match = std::find_if(l.seen.begin(), l.seen.end(), [&](const Stamp& s) { return nearly(s, t); });
        listed |= l.match != l.seen.end();
    }
    if (!listed && !same_bytes(f.target, f.fh2mp)) return false;
    for (auto& l : lists) {
        if (l.match == l.seen.end())
            l.seen.push_back(t);
        else if (l.match + 1 != l.seen.end())
            std::rotate(l.match, l.match + 1, l.seen.end());
        else
            continue;
        write_seen(l.file, l.seen);
    }
    return true;
}

// What's in mediaoverride\Stripped\gamedbRC.slt.
enum class Found {
    missing,
    ours,     // the copy platform-db.txt names
    ours_old, // a copy from 2.7.1 or older: built again, with the mark
    // Replaceable: never built on, never put back.
    fh2ml,    // FH2ML's copy (it keeps the file it was built on itself)
    fh2mp,    // FH2MP's copy of the server's database (FH2MP can copy it again)
    marked,   // another install's copy, or FH2ML's it doesn't name: kept in previous
    game,     // a plain copy of media's or mediapc's
    // Another mod's database: built on, kept in base.
    foreign,
};

Found classify(const Files& f, const State& st) {
    const auto t = stamp_of(f.target);
    if (!t) return Found::missing;
    if (st.names(*t)) return Found::ours;
    if (st.old())
        if (const auto s = stamp_of(f.state); s && t->time + kOldBefore >= s->time && t->time <= s->time + kOldAfter)
            return Found::ours_old;
    const bool marked = has_marker(f.target);
    if (marked && read_state(f.ml_state).names(*t)) return Found::fh2ml;
    if (fh2mp_copy(f, *t)) return Found::fh2mp;
    if (marked) return Found::marked;
    if (same_bytes(f.target, f.media) || same_bytes(f.target, f.mediapc)) return Found::game;
    return Found::foreign;
}

// No mod changes the database any more, or none of their changes could be
// made: the platform's copy goes, and what another mod had there comes back.
// If a step on the copy, the kept file or platform-db-base.txt fails,
// platform-db.txt stays, so the next launch tries again.
void release(const Files& f, const State& st, Found found, bool none_applied) {
    const std::string why = none_applied ? "no mod's changes could be applied" : "no mod changes the database any more";
    const bool have_base  = is_file(f.base);
    if (!st.empty() || have_base) {
        DWORD e = 0;
        std::string what;
        const auto put_back = [&] {
            what = "put back the gamedbRC.slt another mod had in mediaoverride";
            e    = copy_over(f.base, f.target);
            if (!e) plog("database: " + what + " (" + why + ")");
        };
        const auto move_aside = [&](const std::string& said) {
            what = "move fh2-mods\\platform-db-base.slt aside";
            e    = move_over(f.base, f.previous);
            if (!e) plog("database: " + said);
        };
        switch (found) {
        case Found::missing:
            // Removed by hand while the copy was built on it (some mods' uninstall
            // steps say to delete it), also after FH2MP replaced the copy online and
            // deleted platform-db.txt: put back. Removed once the platform had put it
            // back, or kept a copy for FH2MP: that mod is gone, so it isn't put back
            // again. A copy that was never written (the "0 0" line) doesn't count.
            if (have_base && (st.stamped() || st.old() || is_file(f.built))) {
                put_back();
            } else if (have_base) {
                move_aside("the gamedbRC.slt another mod had in mediaoverride was removed; the platform's copy of it is now "
                           "fh2-mods\\platform-db-base.previous.slt");
            } else {
                plog(none_applied ? "database: no changes could be applied; the game uses its own database"
                                  : "removed the game database override (no mod changes the database any more)");
            }
            break;
        case Found::ours:
        case Found::ours_old:
            if (have_base) {
                put_back();
            } else {
                what = "remove the game database override";
                e    = remove_file(f.target);
                if (!e)
                    plog(none_applied ? "database: no changes could be applied; the game uses its own database"
                                      : "removed the game database override (no mod changes the database any more)");
            }
            break;
        case Found::game:
            // Put over the platform's copy, or over the file it put back: the other mod
            // was uninstalled that way, or a car pack installed. (A copy from 2.7.1 that
            // no row changed in is a plain copy too; that one was built on the kept file.)
            if (have_base && !st.old()) {
                move_aside("a plain copy of the game's gamedbRC.slt is in mediaoverride now, so the one another mod had there "
                           "isn't put back; the platform's copy of it is now fh2-mods\\platform-db-base.previous.slt");
                break;
            }
            // That copy from 2.7.1 with nothing kept is removed: all of it is still in
            // media's or mediapc's, and a car pack's database in mediapc shows again.
            // Any other plain copy is left alone.
            if (!have_base && st.old()) {
                what = "remove the game database override";
                e    = remove_file(f.target);
                if (!e)
                    plog("database: removed mediaoverride's gamedbRC.slt, a plain copy of the game's from an older platform (" + why +
                         ")");
                break;
            }
            [[fallthrough]];
        case Found::fh2ml:
        case Found::fh2mp:
        case Found::marked:
            if (!have_base) {
                plog("database: left mediaoverride\\Stripped\\gamedbRC.slt alone: it isn't the platform's");
                break;
            }
            if (found == Found::marked) {
                what = "keep mediaoverride's gamedbRC.slt";
                e    = copy_over(f.target, f.previous);
                if (!e)
                    plog("database: kept mediaoverride's gamedbRC.slt (it has the platform's mark, but isn't this "
                         "install's copy) as fh2-mods\\platform-db-base.previous.slt");
            }
            if (!e) put_back();
            break;
        case Found::foreign:
            if (!st.empty()) plog("database: left mediaoverride\\Stripped\\gamedbRC.slt alone: it isn't the platform's");
            if (!have_base) break;
            if (same_bytes(f.base, f.target)) {
                // Put back earlier. Kept while FH2MP is installed (below).
                if (!is_file(f.fh2mp)) {
                    what = "remove fh2-mods\\platform-db-base.slt";
                    e    = remove_file(f.base);
                    if (!e)
                        plog("database: mediaoverride's gamedbRC.slt is the one the platform put back, so its copy "
                             "(fh2-mods\\platform-db-base.slt) was removed");
                }
            } else {
                move_aside("another mod's gamedbRC.slt is in mediaoverride now; the one the platform kept before is now "
                           "fh2-mods\\platform-db-base.previous.slt");
            }
            break;
        }
        if (!e) {
            what = "remove fh2-mods\\platform-db-base.txt";
            e    = remove_file(f.built);
        }
        if (e) {
            plog("database: couldn't " + what + " (error " + std::to_string(e) + "); trying again next launch");
            return;
        }
        if (!st.empty()) remove_or_log(f.state, "fh2-mods\\platform-db.txt");
    }
    // FH2MP copies the server's database over mediaoverride's when you play
    // online, and keeps no copy: keep one, to put back after.
    if (found == Found::foreign && is_file(f.fh2mp) && !same_bytes(f.base, f.target)) {
        DWORD e = remove_file(f.built);
        if (!e) e = copy_over(f.target, f.base);
        if (e)
            plog("database: couldn't keep a copy of mediaoverride's gamedbRC.slt (error " + std::to_string(e) + ")");
        else
            plog("database: kept a copy of mediaoverride's gamedbRC.slt in case FH2MP replaces it online");
    }
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
    const Files f = files();
    note_fh2mp_copy(f);

    std::vector<FH2Mod*> with_sql;
    for (auto* m : mods)
        if (!m->sql.empty()) with_sql.push_back(m);

    const State st = read_state(f.state);
    if (with_sql.empty() && st.empty() && !is_file(f.base) && !is_file(f.fh2mp)) return;
    // A file that can't be read yet is being written, and would look like
    // another mod's: it's looked at again next time.
    for (const auto& [file, shown] : {std::pair{&f.target, "mediaoverride\\Stripped\\gamedbRC.slt"},
                                      std::pair{&f.base, "fh2-mods\\platform-db-base.slt"}})
        if (const DWORD e = unreadable(*file)) {
            plog(std::string("database: ") + shown + " is in use (error " + std::to_string(e) + "); no database changes this time");
            return;
        }
    const auto was    = stamp_of(f.target);
    const Found found = classify(f, st);
    if (with_sql.empty()) {
        release(f, st, found, false);
        return;
    }

    // What to build on: another mod's file, the one kept from it, a car pack's, or the game's own.
    // The kept one isn't built on any more once the copy was removed while a mod still changes the
    // database, or a plain copy of the game's was put there (the other mod uninstalled, or a car
    // pack installed). It's moved aside below, once a mod's changes are made.
    const bool drop_base = is_file(f.base) && (found == Found::missing || (found == Found::game && !st.old()));
    fs::path base;
    std::string kind, shown;
    bool base_from_fh2ml = false;
    if (found == Found::foreign) {
        base = f.target, kind = "foreign", shown = "mediaoverride\\Stripped\\gamedbRC.slt";
    } else if (is_file(f.base) && !drop_base) {
        base = f.base, kind = "foreign", shown = "fh2-mods\\platform-db-base.slt";
    } else if (found == Found::fh2ml && is_file(f.ml_base)) {
        base = f.ml_base, kind = "foreign", shown = "FH2ML\\cache\\database-base.slt", base_from_fh2ml = true;
    } else if (is_file(f.mediapc)) {
        base = f.mediapc, kind = "mediapc", shown = "mediapc\\Stripped\\gamedbRC.slt";
    } else {
        base = f.media, kind = "media", shown = "media\\Stripped\\gamedbRC.slt";
    }
    const auto bs = stamp_of(base);
    if (!bs) {
        plog("database: couldn't find " + narrow(base.wstring()) + "; no database changes this time");
        return;
    }
    // "foreign" for mediaoverride's file and the copy kept from it alike (the copy has its date),
    // so keeping it doesn't mean building again.
    Fnv h;
    h.add(kind);
    const DWORD size_hi = static_cast<DWORD>(bs->size >> 32), size_lo = static_cast<DWORD>(bs->size);
    h.add(&size_hi, sizeof size_hi);
    h.add(&size_lo, sizeof size_lo);
    h.add(&bs->time, sizeof bs->time);
    for (auto* m : with_sql) {
        h.add(m->name);
        for (const auto& s : m->sql) h.add(s);
    }
    char sig[17];
    std::snprintf(sig, sizeof sig, "%016llx", static_cast<unsigned long long>(h.h));
    if (found == Found::ours && st.sig == sig) {
        plog("database: same changes as last launch, reusing the copy");
        for (std::size_t i = 1; i < st.lines.size(); ++i)
            if (const auto tab = st.lines[i].find('\t'); tab != std::string::npos)
                log_line(st.lines[i].substr(0, tab), "database: " + st.lines[i].substr(tab + 1));
        return;
    }

    switch (found) {
    case Found::ours_old: plog("database: mediaoverride's gamedbRC.slt is from an older platform; building it again"); break;
    case Found::fh2ml: plog("database: mediaoverride's gamedbRC.slt is FH2ML's copy; building over it"); break;
    case Found::fh2mp:
        plog("database: mediaoverride's gamedbRC.slt is FH2MP's online copy; building over it (FH2MP puts it back when you "
             "play online)");
        break;
    case Found::game: plog("database: mediaoverride's gamedbRC.slt is a plain copy of the game's; building over it"); break;
    default: break;
    }
    if (kind == "foreign" && found != Found::foreign)
        plog("database: building on " + shown + " (the gamedbRC.slt another mod had in mediaoverride)");
    else if (kind == "mediapc")
        plog("database: building on " + shown + " (a car pack's)");
    if (has_marker(base))
        plog("database: " + shown + " has the platform's mark (the platform or FH2ML built it), so mods' changes may be made twice");

    std::error_code ec;
    fs::create_directories(f.target.parent_path(), ec);
    auto tmp = f.target;
    tmp += L".platform-tmp";
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(base.c_str(), tmp.c_str(), FALSE)) {
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
    sqlite3_exec(db, ("PRAGMA application_id = " + std::to_string(kMarker)).c_str(), nullptr, nullptr, nullptr);
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
        release(f, st, found, true);
        return;
    }
    // mediaoverride's file was replaced while the copy was being built (FH2MP
    // putting the server's in place, say): what's there now is looked at
    // next time, and isn't kept as a file the copy wasn't built on.
    const auto replaced = [&] {
        if (stamp_of(f.target) == was) return false;
        DeleteFileW(tmp.c_str());
        plog("database: mediaoverride\\Stripped\\gamedbRC.slt changed while the copy was being built; no database changes this time");
        return true;
    };
    if (replaced()) return;

    // Keep what can't be made again before replacing it. platform-db.txt
    // first, with a date no file has: the kept file is never there without
    // it, and if the copy can't be written below, mediaoverride's file still
    // isn't the platform's next time.
    // (A file put back earlier still has its copy.)
    const bool kept      = found == Found::foreign && same_bytes(f.base, f.target) && stamp_of(f.base) == stamp_of(f.target);
    const bool keep_base = (found == Found::foreign && !kept) || base_from_fh2ml;
    if (keep_base || drop_base || found == Found::marked) {
        const auto before = read_raw(f.state);
        if (!before && is_file(f.state)) {
            DeleteFileW(tmp.c_str());
            plog("database: couldn't read fh2-mods\\platform-db.txt, so no database changes are made this time");
            return;
        }
        if (!write_state(f.state, std::string(sig) + " 0 0", results)) {
            DeleteFileW(tmp.c_str());
            if (!restore_raw(f.state, before)) plog("database: couldn't put fh2-mods\\platform-db.txt back as it was");
            plog("database: couldn't write fh2-mods\\platform-db.txt, so no database changes are made this time");
            return;
        }
        DWORD e = 0;
        if (found == Found::marked) {
            e = copy_over(f.target, f.previous);
            if (!e)
                plog("database: mediaoverride's gamedbRC.slt has the platform's mark, but isn't this install's copy; kept "
                     "it as fh2-mods\\platform-db-base.previous.slt and building over it");
        } else if (drop_base) {
            e = move_over(f.base, f.previous);
            if (!e)
                plog(std::string("database: ") +
                     (found == Found::missing ? "mediaoverride\\Stripped\\gamedbRC.slt was removed while a mod still changes the database"
                                              : "a plain copy of the game's gamedbRC.slt is in mediaoverride now") +
                     ", so the one another mod had there isn't built on any more; the platform's copy of it is now "
                     "fh2-mods\\platform-db-base.previous.slt");
        } else if (base_from_fh2ml) {
            e = copy_over(f.ml_base, f.base);
        } else {
            // The kept file is mediaoverride's own now, not one a copy was built on.
            e = remove_file(f.built);
            if (!e && is_file(f.base)) {
                e = move_over(f.base, f.previous);
                if (!e) plog("database: the gamedbRC.slt kept from another mod before is now fh2-mods\\platform-db-base.previous.slt");
            }
            if (!e) e = copy_over(f.target, f.base);
        }
        if (e) {
            DeleteFileW(tmp.c_str());
            if (!restore_raw(f.state, before)) plog("database: couldn't put fh2-mods\\platform-db.txt back as it was");
            plog(drop_base ? "database: couldn't move fh2-mods\\platform-db-base.slt aside (error " + std::to_string(e) +
                                 "), so no database changes are made this time"
                           : "database: couldn't keep a copy of another mod's gamedbRC.slt (error " + std::to_string(e) +
                                 "), so it stays as it is and no database changes are made this time");
            return;
        }
    }
    // Then as when the copy can't be written below.
    if (replaced()) return;
    if (found == Found::foreign)
        plog("database: mediaoverride\\Stripped\\gamedbRC.slt is another mod's; building on it (a copy is kept as "
             "fh2-mods\\platform-db-base.slt)");
    if (const DWORD e = move_over(tmp, f.target)) {
        plog("database: couldn't write " + narrow(f.target.wstring()) + " (error " + std::to_string(e) + ")");
        DeleteFileW(tmp.c_str());
        return;
    }
    const Stamp t           = stamp_of(f.target).value_or(Stamp{});
    const std::string stamp = std::to_string(t.size) + " " + std::to_string(t.time);
    // FH2MP deletes platform-db.txt, not this: once the copy is removed, what
    // it was built on still comes back (release).
    if (is_file(f.base)) {
        if (!write_state(f.built, stamp, {})) plog("database: couldn't write fh2-mods\\platform-db-base.txt");
    } else {
        remove_or_log(f.built, "fh2-mods\\platform-db-base.txt");
    }
    if (!write_state(f.state, std::string(sig) + " " + stamp, results))
        plog("database: couldn't write fh2-mods\\platform-db.txt, so the copy isn't known as the platform's and is built "
             "again next launch");
    plog("database: wrote the game database with changes from " + std::to_string(applied) + " mod(s)");
}

} // namespace fh2platform

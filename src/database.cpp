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
//
// Car packs (car_packs.cpp) have SQL too: theirs runs first, each pack in a
// transaction of its own, then the mods'. When the platform mounts the folders
// itself (no Extra Options), the database is this Azimuth folder's own, on the
// game's temp drive ahead of mediaoverride (apply_layer_database), and nothing
// is written to mediaoverride's file, which every Azimuth folder using the
// game folder reads. Not when FH2MP is installed in this Azimuth folder: it
// puts the server's database in mediaoverride for online play, which needs
// exactly that one, so the database is built there as before.
#include "car_ids.hpp"
#include "db_guards.hpp"
#include "platform.hpp"
#include "text.hpp"
#include "updates.hpp"

#include <windows.h>

#include "sqlite3.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>
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
    fs::path layer;       // this Azimuth folder's own, on the game's temp drive (car_packs.cpp)
    fs::path layer_state; // fh2-mods\platform-db-layer.txt, its record (as platform-db.txt)
    fs::path strings;       // fh2-mods\platform-db-strings.txt and platform-db-layer-strings.txt: the Data_Car.str
    fs::path layer_strings; // keys each copy's car names have (name_strings)
    fs::path car_ids;       // fh2-mods\platform-car-ids.txt: the Ids pack cars have (with_car_ids)
    fs::path car_ids_copy;  // and its copy next to this folder's database: Stripped\platform-car-ids.txt in the layer
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
            ml / "database-base.slt",
            instance_media() / L"Stripped" / L"gamedbRC.slt",
            mods / "platform-db-layer.txt",
            mods / "platform-db-strings.txt",
            mods / "platform-db-layer-strings.txt",
            mods / "platform-car-ids.txt",
            instance_media() / L"Stripped" / L"platform-car-ids.txt"};
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
// made, or the database is built on the temp drive now (`reason`): the
// platform's copy goes, and what another mod had there comes back. If a step
// on the copy, the kept file or platform-db-base.txt fails, platform-db.txt
// stays, so the next launch tries again.
void release(const Files& f, const State& st, Found found, bool none_applied, const char* reason = nullptr) {
    const std::string why = reason ? reason : none_applied ? "no mod's changes could be applied" : "no mod changes the database any more";
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
                                  : "removed the game database override (" + why + ")");
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
                                      : "removed the game database override (" + why + ")");
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

// Where SQL comes from: a car pack or a mod, by name, and its statements.
struct Sql {
    std::string name;
    const std::vector<std::string>* statements;
    const CarPack* pack = nullptr;
};

std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// What in a pack's or mod's SQL could clash with car packs' cars (car_ids::plan), for the MODS tab:
// pack -> its warnings, and mod -> its warnings, from this session's last build.
std::mutex g_warn_mu;
std::map<std::string, std::vector<std::string>> g_pack_warnings, g_mod_warnings;   // messages from later()

void clear_sql_warnings() {
    std::scoped_lock lk{g_warn_mu};
    g_pack_warnings.clear();
    g_mod_warnings.clear();
}

void note_sql_warning(const Sql& s, const std::string& why) {
    std::scoped_lock lk{g_warn_mu};
    (s.pack ? g_pack_warnings[s.pack->name] : g_mod_warnings[s.name]).push_back(why);
}

// The cars packs add (pack.ini) get their Ids here: a pack's SQL gives a car's Id as
// {car:<media name>}. Each car keeps the Id it was first given, since a bought car's Id is in the
// save: fh2-mods\platform-car-ids.txt records them ("<media name><TAB><id>" lines, never removed),
// and a new car gets the Id its pack.ini asks for (id=) when that one is free, else the lowest Id from
// 3000 up (above the game's and FM5's) that is and that no other new car asks for; below 9000 (for
// cars a mod adds by an Id of its own) either way. Free: its rows are, in the database being built
// on, in what the mods' SQL adds, and in the Ids packs write themselves (car_ids.cpp). A pack is left out, saying
// why, when that database already has one of its cars, another pack adds it too, or its recorded Id's
// rows are another source's now.
//
// The record is rewritten whole, in one step, each line with its line end (a hand edit that left the last line without
// one no longer has the next car's line joined to it), with a copy next to this folder's database, which a clean
// reinstall that empties fh2-mods keeps. A car the record doesn't have keeps the Id it had in the database the platform
// built last time (ids_from_last_build), so losing both doesn't turn a save's car into another one.

// "<media name><TAB><id>": false for any other line.
bool record_line(const std::string& l, std::string& car, int64_t& id) {
    const auto tab = l.find('\t');
    uint64_t n     = 0;
    if (tab == std::string::npos || !tab || !to_u64(std::string_view{l}.substr(tab + 1), n)) return false;
    car = l.substr(0, tab);
    id  = static_cast<int64_t>(n);
    return true;
}

struct Record {
    std::map<std::string, int64_t> ids;   // lower-case media name -> Id
    std::set<int64_t> used;
    std::vector<std::string> lines;      // platform-car-ids.txt's lines as they are, then the ones added this time
};

// Packs' cars the record doesn't have, with the Id they have in the last database the platform built: this folder's
// on the temp drive, else a copy in mediaoverride with the platform's mark. Only Ids from kFirstPackCar to below
// kFirstHandMade, and none the record gives another car.
void ids_from_last_build(const Files& f, const std::vector<Sql>& given, Record& rec) {
    struct Missing {
        std::string key, car, source;
    };
    std::vector<Missing> missing;
    for (const auto& s : given)
        if (s.pack)
            for (const auto& c : s.pack->cars)
                if (!rec.ids.count(lower_ascii(c))) missing.push_back({lower_ascii(c), c, s.name});
    for (const auto& [built, where] : {std::pair{&f.layer, "this folder's database on the temp drive"},
                                       std::pair{&f.target, "mediaoverride\\Stripped\\gamedbRC.slt"}}) {
        if (missing.empty() || !is_file(*built) || !has_marker(*built)) continue;
        sqlite3* db       = nullptr;
        sqlite3_stmt* q   = nullptr;
        if (sqlite3_open_v2(narrow(built->wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
            sqlite3_prepare_v2(db, "SELECT Id, MediaName FROM Data_Car WHERE Id >= ?1 AND Id < ?2", -1, &q, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, car_ids::kFirstPackCar);
            sqlite3_bind_int64(q, 2, car_ids::kFirstHandMade);
            while (sqlite3_step(q) == SQLITE_ROW) {
                const int64_t id = sqlite3_column_int64(q, 0);
                const auto* t    = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
                const auto m     = std::find_if(missing.begin(), missing.end(), [&](const Missing& x) { return t && x.key == lower_ascii(t); });
                if (m == missing.end() || rec.used.count(id)) continue;
                rec.ids[m->key] = id;
                rec.used.insert(id);
                rec.lines.push_back(m->car + "\t" + std::to_string(id));
                log_line(m->source, "database: " + m->car + " is car " + std::to_string(id) + " again, as in " + where +
                                        ", which the platform built last time (fh2-mods\\platform-car-ids.txt didn't have it)");
                missing.erase(m);
            }
        }
        sqlite3_finalize(q);
        sqlite3_close(db);
    }
}

// Writes `text` to `to` in one step, by way of a .platform-tmp file next to it.
bool write_whole(const fs::path& to, const std::string& text) {
    auto tmp = to;
    tmp += L".platform-tmp";
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out << text;
        out.close();
        if (out.fail()) return false;
    }
    if (move_over(tmp, to)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

//
// `keep_last` comes back true, with nothing changed, when the database built on couldn't be read (so every pack with
// cars would be left out, for a reason that can pass on its own) and `can_keep_last` says there's a copy from last
// time to keep instead, with their cars.
std::vector<Sql> with_car_ids(const Files& f, const fs::path& base, const std::string& shown, const std::vector<Sql>& given,
                              std::list<std::vector<std::string>>& held, bool can_keep_last, bool& keep_last) {
    keep_last = false;
    if (std::none_of(given.begin(), given.end(), [](const Sql& s) { return s.pack && !s.pack->cars.empty(); }))
        return given;
    Record rec;
    for (const auto& l : read_lines(f.car_ids)) {
        std::string car;
        int64_t id = 0;
        if (record_line(l, car, id) && !rec.ids.count(lower_ascii(car))) rec.ids[lower_ascii(car)] = id, rec.used.insert(id);
        if (!l.empty()) rec.lines.push_back(l);
    }
    std::size_t from_copy = 0;
    for (const auto& l : read_lines(f.car_ids_copy)) {
        std::string car;
        int64_t id = 0;
        if (!record_line(l, car, id) || rec.ids.count(lower_ascii(car)) || rec.used.count(id)) continue;
        rec.ids[lower_ascii(car)] = id;
        rec.used.insert(id);
        rec.lines.push_back(l);
        ++from_copy;
    }
    if (from_copy)
        plog("database: " + std::to_string(from_copy) + " car Id(s) fh2-mods\\platform-car-ids.txt didn't have were taken from "
             "its copy next to this folder's database");
    ids_from_last_build(f, given, rec);
    const std::map<std::string, int64_t>& record = rec.ids;
    std::vector<car_ids::Source> sources;
    for (const auto& s : given)
        sources.push_back({s.name, s.statements, s.pack ? &s.pack->cars : nullptr, s.pack ? &s.pack->wanted_ids : nullptr});
    const ULONGLONG started = GetTickCount64();
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(narrow(base.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        db = nullptr;
    }
    const std::vector<car_ids::Outcome> plan = car_ids::plan(db, shown, sources, record);
    sqlite3_close(db);
    plog("database: car packs' car Ids checked against " + shown + " and the other SQL in " +
         std::to_string(GetTickCount64() - started) + " ms");
    keep_last = can_keep_last && std::any_of(plan.begin(), plan.end(), [](const car_ids::Outcome& o) { return o.base_unread; });
    if (keep_last) return {};
    std::vector<Sql> out;
    for (std::size_t i = 0; i < given.size(); ++i) {
        const Sql& s = given[i];
        const car_ids::Outcome& o = plan[i];
        for (const auto& w : o.warnings) {
            log_line(s.name, "database: warning: " + english(w));
            note_sql_warning(s, w);
        }
        for (const auto& n : o.notes) log_line(s.name, "database: " + n);
        if (!s.pack || s.pack->cars.empty()) {
            out.push_back(s);
            continue;
        }
        if (!o.problem.empty()) {
            log_line(s.name, "database: left out, so none of its changes were made: " + english(o.problem));
            note_car_pack_problem(s.pack->name, o.problem);
            continue;
        }
        for (const auto& [car, id] : o.given) {
            rec.lines.push_back(car + "\t" + std::to_string(id));
            log_line(s.name, "database: " + car + " is car " + std::to_string(id) + " from now on");
        }
        const auto& cars = o.ids;
        place_car_ids(*s.pack, cars);
        auto& text = held.emplace_back();
        for (const auto& statement : *s.statements) {
            std::string t = statement;
            for (const auto& [car, id] : cars) {
                const std::string tag = "{car:" + lower_ascii(car) + "}", value = std::to_string(id);
                for (std::size_t p = 0; (p = lower_ascii(t).find(tag, p)) != std::string::npos; p += value.size())
                    t.replace(p, tag.size(), value);
            }
            text.push_back(std::move(t));
        }
        out.push_back({s.name, &text, s.pack});
    }
    std::string text;
    for (const auto& l : rec.lines) text += l + "\r\n";
    const auto now = [](const fs::path& p) {
        std::ifstream in{p, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>(in), {}};
    };
    if (now(f.car_ids) != text && !write_whole(f.car_ids, text)) plog("database: couldn't write fh2-mods\\platform-car-ids.txt");
    if (instance_layer() && now(f.car_ids_copy) != text && !write_whole(f.car_ids_copy, text))
        plog("database: couldn't write the copy of fh2-mods\\platform-car-ids.txt next to this folder's database");
    return out;
}

// What a copy is built from: what it's built on (its kind and date) and every source's statements.
std::string signature(const std::string& kind, const Stamp& base, const std::vector<Sql>& sources) {
    Fnv h;
    h.add("car names as keys"); // what a build does besides the SQL (name_strings): a copy made before is built again
    h.add(kind);
    const DWORD size_hi = static_cast<DWORD>(base.size >> 32), size_lo = static_cast<DWORD>(base.size);
    h.add(&size_hi, sizeof size_hi);
    h.add(&size_lo, sizeof size_lo);
    h.add(&base.time, sizeof base.time);
    for (const auto& s : sources) {
        h.add(s.name);
        for (const auto& t : *s.statements) h.add(t);
    }
    char sig[17];
    std::snprintf(sig, sizeof sig, "%016llx", static_cast<unsigned long long>(h.h));
    return sig;
}

// Data_Car.str keys for car names given as text, the first one; the game's own go up to 0xFEC3.
constexpr uint16_t kFirstNameKey = 0xFF00;
std::map<uint16_t, std::wstring> g_strings; // this launch's (database_strings)

// Car names a pack or mod gave as text (Data_Car.DisplayName / ModelShort): the game shows one
// that isn't a string-table reference ("_&<id>") in the Autoshow, but not in the top bar, so each
// becomes a reference to a new key in Data_Car.str (added when the string tables are written).
// The reference's high 16 bits name the table, as the game's own names' do.
std::map<uint16_t, std::wstring> name_strings(sqlite3* db) {
    std::map<uint16_t, std::wstring> out;
    sqlite3_stmt* q = nullptr;
    uint64_t table  = 0;
    if (sqlite3_prepare_v2(db, "SELECT DisplayName FROM Data_Car WHERE substr(DisplayName, 1, 2) = '_&' LIMIT 1", -1, &q,
                           nullptr) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW)
        table = std::strtoull(reinterpret_cast<const char*>(sqlite3_column_text(q, 0)) + 2, nullptr, 10) >> 16;
    sqlite3_finalize(q);
    if (!table) return out;
    struct Name {
        int64_t id;
        const char* column;
        std::string text;
    };
    std::vector<Name> names;
    if (sqlite3_prepare_v2(db, "SELECT Id, DisplayName, ModelShort FROM Data_Car ORDER BY Id", -1, &q, nullptr) == SQLITE_OK)
        while (sqlite3_step(q) == SQLITE_ROW)
            for (int c : {1, 2}) {
                const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(q, c));
                if (t && *t && std::strncmp(t, "_&", 2) != 0)
                    names.push_back({sqlite3_column_int64(q, 0), c == 1 ? "DisplayName" : "ModelShort", t});
            }
    sqlite3_finalize(q);
    uint32_t key = kFirstNameKey;
    for (const auto& n : names) {
        if (key >= 0xFFFF) {
            plog("database: more car names given as text than Data_Car.str has keys for; the rest stay text");
            break;
        }
        const std::string ref = "_&" + std::to_string(table << 16 | key);
        if (sqlite3_prepare_v2(db, (std::string("UPDATE Data_Car SET ") + n.column + " = ?1 WHERE Id = ?2").c_str(), -1, &q,
                               nullptr) != SQLITE_OK)
            continue;
        sqlite3_bind_text(q, 1, ref.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(q, 2, n.id);
        if (sqlite3_step(q) == SQLITE_DONE) {
            std::wstring w = widen(n.text);
            for (auto& c : w)
                if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            out[static_cast<uint16_t>(key++)] = std::move(w);
        }
        sqlite3_finalize(q);
    }
    if (!out.empty()) {
        char range[32];
        std::snprintf(range, sizeof range, "0x%04X-0x%04X", out.begin()->first, out.rbegin()->first);
        plog("database: " + std::to_string(out.size()) + " car name(s) given as text get Data_Car.str keys " + range);
    }
    return out;
}

// A copy's keys, kept next to its record ("key<TAB>text" lines, UTF-8), for a launch that reuses it.
void keep_strings(const fs::path& file, std::map<uint16_t, std::wstring> s) {
    if (s.empty()) {
        remove_or_log(file, "fh2-mods\\platform-db-strings.txt");
    } else {
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        for (const auto& [k, v] : s) out << k << '\t' << narrow(v) << "\n";
        out.close();
        if (out.fail()) plog("database: couldn't write " + narrow(file.wstring()));
    }
    g_strings = std::move(s);
}

void reuse_strings(const fs::path& file) {
    g_strings.clear();
    for (const auto& l : read_lines(file)) {
        const auto tab = l.find('\t');
        uint64_t k     = 0;
        if (tab != std::string::npos && to_u64(std::string_view{l}.substr(0, tab), k) && k < 0xFFFF)
            g_strings[static_cast<uint16_t>(k)] = widen(std::string_view{l}.substr(tab + 1));
    }
}

// Whether every Data_Car.str key a copy's car names point at (from kFirstNameKey, name_strings) is among this launch's
// keys (reuse_strings). The copy's signature doesn't cover its strings file: without this, a copy whose file was lost
// would be reused, and Data_Car.str written without its keys, so those cars' names would be blank. True when it can't
// be told.
bool names_kept(const fs::path& copy) {
    sqlite3* db     = nullptr;
    sqlite3_stmt* q = nullptr;
    bool kept       = true;
    if (sqlite3_open_v2(narrow(copy.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(db, "SELECT DisplayName, ModelShort FROM Data_Car", -1, &q, nullptr) == SQLITE_OK)
        while (kept && sqlite3_step(q) == SQLITE_ROW)
            for (int c : {0, 1})
                if (const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(q, c)); t && std::strncmp(t, "_&", 2) == 0) {
                    const uint64_t key = std::strtoull(t + 2, nullptr, 10) & 0xFFFF;
                    if (key >= kFirstNameKey && !g_strings.count(static_cast<uint16_t>(key))) kept = false;
                }
    sqlite3_finalize(q);
    sqlite3_close(db);
    return kept;
}

// A car pack's statements that failed (a result line, "car pack <name><TAB>FAILED, ..."): for the MODS tab.
void note_pack_result(const std::string& source, const std::string& result) {
    const std::string pack = "car pack ", failed = "FAILED, so none of its changes were made: ";
    if (source.rfind(pack, 0) == 0 && result.rfind(failed, 0) == 0)
        note_car_pack_problem(source.substr(pack.size()), later("Its database changes failed: {0}.", {result.substr(failed.size())}));
}

void replay(const State& st) {
    for (std::size_t i = 1; i < st.lines.size(); ++i)
        if (const auto tab = st.lines[i].find('\t'); tab != std::string::npos) {
            log_line(st.lines[i].substr(0, tab), "database: " + st.lines[i].substr(tab + 1));
            note_pack_result(st.lines[i].substr(0, tab), st.lines[i].substr(tab + 1));
        }
}

// Copies `base` to `tmp` (marked as the platform's) and runs every source's statements on it, each
// source in a transaction of its own, so one that fails changes nothing. `results` gets
// "name<TAB>result" for each, which is logged too. -> how many sources' changes were made, or -1
// when the copy couldn't be made or opened.
int build_copy(const fs::path& base, const fs::path& tmp, const std::vector<Sql>& sources, std::vector<std::string>& results,
               std::map<uint16_t, std::wstring>& strings) {
    std::error_code ec;
    fs::create_directories(tmp.parent_path(), ec);
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(base.c_str(), tmp.c_str(), FALSE)) {
        plog("database: couldn't copy the game database (error " + std::to_string(GetLastError()) + ")");
        return -1;
    }
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(narrow(tmp.wstring()).c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        plog(std::string("database: couldn't open the copy: ") + (db ? sqlite3_errmsg(db) : "out of memory"));
        if (db) sqlite3_close(db);
        DeleteFileW(tmp.c_str());
        return -1;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=MEMORY", nullptr, nullptr, nullptr);
    sqlite3_exec(db, ("PRAGMA application_id = " + std::to_string(kMarker)).c_str(), nullptr, nullptr, nullptr);
    int applied = 0;
    for (const auto& s : sources) {
        char* err        = nullptr;
        const int before = sqlite3_total_changes(db);
        int rc           = sqlite3_exec(db, "BEGIN", nullptr, nullptr, &err);
        for (std::size_t i = 0; rc == SQLITE_OK && i < s.statements->size(); ++i)
            rc = sqlite3_exec(db, (*s.statements)[i].c_str(), nullptr, nullptr, &err);
        if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT", nullptr, nullptr, &err);
        if (rc == SQLITE_OK) {
            ++applied;
            results.push_back(s.name + "\t" + std::to_string(sqlite3_total_changes(db) - before) + " row(s) changed");
        } else {
            std::string why = err ? err : sqlite3_errmsg(db);
            for (auto& c : why)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            results.push_back(s.name + "\tFAILED, so none of its changes were made: " + why);
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        }
        {
            const auto& r = results.back();
            log_line(r.substr(0, r.find('\t')), "database: " + r.substr(r.find('\t') + 1));
            note_pack_result(r.substr(0, r.find('\t')), r.substr(r.find('\t') + 1));
        }
        if (err) sqlite3_free(err);
    }
    if (applied) strings = name_strings(db);
    sqlite3_close(db);
    return applied;
}

// This Azimuth folder's own database (instance_media()\Stripped\gamedbRC.slt) and its record go.
void drop_layer_copy(const Files& f, const char* why) {
    if (is_file(f.layer)) {
        if (const DWORD e = remove_file(f.layer)) {
            plog("database: couldn't remove this folder's database on the temp drive (error " + std::to_string(e) + ")");
            return;
        }
        plog(std::string("database: removed this folder's database on the temp drive (") + why + ")");
    }
    remove_or_log(f.layer_state, "fh2-mods\\platform-db-layer.txt");
}

// The database on the game's temp drive (car_packs.cpp), which only this Azimuth folder's game
// reads, ahead of mediaoverride's: built on what the game would read without it, with every car
// pack's and mod's changes. mediaoverride's file isn't written, so the other Azimuth folders that
// use this game folder don't get them. A copy this folder put in mediaoverride before goes, and
// what another mod had there comes back, as when no mod has SQL. The layer's record is
// fh2-mods\platform-db-layer.txt, as platform-db.txt is mediaoverride's.
void apply_layer_database(const Files& f, const std::vector<Sql>& given) {
    reuse_strings(f.layer_strings); // what the copy there has, unless it's built again or goes
    if (const State st = read_state(f.state); !st.empty() || is_file(f.base)) {
        if (const DWORD e = unreadable(f.target)) {
            plog("database: mediaoverride\\Stripped\\gamedbRC.slt is in use (error " + std::to_string(e) +
                 "); no database changes this time");
            return;
        }
        release(f, st, classify(f, st), false, "this folder's database is on the game's temp drive now");
    }
    if (given.empty()) {
        drop_layer_copy(f, "no car pack or mod changes the database any more");
        keep_strings(f.layer_strings, {});
        return;
    }
    // Built on another mod's database in mediaoverride (traffic cars, say), or a plain copy of the
    // game's; never on another Azimuth folder's copy (it has that folder's mods' changes) or FH2MP's
    // copy of the server's (it's there for online play in an Azimuth folder that has FH2MP).
    const Found found = classify(f, read_state(f.state));
    fs::path base;
    std::string kind, shown;
    if (found == Found::foreign || found == Found::game) {
        base = f.target, kind = "mediaoverride", shown = "mediaoverride\\Stripped\\gamedbRC.slt";
    } else if (found == Found::fh2ml && is_file(f.ml_base)) {
        base = f.ml_base, kind = "fh2ml", shown = "FH2ML\\cache\\database-base.slt";
    } else if (is_file(f.mediapc)) {
        base = f.mediapc, kind = "mediapc", shown = "mediapc\\Stripped\\gamedbRC.slt";
    } else {
        base = f.media, kind = "media", shown = "media\\Stripped\\gamedbRC.slt";
    }
    if (const DWORD e = unreadable(base)) {
        plog("database: " + shown + " is in use (error " + std::to_string(e) + "); no database changes this time");
        return;
    }
    const auto bs = stamp_of(base);
    if (!bs) {
        plog("database: couldn't find " + narrow(base.wstring()) + "; no database changes this time");
        return;
    }
    std::list<std::vector<std::string>> held;
    const State last = read_state(f.layer_state);
    const auto at    = stamp_of(f.layer);
    bool keep_last   = false;
    const std::vector<Sql> sources = with_car_ids(f, base, shown, given, held, at && last.names(*at), keep_last);
    if (keep_last) {
        plog("database: car packs' cars couldn't be checked against " + shown + ", as it couldn't be read, so this folder's "
             "database from last time is kept as it is, with their cars (it's built again next launch)");
        replay(last);
        return;
    }
    if (sources.empty()) {
        drop_layer_copy(f, "no changes could be applied");
        keep_strings(f.layer_strings, {});
        return;
    }
    const std::string sig = signature(kind, *bs, sources);
    const State st        = read_state(f.layer_state);
    if (const auto t = stamp_of(f.layer); t && st.names(*t) && st.sig == sig) {
        if (names_kept(f.layer)) {
            plog("database: same changes as last launch, reusing this folder's database on the temp drive");
            replay(st);
            return;
        }
        plog("database: fh2-mods\\platform-db-layer-strings.txt doesn't have the keys of the car names this folder's database "
             "gives as text, so it's built again");
    }
    switch (found) {
    case Found::foreign: plog("database: building on " + shown + " (another mod's)"); break;
    case Found::game: plog("database: building on " + shown + " (a plain copy of the game's)"); break;
    case Found::fh2mp:
        plog("database: mediaoverride's gamedbRC.slt is FH2MP's copy of the server's, so it isn't built on; building on " + shown);
        break;
    case Found::marked:
        plog("database: mediaoverride's gamedbRC.slt is another Azimuth folder's, so it isn't built on; building on " + shown);
        break;
    default: plog("database: building on " + shown); break;
    }
    auto tmp = f.layer;
    tmp += L".platform-tmp";
    std::vector<std::string> results;
    std::map<uint16_t, std::wstring> strings;
    const int applied = build_copy(base, tmp, sources, results, strings);
    if (applied < 0) return;
    if (!applied) {
        DeleteFileW(tmp.c_str());
        drop_layer_copy(f, "no changes could be applied");
        keep_strings(f.layer_strings, {});
        return;
    }
    if (const DWORD e = move_over(tmp, f.layer)) {
        plog("database: couldn't write " + narrow(f.layer.wstring()) + " (error " + std::to_string(e) + ")");
        DeleteFileW(tmp.c_str());
        return;
    }
    keep_strings(f.layer_strings, std::move(strings));
    const Stamp t = stamp_of(f.layer).value_or(Stamp{});
    if (!write_state(f.layer_state, sig + " " + std::to_string(t.size) + " " + std::to_string(t.time), results))
        plog("database: couldn't write fh2-mods\\platform-db-layer.txt, so the database is built again next launch");
    plog("database: wrote this folder's database on the temp drive with changes from " + std::to_string(applied) +
         " car pack(s) and mod(s)");
}

// Cars fh2-mods\platform-car-ids.txt records, which a save may have (by Id), that aren't in the database the game reads
// this session: their pack left out, its SQL failed, the packs not read at all (Extra Options, say), or the pack
// removed. What the game does with such a save hasn't been tried; it may drop the car and save. So the first session
// a car goes missing (fh2-mods\platform-cars-missing.txt lists last time's), the saves are copied first; and every
// session the title screen and the MODS tab say so for a pack that's there, or for the packs that aren't read. The
// log has every car.
std::set<std::string> g_missing_packs;   // under g_warn_mu: packs whose recorded cars aren't in the database
bool g_missing_unread = false;           // the same for the packs that aren't read
std::vector<std::string> g_title_lines;  // the title screen's lines about them (messages from later())

void check_recorded_cars(const Files& f) {
    std::vector<std::pair<std::string, int64_t>> recorded;   // as the record spells them
    std::set<std::string> listed;
    for (const fs::path* file : {&f.car_ids, &f.car_ids_copy})
        for (const auto& l : read_lines(*file)) {
            std::string car;
            int64_t id = 0;
            if (record_line(l, car, id) && listed.insert(lower_ascii(car)).second) recorded.emplace_back(car, id);
        }
    std::set<std::string> packs;
    bool unread = false;
    std::vector<std::string> lines, missing;   // missing: "<car><TAB><id>"
    if (!recorded.empty()) {
        fs::path read;   // the database the game reads, as cars_without_powertrain finds it
        for (const fs::path* p : {&f.layer, &f.target, &f.mediapc, &f.media})
            if ((p != &f.layer || instance_layer()) && is_file(*p)) {
                read = *p;
                break;
            }
        sqlite3* db = nullptr;
        if (read.empty() || sqlite3_open_v2(narrow(read.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            sqlite3_close(db);
            return;   // can't tell: as it was
        }
        const auto ask = [&](const char* sql, const std::function<void(sqlite3_stmt*)>& bind) {
            std::string name;
            sqlite3_stmt* q = nullptr;
            if (sqlite3_prepare_v2(db, sql, -1, &q, nullptr) == SQLITE_OK) {
                bind(q);
                if (sqlite3_step(q) == SQLITE_ROW)
                    if (const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(q, 0))) name = t;
            }
            sqlite3_finalize(q);
            return name;
        };
        for (const auto& [car, id] : recorded) {
            const CarPack* pack = nullptr;
            for (const auto& p : car_packs())
                for (const auto& c : p.cars)
                    if (lower_ascii(c) == lower_ascii(car)) pack = &p;
            // There: its Id is still that car; or, for a pack whose SQL writes an Id of its own, the pack ran and the
            // database has the car by name.
            const int64_t at = id;
            if (lower_ascii(ask("SELECT MediaName FROM Data_Car WHERE Id = ?1", [&](sqlite3_stmt* q) { sqlite3_bind_int64(q, 1, at); })) ==
                lower_ascii(car))
                continue;
            if (pack && car_pack_problem(pack->name).empty() &&
                !ask("SELECT MediaName FROM Data_Car WHERE MediaName = ?1 COLLATE NOCASE",
                     [&](sqlite3_stmt* q) { sqlite3_bind_text(q, 1, car.c_str(), -1, SQLITE_TRANSIENT); }).empty())
                continue;
            missing.push_back(car + "\t" + std::to_string(id));
            const std::string what = "database: " + car + " (car " + std::to_string(id) + "), which a save may have, isn't in the "
                                     "game database this session: ";
            if (pack) {
                if (car_pack_problem(pack->name).empty()) note_car_pack_problem(pack->name, later("Its cars aren't in the game database this time."));
                const std::string why = car_pack_problem(pack->name);
                log_line("car pack " + pack->name, what + english(why));
                if (packs.insert(pack->name).second)
                    lines.push_back(later("Car pack {0}'s cars aren't in the game this time: {1}", {pack->title.empty() ? pack->name : pack->title, why}));
            } else if (!car_packs_unread().empty()) {
                plog(what + english(car_packs_unread()));
                if (!unread) lines.push_back(later("The car packs in fh2-cars aren't read this time: {0}", {car_packs_unread_why()}));
                unread = true;
            } else {
                plog(what + "no car pack in fh2-cars adds it now.");
            }
        }
        sqlite3_close(db);
    }
    // The first session a car goes missing, the saves are copied before the game reads them.
    const fs::path state = azimuth_dir() / "fh2-mods" / "platform-cars-missing.txt";
    const std::vector<std::string> before = read_lines(state);
    std::string fresh;
    for (const auto& m : missing)
        if (std::find(before.begin(), before.end(), m) == before.end())
            fresh += (fresh.empty() ? "" : ", ") + m.substr(0, m.find('\t')) + " (car " + m.substr(m.find('\t') + 1) + ")";
    if (!fresh.empty()) platform_save_snapshot("cars a save may have aren't in the game database: " + fresh);
    if (missing.empty()) {
        if (!before.empty()) remove_or_log(state, "fh2-mods\\platform-cars-missing.txt");
    } else if (missing != before) {
        std::string text;
        for (const auto& m : missing) text += m + "\r\n";
        if (!write_whole(state, text)) plog("database: couldn't write fh2-mods\\platform-cars-missing.txt");
    }
    std::scoped_lock lk{g_warn_mu};
    g_missing_packs  = std::move(packs);
    g_missing_unread = unread;
    g_title_lines    = std::move(lines);
}

} // namespace

std::map<uint16_t, std::wstring> database_strings() {
    std::scoped_lock lk{g_db_mu};
    return g_strings;
}

// The warnings for a pack or mod, in the game's language now, one sentence after another.
static std::string shown_warnings(const std::map<std::string, std::vector<std::string>>& all, const std::string& who) {
    std::scoped_lock lk{g_warn_mu};
    std::string text;
    if (const auto it = all.find(who); it != all.end())
        for (const std::string& w : it->second) text = join_sentences(text, shown(w));
    return text;
}

std::string car_pack_sql_warning(const std::string& pack) { return shown_warnings(g_pack_warnings, pack); }

std::string mod_sql_warning(const std::string& mod) { return shown_warnings(g_mod_warnings, mod); }

// The files as the game finds them now: this folder's own database on the temp drive (only ever the platform's),
// mediaoverride's told apart as apply_database tells it, and whether mediapc has one of its own.
DbFacts database_facts() {
    std::scoped_lock lk{g_db_mu};
    DbFacts d;
    if (game_mount().empty()) return d;
    const Files f = files();
    d.layer = instance_layer() && is_file(f.layer);
    switch (classify(f, read_state(f.state))) {
    case Found::missing: d.override_db = OverrideDb::none; break;
    case Found::ours:
    case Found::ours_old:
    case Found::marked: d.override_db = OverrideDb::platform; break;
    case Found::fh2ml: d.override_db = OverrideDb::fh2ml; break;
    case Found::fh2mp: d.override_db = OverrideDb::fh2mp; break;
    case Found::game: d.override_db = same_bytes(f.target, f.media) ? OverrideDb::copy_of_media : OverrideDb::copy_of_mediapc; break;
    case Found::foreign: d.override_db = OverrideDb::other; break;
    }
    d.mediapc               = is_file(f.mediapc);
    d.mediapc_same_as_media = d.mediapc && same_bytes(f.mediapc, f.media);
    return d;
}

std::vector<std::string> cars_without_powertrain() {
    std::vector<std::string> cars;
    fs::path read;
    {
        std::scoped_lock lk{g_db_mu};
        if (game_mount().empty()) return cars;
        const Files f = files();
        for (const fs::path* p : {&f.layer, &f.target, &f.mediapc, &f.media})
            if ((p != &f.layer || instance_layer()) && is_file(*p)) {
                read = *p;
                break;
            }
    }
    sqlite3* db = nullptr;
    if (read.empty() || sqlite3_open_v2(narrow(read.wstring()).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return cars;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, kCarsWithoutPowertrainSql, -1, &st, nullptr) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) {
            const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
            cars.push_back(std::string(name ? name : "?") + " (" +
                           (sqlite3_column_type(st, 1) == SQLITE_NULL ? std::string("none") : std::to_string(sqlite3_column_int64(st, 1))) + ")");
        }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return cars;
}

bool car_pack_cars_missing(const std::string& pack) {
    std::scoped_lock lk{g_warn_mu};
    return g_missing_packs.count(pack) != 0;
}

bool unread_cars_missing() {
    std::scoped_lock lk{g_warn_mu};
    return g_missing_unread;
}

std::vector<std::string> car_pack_title_lines() {
    std::scoped_lock lk{g_warn_mu};
    return g_title_lines;
}

// The build itself (apply_database), under g_db_mu.
static void build_and_write(const std::vector<FH2Mod*>& mods) {
    g_strings.clear();
    clear_sql_warnings();
    if (game_mount().empty()) return;
    const Files f = files();
    note_fh2mp_copy(f);

    // Car packs first, so mods' changes reach their cars too.
    std::vector<Sql> sources;
    for (const auto& p : car_packs())
        if (!p.sql.empty()) sources.push_back({"car pack " + p.name, &p.sql, &p});
    for (auto* m : mods)
        if (!m->sql.empty()) sources.push_back({m->name, &m->sql});

    if (instance_layer()) {
        std::error_code ec;
        if (!fs::is_directory(azimuth_dir() / L"fh2mp", ec)) {
            apply_layer_database(f, sources);
            return;
        }
        // FH2MP puts the server's database in mediaoverride for online play, which needs exactly
        // that one. One on the temp drive would hide it, so with FH2MP here the database is built in
        // mediaoverride as before, and FH2MP replaces it there online.
        drop_layer_copy(f, "FH2MP is in this Azimuth folder, so the database is built in mediaoverride");
        keep_strings(f.layer_strings, {});
    }
    reuse_strings(f.strings); // what mediaoverride's copy has, unless it's built again or goes

    const State st = read_state(f.state);
    if (sources.empty() && st.empty() && !is_file(f.base) && !is_file(f.fh2mp)) {
        keep_strings(f.strings, {});
        return;
    }
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
    if (sources.empty()) {
        release(f, st, found, false);
        keep_strings(f.strings, {});
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
    std::list<std::vector<std::string>> held;
    bool keep_last = false;
    const std::vector<Sql> resolved = with_car_ids(f, base, shown, sources, held, found == Found::ours, keep_last);
    if (keep_last) {
        plog("database: car packs' cars couldn't be checked against " + shown + ", as it couldn't be read, so the copy from "
             "last time is kept as it is, with their cars (it's built again next launch)");
        replay(st);
        return;
    }
    if (resolved.empty()) {
        release(f, st, found, true);
        keep_strings(f.strings, {});
        return;
    }
    // "foreign" for mediaoverride's file and the copy kept from it alike (the copy has its date),
    // so keeping it doesn't mean building again.
    const std::string sig = signature(kind, *bs, resolved);
    if (found == Found::ours && st.sig == sig) {
        if (names_kept(f.target)) {
            plog("database: same changes as last launch, reusing the copy");
            replay(st);
            return;
        }
        plog("database: fh2-mods\\platform-db-strings.txt doesn't have the keys of the car names the copy gives as text, so it's "
             "built again");
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

    auto tmp = f.target;
    tmp += L".platform-tmp";
    std::vector<std::string> results;
    std::map<uint16_t, std::wstring> strings;
    const int applied = build_copy(base, tmp, resolved, results, strings);
    if (applied < 0) return;

    if (!applied) {
        DeleteFileW(tmp.c_str());
        release(f, st, found, true);
        keep_strings(f.strings, {});
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
        if (!write_state(f.state, sig + " 0 0", results)) {
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
    keep_strings(f.strings, std::move(strings));
    const Stamp t           = stamp_of(f.target).value_or(Stamp{});
    const std::string stamp = std::to_string(t.size) + " " + std::to_string(t.time);
    // FH2MP deletes platform-db.txt, not this: once the copy is removed, what
    // it was built on still comes back (release).
    if (is_file(f.base)) {
        if (!write_state(f.built, stamp, {})) plog("database: couldn't write fh2-mods\\platform-db-base.txt");
    } else {
        remove_or_log(f.built, "fh2-mods\\platform-db-base.txt");
    }
    if (!write_state(f.state, sig + " " + stamp, results))
        plog("database: couldn't write fh2-mods\\platform-db.txt, so the copy isn't known as the platform's and is built "
             "again next launch");
    plog("database: wrote the game database with changes from " + std::to_string(applied) + " mod(s)");
}

void apply_database(const std::vector<FH2Mod*>& mods) {
    std::scoped_lock lk{g_db_mu};
    build_and_write(mods);
    if (!game_mount().empty()) check_recorded_cars(files());   // however the build went, against what the game reads
}

} // namespace fh2platform

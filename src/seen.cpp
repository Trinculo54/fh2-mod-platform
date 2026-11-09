// What the player has already been shown: fh2-mods\platform-seen.txt (see seen.hpp for who uses it).
//
// The file, UTF-8, one entry a line, tab-separated, with "#" lines as comments:
//
//     # FH2 Mod Platform: hints and tips the player has already seen, so each shows only once.
//     WreckingBall.dll<TAB>FirstTree<TAB>seen<TAB>2026-11-01
//     *<TAB>home<TAB>seen<TAB>2026-11-08
//     *<TAB>welcome.sessions<TAB>2<TAB>2026-11-08
//
//   who    a mod's DLL file name in fh2-mods, or "*" for the platform's own cues. Compared in any case (a-z).
//          1 to 260 bytes; no tab, line break, control character, "/", "\" or ":", and not starting with "#".
//   key    1 to 64 of letters, digits, "_", "." and "-", compared exactly. A mod's tips use its own keys (letters,
//          digits and "_"); the platform's own keys about a mod have a "." in them ("page.opened"), so they can't
//          clash with a tip's.
//   value  "seen", or a count; 1 to 64 printable bytes without a tab.
//   date   the day it was set (yyyy-mm-dd, local time), for whoever reads the file; the store doesn't use it.
// A line that doesn't read as that is left out, and the same (who, key) twice keeps the last. No names, gamertags or
// paths go in it.
//
// Crash safety: the file is read once, at seen_open, and every change is kept in memory and in a list of changes
// to make. The store's worker thread writes them about 0.1 s after the first: it reads the file again (another copy
// of the game may have written it meanwhile), makes the changes on what it read, writes the result to
// platform-seen.txt.tmp, flushes it, and renames it over the file (MoveFileEx, write-through). A crash or Azimuth's
// TerminateProcess at any point leaves either the old file or the new one, never half of one. A file that exists but
// can't be read is never written over: the changes wait for the next try, so another program's lock can't make the
// store lose what's in the file, only cues show again meanwhile.
#include "seen.hpp"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace fh2platform {
namespace {

constexpr DWORD kCoalesceMs = 100;    // after the first change, so a burst of them is one write
constexpr int kTries        = 3;      // a write that fails is tried again after kRetryMs, this many times in all
constexpr DWORD kRetryMs    = 1000;
constexpr std::size_t kMaxWho = 260, kMaxKey = 64, kMaxValue = 64;

struct Entry {
    std::string who, value, date;     // who as last spelled
};
using Key     = std::pair<std::string, std::string>;   // who in lower case, key
using Entries = std::map<Key, Entry>;

struct Change {
    Key key;
    bool set = false;                 // false: forget it
    Entry entry;
};

// Kept on the heap and never freed, so the worker thread can still be waiting on it while the process exits.
struct Store {
    std::mutex mu;
    std::condition_variable wake, written_cv;
    std::filesystem::path file;
    std::function<void(const std::string&)> log;
    bool open = false;
    uint64_t epoch = 0;               // which seen_open the state below belongs to
    Entries entries;                  // the file as read, with every change made since
    std::vector<Change> pending;      // changes not in the file yet, oldest first
    uint64_t changes = 0, written = 0;  // changes made since seen_open, and how many of them are in the file
    bool kick = false;                // seen_flush asks for a try now, after one that failed
    bool said_unwritable = false;
    bool worker = false;
};
Store& S() {
    static Store* s = new Store;
    return *s;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool printable(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}

bool who_ok(const std::string& w) {
    if (w == kSeenPlatform) return true;
    if (w.empty() || w.size() > kMaxWho || w[0] == '#' || !printable(w)) return false;
    return w.find_first_of("/\\:*") == std::string::npos;
}

bool key_ok(const std::string& k) {
    if (k.empty() || k.size() > kMaxKey) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return false;
    return true;
}

bool value_ok(const std::string& v) { return !v.empty() && v.size() <= kMaxValue && printable(v); }

std::string today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof b, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return b;
}

std::vector<std::string> split_tabs(const std::string& l) {
    std::vector<std::string> out(1);
    for (char c : l)
        if (c == '\t') out.emplace_back();
        else out.back() += c;
    return out;
}

enum class Read { ok, missing, unreadable };

// The file's entries, and how many lines were left out. Takes no lock.
Read read_file(const std::filesystem::path& file, Entries& out, int& bad) {
    out.clear();
    bad = 0;
    const HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? Read::missing : Read::unreadable;
    }
    std::string text;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 24);
    if (ok) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        ok = text.empty() || (ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr) && got == text.size());
    }
    CloseHandle(h);
    if (!ok) return Read::unreadable;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_tabs(line);
        if (f.size() < 3 || !who_ok(f[0]) || !key_ok(f[1]) || !value_ok(f[2])) {
            ++bad;
            continue;
        }
        out[{lower(f[0]), f[1]}] = Entry{f[0], f[2], f.size() > 3 && printable(f[3]) ? f[3] : std::string()};
    }
    return Read::ok;
}

// Writes `entries` to the file through file.tmp and a rename. Takes no lock. On failure, `why` says why.
bool write_file(const std::filesystem::path& file, const Entries& entries, std::string& why) {
    std::string text = "# FH2 Mod Platform: hints and tips the player has already seen, so each shows only once.\r\n"
                       "# Delete this file to see them all again. One line each, tab-separated: who (a mod's DLL, or * for\r\n"
                       "# the platform), key, value, and the day it was set.\r\n";
    for (const auto& [k, e] : entries) {
        text += e.who + "\t" + k.second + "\t" + e.value;
        if (!e.date.empty()) text += "\t" + e.date;
        text += "\r\n";
    }
    const std::filesystem::path tmp = file.wstring() + L".tmp";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "platform-seen.txt.tmp can't be made (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &put, nullptr) && put == text.size() &&
                    FlushFileBuffers(h);
    const DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        why = "platform-seen.txt.tmp can't be written (error " + std::to_string(e) + ")";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        why = "platform-seen.txt can't be replaced (error " + std::to_string(GetLastError()) + ")";
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// One try at putting `changes` into the file: what's in it now, with the changes made on top. Takes no lock.
bool write_changes(const std::filesystem::path& file, const std::vector<Change>& changes, std::string& why) {
    Entries now;
    int bad = 0;
    if (read_file(file, now, bad) == Read::unreadable) {
        why = "it couldn't be read first, and it isn't written over unread";
        return false;
    }
    for (const Change& c : changes)
        if (c.set) now[c.key] = c.entry;
        else now.erase(c.key);
    return write_file(file, now, why);
}

DWORD WINAPI writer(LPVOID) {
    Store& s = S();
    std::unique_lock lk{s.mu};
    for (;;) {
        s.wake.wait(lk, [&] { return !s.pending.empty(); });
        lk.unlock();
        Sleep(kCoalesceMs);
        lk.lock();
        if (s.pending.empty()) continue;   // a seen_open in the meantime
        const uint64_t epoch = s.epoch, upto = s.changes;
        const std::filesystem::path file = s.file;
        std::vector<Change> changes = std::move(s.pending);
        s.pending.clear();
        s.kick = false;
        lk.unlock();
        bool ok = false;
        std::string why;
        for (int i = 0; i < kTries && !ok; ++i) {
            if (i) Sleep(kRetryMs);
            ok = write_changes(file, changes, why);
        }
        std::string line;
        lk.lock();
        if (s.epoch != epoch) continue;   // started afresh meanwhile: those changes belonged to the old one
        if (ok) {
            s.written = upto;
            if (s.said_unwritable) line = "platform-seen.txt is written again";
            s.said_unwritable = false;
            s.written_cv.notify_all();
        } else {
            s.pending.insert(s.pending.begin(), changes.begin(), changes.end());
            if (!s.said_unwritable)
                line = "platform-seen.txt couldn't be written (" + why +
                       "); it's tried again at the next change, and until then hints already seen may show again next time";
            s.said_unwritable = true;
        }
        if (!line.empty() && s.log) {
            const auto log = s.log;
            lk.unlock();
            log(line);
            lk.lock();
        }
        // after a failure, wait for a change (or seen_flush) before trying again
        if (!ok) s.wake.wait(lk, [&] { return s.epoch != epoch || s.changes != upto || s.kick; });
    }
}

// Records a change under the lock, and wakes the worker.
void change(Store& s, Change c) {
    s.pending.push_back(std::move(c));
    ++s.changes;
    s.wake.notify_all();
}

} // namespace

void seen_open(const std::filesystem::path& file, std::function<void(const std::string&)> log) {
    Entries entries;
    int bad = 0;
    const Read r = read_file(file, entries, bad);
    Store& s = S();
    {
        std::scoped_lock lk{s.mu};
        s.file    = file;
        s.log     = log;
        s.open    = true;
        s.entries = std::move(entries);
        s.pending.clear();
        s.changes = s.written = 0;
        s.kick = s.said_unwritable = false;
        ++s.epoch;
        s.wake.notify_all();
        s.written_cv.notify_all();
        if (!s.worker) {
            if (const HANDLE t = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr)) {
                CloseHandle(t);
                s.worker = true;
            }
        }
    }
    if (!log) return;
    if (r == Read::unreadable)
        log("platform-seen.txt couldn't be read (another program may have it open), so hints and tips already seen may show "
            "again this session");
    else if (bad)
        log("platform-seen.txt: " + std::to_string(bad) + " line(s) couldn't be read and were left out");
}

std::string seen_get(const std::string& who, const std::string& key) {
    Store& s = S();
    std::scoped_lock lk{s.mu};
    const auto it = s.entries.find({lower(who), key});
    return it == s.entries.end() ? std::string() : it->second.value;
}

bool seen_set(const std::string& who, const std::string& key, const std::string& value) {
    if (!who_ok(who) || !key_ok(key) || !value_ok(value)) return false;
    Store& s = S();
    std::scoped_lock lk{s.mu};
    if (!s.open || !s.worker) return false;
    const Key k{lower(who), key};
    const Entry e{who, value, today()};
    const auto it = s.entries.find(k);
    if (it != s.entries.end() && it->second.value == value && it->second.who == who && it->second.date == e.date) return true;
    s.entries[k] = e;
    change(s, Change{k, true, e});
    return true;
}

bool seen_clear(const std::string& who, const std::string& key) {
    Store& s = S();
    std::scoped_lock lk{s.mu};
    const auto it = s.entries.find({lower(who), key});
    if (!s.open || it == s.entries.end()) return false;
    Change c{it->first, false, {}};
    s.entries.erase(it);
    change(s, std::move(c));
    return true;
}

int seen_clear_if(const std::function<bool(const std::string&, const std::string&, const std::string&)>& drop) {
    Store& s = S();
    std::vector<Key> gone;
    {
        // the predicate runs on a copy, outside the lock, so it may call seen_get
        Entries copy;
        {
            std::scoped_lock lk{s.mu};
            if (!s.open) return 0;
            copy = s.entries;
        }
        for (const auto& [k, e] : copy)
            if (drop(e.who, k.second, e.value)) gone.push_back(k);
    }
    int n = 0;
    std::scoped_lock lk{s.mu};
    for (const Key& k : gone)
        if (s.entries.erase(k)) {
            change(s, Change{k, false, {}});
            ++n;
        }
    return n;
}

bool seen_flush(uint32_t timeout_ms) {
    Store& s = S();
    std::unique_lock lk{s.mu};
    if (!s.open) return false;
    const uint64_t epoch = s.epoch, upto = s.changes;
    if (s.written >= upto) return true;
    s.kick = true;
    s.wake.notify_all();
    return s.written_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                 [&] { return s.epoch != epoch || s.written >= upto; }) &&
           s.epoch == epoch;
}

} // namespace fh2platform

// The store of what the player has already seen (src/seen.cpp), with no game: fh2-mods\platform-seen.txt read,
// written atomically, merged with what another copy of the game wrote, and never written over while it can't be
// read. Runs in a folder of its own under the current one. Exits 0 when every check passes.
#include "seen.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace fh2platform;

namespace {

int g_failed = 0;
std::mutex g_log_mu;
std::vector<std::string> g_log;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

void log_line(const std::string& l) {
    std::scoped_lock lk{g_log_mu};
    g_log.push_back(l);
}
bool logged(const std::string& part) {
    std::scoped_lock lk{g_log_mu};
    for (const auto& l : g_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
void clear_log() {
    std::scoped_lock lk{g_log_mu};
    g_log.clear();
}

std::string slurp(const fs::path& p) {
    std::ifstream f{p, std::ios::binary};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
void put(const fs::path& p, const std::string& text) { std::ofstream{p, std::ios::binary} << text; }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// The lines of the file that aren't comments.
std::vector<std::string> entries(const fs::path& p) {
    std::vector<std::string> out;
    std::istringstream in{slurp(p)};
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && l[0] != '#') out.push_back(l);
    }
    return out;
}

bool dated_today(const std::string& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char d[16];
    std::snprintf(d, sizeof d, "\t%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return line.size() > 11 && line.compare(line.size() - 11, 11, d) == 0;
}

} // namespace

int main() {
    const fs::path dir = fs::current_path() / "seen_test_dir";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path file = dir / "platform-seen.txt";

    // ---- before the store is open: nothing is seen, and nothing can be set
    check(seen_get("WreckingBall.dll", "FirstTree").empty() && !seen_set("WreckingBall.dll", "FirstTree") && !seen_flush(10),
          "not open yet: nothing seen, nothing set");

    // ---- no file: nothing seen, and nothing written until something is set
    seen_open(file, log_line);
    check(!seen_has("WreckingBall.dll", "FirstTree") && seen_flush(100) && !fs::exists(file),
          "no file: nothing seen, and the file isn't made until something is set");

    // ---- set, written, read back
    check(seen_set("WreckingBall.dll", "FirstTree") && seen_has("WreckingBall.dll", "FirstTree"), "set: seen at once");
    check(seen_get("wreckingball.DLL", "FirstTree") == "seen", "the DLL's name is compared in any case");
    check(seen_get("WreckingBall.dll", "firsttree").empty(), "the key is compared exactly");
    check(seen_set(kSeenPlatform, "home") && seen_set(kSeenPlatform, "welcome.sessions", "2"), "the platform's own cues");
    check(seen_flush(3000), "flushed");
    {
        const auto e = entries(file);
        check(e.size() == 3, "three entries in the file");
        check(e.size() == 3 && e[0].rfind("*\thome\tseen\t", 0) == 0 && e[1].rfind("*\twelcome.sessions\t2\t", 0) == 0 &&
                  e[2].rfind("WreckingBall.dll\tFirstTree\tseen\t", 0) == 0,
              "each as who, key, value, date");
        check(!e.empty() && dated_today(e[0]), "dated today, yyyy-mm-dd");
        check(has(slurp(file), "# FH2 Mod Platform: hints and tips the player has already seen"), "with a comment saying what it is");
        check(!fs::exists(dir / "platform-seen.txt.tmp"), "no .tmp left behind");
    }
    seen_open(file, log_line);
    check(seen_has("WreckingBall.dll", "FirstTree") && seen_get("*", "welcome.sessions") == "2", "read back after a restart");

    // ---- unusable who, key and value: refused, nothing set
    check(!seen_set("", "Key"), "refused: no who");
    check(!seen_set("#Mod.dll", "Key"), "refused: a who that would be a comment");
    check(!seen_set("C:\\Mods\\Mod.dll", "Key") && !seen_set("mods/Mod.dll", "Key"), "refused: a path, not a file name");
    check(!seen_set("Mod.dll", "") && !seen_set("Mod.dll", "two words") && !seen_set("Mod.dll", "tab\there"), "refused: bad keys");
    check(!seen_set("Mod.dll", std::string(65, 'k')) && seen_set("Mod.dll", std::string(64, 'k')), "keys up to 64 characters");
    check(!seen_set("Mod.dll", "Key", "") && !seen_set("Mod.dll", "Key", "a\tb") && !seen_set("Mod.dll", "Key", "line\nbreak"),
          "refused: bad values");
    check(!seen_set("Bad\tName.dll", "Key") && !seen_has("Mod.dll", "Key"), "nothing set by a refused call");

    // ---- clearing: one entry, then by a test (every mod's tips: not the platform's, not keys with a ".")
    seen_set("Mod.dll", "TipA");
    seen_set("Mod.dll", "page.opened");
    seen_set("Other Mod.dll", "TipB");
    check(seen_clear("Mod.dll", std::string(64, 'k')) && !seen_clear("Mod.dll", "NeverSet"), "clear: true only when it was set");
    const int dropped = seen_clear_if([](const std::string& who, const std::string& key, const std::string&) {
        return who != kSeenPlatform && key.find('.') == std::string::npos;
    });
    check(dropped == 3, "clear_if: every mod's tips (FirstTree, TipA, TipB)");
    check(!seen_has("WreckingBall.dll", "FirstTree") && !seen_has("Other Mod.dll", "TipB") && seen_has("Mod.dll", "page.opened") &&
              seen_has("*", "home"),
          "clear_if: the platform's cues and its keys about a mod stay");
    check(seen_flush(3000), "flushed");
    {
        const std::string t = slurp(file);
        check(!has(t, "FirstTree") && !has(t, "TipB") && has(t, "Mod.dll\tpage.opened\tseen") && has(t, "*\thome"), "and so does the file");
    }

    // ---- another copy of the game wrote the file meanwhile: its entries stay, ours go on top
    {
        std::string t = slurp(file);
        t += "Elsewhere.dll\tTheirTip\tseen\t2026-11-01\r\n";
        put(file, t);
        seen_set("Mod.dll", "Mine");
        check(seen_flush(3000), "flushed");
        const std::string after = slurp(file);
        check(has(after, "Elsewhere.dll\tTheirTip\tseen\t2026-11-01") && has(after, "Mod.dll\tMine\tseen"),
              "merged with what another copy wrote: both kept");
        seen_clear("Mod.dll", "Mine");
        check(seen_flush(3000) && !has(slurp(file), "\tMine\t") && has(slurp(file), "TheirTip"), "a clear merges the same way");
    }

    // ---- a file as people and older copies leave it: a BOM, LF or CRLF, comments, bad lines, a key twice
    put(file, "\xEF\xBB\xBF# a comment\nA.dll\tOne\tseen\t2026-01-01\r\n\nA.dll\tOne\t3\nB.dll\tbad key\tseen\nonly two\tfields\n"
              "B.dll\tTwo\tseen\n#C.dll\tThree\tseen\n");
    clear_log();
    seen_open(file, log_line);
    check(seen_get("A.dll", "One") == "3", "the same key twice: the last wins");
    check(seen_get("B.dll", "Two") == "seen", "a line without a date is read");
    check(!seen_has("C.dll", "Three"), "a commented line isn't read");
    check(logged("platform-seen.txt: 2 line(s) couldn't be read and were left out"), "bad lines are left out, and the log says so");

    // ---- a crash in the middle of a write leaves platform-seen.txt.tmp: it's ignored, and replaced
    put(dir / "platform-seen.txt.tmp", "A.dll\tHalf");
    seen_open(file, log_line);
    check(seen_get("A.dll", "One") == "3" && !seen_has("A.dll", "Half"), "a .tmp left by a crash isn't read");
    seen_set("A.dll", "AfterCrash");
    check(seen_flush(3000) && !fs::exists(dir / "platform-seen.txt.tmp") && has(slurp(file), "AfterCrash"), "and is gone after the next write");

    // ---- locked by another program while writing: the file isn't touched, the changes wait, and go in once it's free
    {
        const std::string before = slurp(file);
        const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "the file is locked for the test");
        clear_log();
        seen_set("A.dll", "WhileLocked");
        check(!seen_flush(4000), "locked: not written");
        check(logged("platform-seen.txt couldn't be written (it couldn't be read first, and it isn't written over unread)"),
              "the log says so");
        check(seen_has("A.dll", "WhileLocked"), "it still counts as seen this session");
        CloseHandle(lock);
        check(slurp(file) == before, "the file is as it was");
        check(seen_flush(4000), "free again: written");
        const std::string after = slurp(file);
        check(has(after, "A.dll\tWhileLocked\tseen") && has(after, "A.dll\tAfterCrash") && has(after, "B.dll\tTwo"),
              "with everything that was in it");
        check(logged("platform-seen.txt is written again"), "the log says it's written again");
    }

    // ---- locked when the store opens: nothing counts as seen (cues come back), and what's in it isn't lost
    {
        const HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        clear_log();
        seen_open(file, log_line);
        check(!seen_has("A.dll", "One") && logged("platform-seen.txt couldn't be read"), "locked at the start: nothing seen, and said");
        CloseHandle(lock);
        seen_set("A.dll", "Later");
        check(seen_flush(3000), "flushed");
        const std::string t = slurp(file);
        check(has(t, "A.dll\tLater\tseen") && has(t, "A.dll\tOne\t3") && has(t, "A.dll\tWhileLocked"),
              "the next write keeps what was in the file");
    }

    // ---- from many threads at once
    {
        seen_open(file, log_line);
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([t] {
                for (int i = 0; i < 50; ++i) seen_set("Thread" + std::to_string(t) + ".dll", "Key" + std::to_string(i));
            });
        for (auto& t : threads) t.join();
        check(seen_flush(5000), "flushed");
        seen_open(file, log_line);
        int n = 0;
        for (int t = 0; t < 8; ++t)
            for (int i = 0; i < 50; ++i) n += seen_has("Thread" + std::to_string(t) + ".dll", "Key" + std::to_string(i));
        check(n == 400, "400 set from 8 threads, all in the file");
    }

    // ---- a missing folder (fh2-mods gone): nothing seen, nothing written, no crash
    {
        const fs::path gone = dir / "no-such-folder" / "platform-seen.txt";
        clear_log();
        seen_open(gone, log_line);
        check(seen_set("A.dll", "One") && seen_has("A.dll", "One"), "set in memory");
        check(!seen_flush(3500) && !fs::exists(gone), "not written");
        check(logged("platform-seen.txt couldn't be written (platform-seen.txt.tmp can't be made"), "the log says why");
    }

    fs::remove_all(dir, ec);
    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

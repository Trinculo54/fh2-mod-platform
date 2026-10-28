// Sessions (API 9, platform 2.8.0). Each run of the game gets an id, "20261021-203224-1234": the
// time the platform started and the process id, since the game can start twice in one launch and
// Restart now starts a new process. platform.log is still rewritten every launch, and a copy of
// it goes to fh2-mods\platform-logs\platform-<id>.log, the newest kSessionLogs kept.
// fh2-mods\platform-session.json says what's running for tools: written to a .tmp and moved over
// on each change, and marked "crashed" by the crash handler, which only has fixed buffers (the
// heap may be what broke), so the file's head is kept ready for it.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

#ifndef FH2MOD_PLATFORM_VERSION
#define FH2MOD_PLATFORM_VERSION "0.0.0"
#endif

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kSessionLogs = 20;
std::string g_id;      // the session's id
std::string g_started; // "2026-10-21 20:32:24"
std::mutex g_mu;
std::string g_pipe;    // the command pipe, once it's up
std::deque<std::wstring> g_paths; // what session_log_path handed out (never moved)

// The status file up to its "state", for the crash handler: two buffers, the newest one's
// index published once it's filled.
constexpr std::size_t kHeadSize = 16384;
char g_head[2][kHeadSize];
std::atomic<int> g_head_ix{-1};

std::string json(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) {
            char u[8];
            std::snprintf(u, sizeof u, "\\u%04x", c);
            out += u;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// Deletes the oldest files called "<prefix>*<suffix>" (or folders, with `folders`) in `dir`, so
// that `keep` are left; names sort by time, as session ids do.
void prune(const fs::path& dir, const std::wstring& prefix, const std::wstring& suffix, std::size_t keep, bool folders) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if ((folders ? e.is_directory(ec) : e.is_regular_file(ec)) && n.size() >= prefix.size() + suffix.size() &&
            n.compare(0, prefix.size(), prefix) == 0 && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
            found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());
    for (std::size_t i = 0; i + keep < found.size(); ++i) folders ? (void)fs::remove_all(found[i], ec) : (void)fs::remove(found[i], ec);
}

bool plain_folder_name(const std::wstring& f) {
    if (f.empty() || f.size() > 64 || f == L"." || f == L"..") return false;
    for (const wchar_t c : f)
        if (c < 0x20 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos) return false;
    return true;
}

} // namespace

FILE* start_session() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char id[64], started[32];
    std::snprintf(id, sizeof id, "%04u%02u%02u-%02u%02u%02u-%lu", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  GetCurrentProcessId());
    std::snprintf(started, sizeof started, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond);
    g_id      = id;
    g_started = started;
    const fs::path dir = azimuth_dir() / L"fh2-mods" / L"platform-logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    prune(dir, L"platform-", L".log", kSessionLogs - 1, false);
    return _wfopen((dir / (L"platform-" + std::wstring(g_id.begin(), g_id.end()) + L".log")).c_str(), L"w");
}

std::string session_line() {
    return "session " + g_id + ": started " + g_started + ", process " + std::to_string(GetCurrentProcessId()) +
           "; this log is kept as fh2-mods\\platform-logs\\platform-" + g_id + ".log";
}

const char* session_id() { return g_id.c_str(); }

void set_session_pipe(const std::string& name) {
    {
        std::scoped_lock lk{g_mu};
        g_pipe = name;
    }
    write_session_status();
}

void write_session_status() {
    std::string head;
    {
        std::scoped_lock lk{g_mu};
        head = "{\r\n  \"session\": " + json(g_id) + ",\r\n  \"pid\": " + std::to_string(GetCurrentProcessId()) +
               ",\r\n  \"started\": " + json(g_started) + ",\r\n  \"platform\": " + json(FH2MOD_PLATFORM_VERSION) +
               ",\r\n  \"azimuth\": " + json(narrow(azimuth_dir().wstring())) + ",\r\n  \"mods\": {";
        bool first = true;
        for (const ModEntry& m : mod_entries()) {
            if (!m.mod) continue;
            head += std::string(first ? "" : ",") + "\r\n    " + json(m.name) + ": " + json(m.version);
            first = false;
        }
        head += std::string(first ? "}" : "\r\n  }") + ",\r\n  \"pipe\": " + json(g_pipe) + ",\r\n";
    }
    // the crash handler's copy first
    const int ix = g_head_ix.load() == 0 ? 1 : 0;
    std::snprintf(g_head[ix], kHeadSize, "%s", head.c_str());
    g_head_ix = ix;
    const std::string body = head + "  \"state\": \"running\"\r\n}\r\n";
    const fs::path file    = azimuth_dir() / L"fh2-mods" / L"platform-session.json";
    const fs::path tmp     = file.wstring() + L".tmp";
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        std::fclose(f);
        if (ok) MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

// The crash handler (crash.cpp): the status file says "crashed", with the report's name. Fixed
// buffers and Win32 only.
void write_session_crashed(const wchar_t* fh2mods_dir, const char* report) {
    const int ix = g_head_ix.load();
    if (ix < 0) return;
    static char body[kHeadSize + 512];
    const int n = std::snprintf(body, sizeof body, "%s  \"state\": \"crashed\",\r\n  \"crash_report\": \"%s\"\r\n}\r\n", g_head[ix],
                                report);
    if (n <= 0) return;
    wchar_t tmp[MAX_PATH + 64], file[MAX_PATH + 64];
    std::swprintf(tmp, sizeof tmp / sizeof *tmp, L"%ls\\platform-session.json.tmp", fh2mods_dir);
    std::swprintf(file, sizeof file / sizeof *file, L"%ls\\platform-session.json", fh2mods_dir);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w       = 0;
    const bool ok = WriteFile(f, body, static_cast<DWORD>(std::min<std::size_t>(n, sizeof body - 1)), &w, nullptr);
    CloseHandle(f);
    if (ok) MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING);
}

// ---- save snapshots ---------------------------------------------------------------------------
// Mods ask from fh2mod_init; once every one has returned, the saves folder is copied (once, on a
// thread of its own) to fh2-mods\save-snapshots\<session id>\. The game only reads the saves
// when A is pressed on the title screen, long after. Nothing is ever written to the saves.

namespace {
std::vector<std::string> g_snap_for; // "Mod (reason)"
uint32_t g_snap_keep = 0;
bool g_snap_closed   = false;        // every fh2mod_init has returned
fs::path saves_dir() { return azimuth_dir() / L"data" / L"titles" / L"1A96545B" / L"saves"; }
} // namespace

int request_save_snapshot(FH2Mod* m, const char* reason, uint32_t keep) {
    if (!m) return 0;
    std::error_code ec;
    std::scoped_lock lk{g_mu};
    if (g_snap_closed) {
        log_line(m->name, "request_save_snapshot came after the mods had started; call it from fh2mod_init");
        return 0;
    }
    if (!fs::is_directory(saves_dir(), ec)) {
        log_line(m->name, "request_save_snapshot: there's no saves folder at data\\titles\\1A96545B\\saves");
        return 0;
    }
    const std::string why = reason && *reason ? reason : "no reason given";
    g_snap_for.push_back(m->name + " (" + why + ")");
    g_snap_keep = std::max(g_snap_keep, keep < 1 ? 1u : keep);
    log_line(m->name, "asked for a copy of the saves before the game reads them: " + why);
    return 1;
}

void start_save_snapshot() {
    std::vector<std::string> asked;
    uint32_t keep = 0;
    {
        std::scoped_lock lk{g_mu};
        g_snap_closed = true;
        asked         = g_snap_for;
        keep          = g_snap_keep;
    }
    if (asked.empty()) return;
    std::string who;
    for (const std::string& a : asked) who += (who.empty() ? "" : ", ") + a;
    struct Job {
        std::string who;
        uint32_t keep;
    };
    auto* job = new Job{who, keep};
    HANDLE t  = CreateThread(
        nullptr, 0,
        [](LPVOID p) -> DWORD {
            const Job j = *static_cast<Job*>(p);
            delete static_cast<Job*>(p);
            const fs::path root = azimuth_dir() / L"fh2-mods" / L"save-snapshots";
            const fs::path to   = root / std::wstring(g_id.begin(), g_id.end());
            std::error_code ec;
            fs::create_directories(to, ec);
            fs::copy(saves_dir(), to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            std::size_t files = 0;
            uintmax_t bytes   = 0;
            for (auto it = fs::recursive_directory_iterator(to, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec)) {
                    ++files;
                    bytes += it->file_size(ec);
                }
            if (ec || files == 0) {
                plog("save snapshot: copying the saves failed" + (ec ? " (" + ec.message() + ")" : std::string()) + ", for " + j.who);
                return 0;
            }
            prune(root, L"", L"", j.keep, true);
            plog("save snapshot: " + std::to_string(files) + " file(s), " + std::to_string((bytes + 524288) / 1048576) +
                 " MB, copied to fh2-mods\\save-snapshots\\" + g_id + " (the newest " + std::to_string(j.keep) + " kept), for " + j.who);
            return 0;
        },
        job, 0, nullptr);
    if (t) CloseHandle(t);
    else delete job;
}

const wchar_t* session_log_path(FH2Mod* m, const wchar_t* folder, uint32_t keep) {
    if (!m) return nullptr;
    const std::wstring f = folder ? folder : L"";
    if (!plain_folder_name(f)) {
        log_line(m->name, "session_log_path: \"" + narrow(f) + "\" isn't a folder name (your mod's own folder next to azimuth.exe)");
        return nullptr;
    }
    const fs::path dir = azimuth_dir() / f / L"logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        log_line(m->name, "session_log_path: couldn't make " + narrow(f) + "\\logs");
        return nullptr;
    }
    if (keep < 1) keep = 1;
    prune(dir, L"", L".log", keep - 1, false);
    const std::wstring path = (dir / (std::wstring(g_id.begin(), g_id.end()) + L".log")).wstring();
    log_line(m->name, "session log: " + narrow(f) + "\\logs\\" + g_id + ".log (the newest " + std::to_string(keep) + " kept)");
    std::scoped_lock lk{g_mu};
    return g_paths.emplace_back(path).c_str();
}

} // namespace fh2platform

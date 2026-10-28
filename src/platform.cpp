// FH2 Mod Platform -- d3dcompiler_47.dll and DWrite.dll proxy for Azimuth.
//
// azimuth.exe statically imports d3dcompiler_47.dll (for D3DCompile, up to
// Azimuth 1.0.1) and DWrite.dll (for DWriteCreateFactory, from 1.0.1 on), and
// Windows looks next to the exe first, so this DLL, under either name, gets
// loaded at startup without touching any of Azimuth's own files, and without
// fighting Forza Horizon 2 Extra Options (which owns winmm.dll). Every
// D3DCompiler and DirectWrite export is a PE forwarder to the real System32
// DLL (d3dcompiler_47.def for llvm-mingw, msvc_exports.inc for MSVC), so
// shader compiles and text never touch it.
//
// On a thread of its own (outside the loader lock), it loads every DLL in
// fh2-mods and calls each one's fh2mod_init with the API in fh2mod.h:
// string-table edits merged across mods, title-screen notices, update
// checks, logging and finding the game. Mods without fh2mod_init (built
// before the platform) still load; they just start themselves from DllMain.
// Everything is logged to fh2-mods\platform.log.

#include <windows.h>
#include <psapi.h>

#include "platform.hpp"
#include "platform_version.h"
#include "strings.hpp"
#include "updates.hpp"

#ifdef _MSC_VER
#include "msvc_exports.inc"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#ifndef FH2MOD_PLATFORM_VERSION
#define FH2MOD_PLATFORM_VERSION "0.0.0"
#endif


namespace fh2platform {

namespace fs = std::filesystem;

namespace {

fs::path g_azimuth;
fs::path g_game; // Mount
std::wstring g_azimuth_str, g_game_str;

std::recursive_mutex g_mu;
std::vector<std::unique_ptr<FH2Mod>> g_mods;
std::map<std::string, std::string> g_table_names; // lower -> as first given
std::set<std::string> g_dirty;                    // lower-case table names
std::wstring g_current_dll;                       // the DLL whose fh2mod_init is running
struct Found {                                    // every DLL in fh2-mods, in load order
    std::wstring file;
    bool loaded = false, disabled = false, has_init = false;
    std::string problem;                          // why it didn't load or start, for the MODS tab
    bool online_off = false;                      // not loaded or started: the game is online (FH2MP)
};
std::vector<Found> g_found;
std::vector<std::unique_ptr<FH2Mod>> g_held;      // mods that registered but don't run online (offline_only)
bool g_online = false;                            // started from the FH2MP launcher (set before any mod loads)

std::mutex g_log_mu;
FILE* g_log = nullptr;
FILE* g_log_copy = nullptr; // fh2-mods\platform-logs\platform-<session id>.log

FH2ModApi g_api{};

HMODULE g_self = nullptr; // this copy of the platform
std::wstring g_self_name; // its file name: d3dcompiler_47.dll or DWrite.dll
std::string g_copies;     // the other copies in this process, for platform.log

} // namespace

// ---- log -----------------------------------------------------------------------

void log_line(const std::string& who, const std::string& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lk{g_log_mu};
    for (FILE* f : {g_log, g_log_copy}) {
        if (!f) continue;
        std::fprintf(f, "%02u:%02u:%02u.%03u %s%s%s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     who.empty() ? "" : "[", who.c_str(), who.empty() ? "" : "] ");
        std::fputs(text.c_str(), f);
        std::fputc('\n', f);
        std::fflush(f);
    }
}
fs::path azimuth_dir() { return g_azimuth; }
fs::path game_mount() { return g_game; }

namespace {


std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// FileVersion of a loaded module, straight from its resources (no
// version.dll needed).
std::wstring lower_w(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string version_string(HMODULE mod) {
    HRSRC r = FindResourceW(mod, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    if (!r) return {};
    HGLOBAL g     = LoadResource(mod, r);
    const auto* p = static_cast<const unsigned char*>(g ? LockResource(g) : nullptr);
    const DWORD n = SizeofResource(mod, r);
    if (!p) return {};
    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= n; i += 4) {
        VS_FIXEDFILEINFO fi;
        std::memcpy(&fi, p + i, sizeof fi);
        if (fi.dwSignature == 0xFEEF04BD)
            return std::to_string(HIWORD(fi.dwFileVersionMS)) + "." + std::to_string(LOWORD(fi.dwFileVersionMS)) +
                   "." + std::to_string(HIWORD(fi.dwFileVersionLS));
    }
    return {};
}

// ---- one platform per process --------------------------------------------------

// Azimuth 1.0.3 no longer imports d3dcompiler_47.dll, so the platform also ships as DWrite.dll,
// which Azimuth imports from 1.0.1 on. Azimuth 1.0.1 imports both, so two copies can start in one
// process, or one copy and an older d3dcompiler_47.dll from another mod's zip, and only one may
// load the mods. Every platform since 2.3.0 exports RestartHelper; copies that take part in this
// also export fh2mod_platform_version. An older copy runs whatever happens, so then the newer
// ones stand down; otherwise the newest copy runs, DWrite.dll on a tie. Each copy decides on its
// own thread, which only starts once the loader has started every DLL Azimuth imports, under a
// mutex; the copy that runs leaves an event behind, so a copy loaded later stands down too.

uint32_t packed_version() { return PLATFORM_V1 << 16 | PLATFORM_V2 << 8 | PLATFORM_V3; }

struct Copy {
    HMODULE mod;
    std::wstring file; // file name
    uint32_t version;  // packed; 0 for a copy from before fh2mod_platform_version
};

std::string describe(const Copy& c) {
    const std::string v = version_string(c.mod);
    return narrow(c.file) + " (FH2 Mod Platform" + (v.empty() ? "" : " " + v) + ")";
}

bool is_dwrite(const Copy& c) { return lower_w(c.file) == L"dwrite.dll"; }

// The two names the platform ships under, spelt as shipped. Windows names a DLL loaded as an
// import the way the importer spells it, and Azimuth asks for D3DCOMPILER_47.dll.
std::wstring shipped_name(const std::wstring& file) {
    const std::wstring l = lower_w(file);
    if (l == L"d3dcompiler_47.dll") return L"d3dcompiler_47.dll";
    if (l == L"dwrite.dll") return L"DWrite.dll";
    return file;
}

// Every copy of the platform loaded from Azimuth's folder, this one included.
std::vector<Copy> platform_copies() {
    std::vector<Copy> out;
    HMODULE mods[1024];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return out;
    const DWORD count = std::min<DWORD>(need / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < count; ++i) {
        if (!GetProcAddress(mods[i], "RestartHelper")) continue;
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(mods[i], path, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        const fs::path p{path};
        if (lower_w(p.parent_path().wstring()) != lower_w(g_azimuth.wstring())) continue;
        const auto version = reinterpret_cast<uint32_t (*)()>(GetProcAddress(mods[i], "fh2mod_platform_version"));
        out.push_back({mods[i], shipped_name(p.filename().wstring()), version ? version() : 0});
    }
    return out;
}

// fh2-mods\platform-standby.log, for when an older copy runs instead: it owns platform.log and
// knows nothing of this one. The first copy to stand down in a launch starts the file afresh.
void note_standing_down(const std::wstring& tag, const std::string& line) {
    HANDLE once      = CreateEventW(nullptr, TRUE, FALSE, (tag + L".standby").c_str()); // open for the launch
    const bool fresh = once && GetLastError() != ERROR_ALREADY_EXISTS;
    std::error_code ec;
    fs::create_directories(g_azimuth / "fh2-mods", ec);
    if (FILE* f = _wfopen((g_azimuth / "fh2-mods" / "platform-standby.log").c_str(), fresh ? L"w" : L"a")) {
        std::fputs(line.c_str(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

// Whether this copy loads the mods. When it does, g_copies names the copies that stood down.
bool this_copy_runs() {
    const std::wstring tag = L"Local\\FH2ModPlatform." + std::to_wstring(GetCurrentProcessId());
    HANDLE mu              = CreateMutexW(nullptr, FALSE, tag.c_str());
    if (mu) WaitForSingleObject(mu, 10000);
    bool run = true;
    if (HANDLE e = OpenEventW(SYNCHRONIZE, FALSE, (tag + L".running").c_str())) {
        CloseHandle(e);
        run = false; // another copy runs already
    } else {
        const auto copies = platform_copies();
        const Copy* best  = nullptr;
        const Copy* older = nullptr;
        for (const auto& c : copies) {
            if (!c.version) {
                if (!older) older = &c;
            } else if (!best || c.version > best->version ||
                       (c.version == best->version &&
                        (is_dwrite(c) > is_dwrite(*best) || (is_dwrite(c) == is_dwrite(*best) && c.mod < best->mod)))) {
                best = &c;
            }
        }
        if (older) {
            run = false;
            note_standing_down(tag, "FH2 Mod Platform " FH2MOD_PLATFORM_VERSION " (" + narrow(g_self_name) +
                                        ") stood down: " + describe(*older) +
                                        " is older and can't stand down itself, so it loads the mods. Replace it "
                                        "with a copy of " + narrow(g_self_name) + ", or delete it.");
        } else if (best && best->mod != g_self) {
            run = false; // the copy that runs names this one in platform.log
        } else {
            for (const auto& c : copies)
                if (c.mod != g_self) g_copies += (g_copies.empty() ? "" : "; ") + describe(c) + " stood down";
            CreateEventW(nullptr, TRUE, TRUE, (tag + L".running").c_str()); // open for the launch
        }
    }
    if (mu) {
        ReleaseMutex(mu);
        CloseHandle(mu);
    }
    return run;
}

// ---- string tables -------------------------------------------------------------

fs::path manifest_path() { return g_azimuth / "fh2-mods" / "platform-tables.txt"; }

std::set<std::string> read_manifest() {
    std::set<std::string> t;
    std::ifstream in{manifest_path()};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) {
            t.insert(lower(line));
            g_table_names.emplace(lower(line), line);
        }
    }
    return t;
}

// Rebuilds every dirty table from all mods' edits (later registrations win
// on the same key), and Main.str's "TO START" from the notices.
void apply_all() {
    std::scoped_lock lk{g_mu};
    if (g_game.empty() || g_dirty.empty()) return;
    std::set<std::string> written = read_manifest();
    for (const auto& table : g_dirty) {
        std::map<uint16_t, std::wstring> keys;
        std::map<std::wstring, std::wstring> texts;
        for (const auto& m : g_mods) {
            if (auto it = m->set.find(table); it != m->set.end())
                for (const auto& [k, v] : it->second) {
                    if (keys.count(k) && keys[k] != v)
                        plog("note: " + m->name + " overrides another mod's string " + std::to_string(k) + " in " +
                             g_table_names[table]);
                    keys[k] = v;
                }
            if (auto it = m->repl.find(table); it != m->repl.end())
                for (const auto& [f, t] : it->second) texts[f] = t;
        }
        const bool is_main = table == "main.str";
        HANDLE mu          = nullptr;
        bool notices       = false;
        if (is_main) {
            // Mods from before the platform render Main.str themselves under
            // this mutex, from the same notice files.
            mu = CreateMutexW(nullptr, FALSE, L"Local\\FH2ModNotices");
            if (mu) WaitForSingleObject(mu, 10000);
            notices = any_notices(g_azimuth);
        }
        const bool remove = keys.empty() && texts.empty() && !notices;
        const auto name   = g_table_names.count(table) ? g_table_names[table] : table;
        const int n       = override_string_table(
            g_game, name,
            [&](uint16_t key, const std::wstring& t) {
                std::wstring s = t;
                if (auto k = keys.find(key); k != keys.end()) s = k->second;
                if (auto x = texts.find(s); x != texts.end()) s = x->second;
                if (is_main && key == kToStartKey) s = title_with_notices(g_azimuth, s);
                return s;
            },
            remove);
        if (mu) {
            ReleaseMutex(mu);
            CloseHandle(mu);
        }
        if (remove) {
            if (n) plog("removed " + name + " overrides (" + std::to_string(n) + " languages)");
            written.erase(table);
        } else {
            plog("wrote " + name + " for " + std::to_string(n) + " languages (" + std::to_string(keys.size()) +
                 " key edits, " + std::to_string(texts.size()) + " text replacements" +
                 (is_main && notices ? ", title notices" : "") + ")");
            written.insert(table);
        }
    }
    g_dirty.clear();
    std::ofstream out{manifest_path(), std::ios::trunc};
    for (const auto& t : written) out << (g_table_names.count(t) ? g_table_names[t] : t) << "\n";
}

void refresh_notice(FH2Mod* m) {
    std::vector<std::string> lines = m->notice;
    lines.insert(lines.end(), m->status.begin(), m->status.end());
    write_notice_file(g_azimuth, m->wname, m->dll, lines);
    g_dirty.insert("main.str");
    g_table_names.emplace("main.str", "Main.str");
}

// ---- the game ------------------------------------------------------------------

constexpr uint32_t kTimestamp   = 0x565501EDu; // forza_durango_release_final.exe, 24 Nov 2015
constexpr uint32_t kSizeOfImage = kGameSizeOfImage;

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

bool is_game(const char* base) {
    if (!readable(base, 0x1000)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 || dos->e_lfanew > 0x800) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt->FileHeader.TimeDateStamp == kTimestamp && nt->OptionalHeader.SizeOfImage == kSizeOfImage;
}

} // namespace

void* find_game() {
    static void* found = nullptr;
    if (found) return found;
    if (launched_game() == Launched::other) return nullptr; // not FH2: no point searching
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    auto addr       = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto last = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < last && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const auto next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase && mbi.RegionSize >= 0x1000 &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && is_game(static_cast<const char*>(mbi.BaseAddress)))
            return found = mbi.BaseAddress;
        if (next <= addr) break;
        addr = next;
    }
    return nullptr;
}

namespace {

// ---- the API ---------------------------------------------------------------------

std::vector<std::string> split_lines(const char* s) {
    std::vector<std::string> out;
    if (!s) return out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == '\n' || *p == '\0') {
            while (!cur.empty() && (cur.back() == '\r' || cur.back() == ' ')) cur.pop_back();
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else {
            cur += *p;
        }
    }
    return out;
}

void check_updates(FH2Mod* m) {
    bool cached = true;
    const auto latest = latest_release(g_azimuth, m->wname, m->repo, m->version, &cached);
    if (latest.empty()) {
        log_line(m->name, "couldn't check GitHub for updates this time");
        return;
    }
    std::scoped_lock lk{g_mu};
    m->latest        = latest;
    const bool newer = compare_versions(latest, m->version) > 0;
    const char* when = cached ? ", checked earlier" : "";
    log_line(m->name, newer ? "update available: " + latest + " (github.com/" + m->repo + "/releases)"
                      : compare_versions(latest, m->version) < 0
                          ? (cached ? "newer than the latest release from an earlier check (" + latest +
                                          "); couldn't reach GitHub this time"
                                    : "newer than the latest release on GitHub (" + latest + ")")
                          : "up to date (latest release " + latest + when + ")");
    const std::string line = m->name + " " + (latest[0] == 'v' || latest[0] == 'V' ? latest.substr(1) : latest) +
                             " is available";
    auto& st     = m->status;
    const auto it = std::find_if(st.begin(), st.end(), [](const std::string& s) {
        return s.size() > 13 && s.compare(s.size() - 13, 13, " is available") == 0;
    });
    const bool had = it != st.end();
    if (had) st.erase(it);
    if (newer) st.insert(st.begin(), line);
    if (newer || had) {
        refresh_notice(m);
        apply_all();
    }
}

// In the MODS tab's section below.
std::map<std::wstring, bool> online_choices();   // the player's "Load when playing online", per DLL (lower case)
bool remembered_offline_only(const std::wstring& dll);

FH2Mod* api_register(const FH2ModInfo* info) {
    if (!info || info->size < offsetof(FH2ModInfo, check_updates) + sizeof(int) || !info->name) return nullptr;
    auto m             = std::make_unique<FH2Mod>();
    m->name            = info->name;
    m->version         = info->version ? info->version : "";
    m->repo            = info->github_repo ? info->github_repo : "";
    m->platform_version = info->platform_version ? info->platform_version : "";
    m->check_updates   = info->check_updates != 0;
    if (info->size >= offsetof(FH2ModInfo, description) + sizeof(const char*) && info->description)
        m->description = info->description;   // API 6
    if (info->size >= offsetof(FH2ModInfo, offline_only) + sizeof(int))
        m->offline_only = info->offline_only != 0;   // API 9
    m->wname           = widen(m->name);
    FH2Mod* raw        = m.get();
    if (m->offline_only && g_online) {
        std::scoped_lock lk{g_mu};
        const auto choices = online_choices();
        const auto chosen  = choices.find(lower_w(g_current_dll));
        if (chosen == choices.end() || !chosen->second) {
            // Its author says it shouldn't run online, and the player hasn't said otherwise. It
            // isn't registered, so its fh2mod_init returns; it's remembered (platform-mods.txt),
            // so the next online launch doesn't load it at all.
            m->dll = g_current_dll;
            for (auto& f : g_found)
                if (lower_w(f.file) == lower_w(g_current_dll)) f.online_off = true;
            log_line(m->name, m->version + " doesn't run in online play (its author says so), so it isn't started; "
                                           "MODS > Installed mods has its \"Load when playing online\" switch");
            g_held.push_back(std::move(m));
            return nullptr;
        }
    }
    {
        std::scoped_lock lk{g_mu};
        m->dll = g_current_dll;
        if (!m->platform_version.empty() && compare_versions(m->platform_version, FH2MOD_PLATFORM_VERSION) > 0) {
            m->status.push_back("Old FH2 Mod Platform: reinstall " + m->name);
            log_line(m->name, "built for FH2 Mod Platform " + m->platform_version + ", but this is " +
                                  FH2MOD_PLATFORM_VERSION + "; reinstall it to get the platform it came with");
        }
        g_mods.push_back(std::move(m));
        refresh_notice(raw); // also clears a stale notice from last launch
    }
    log_line(raw->name, raw->version + " registered");
    if (!raw->repo.empty() && raw->check_updates) std::thread([raw] { check_updates(raw); }).detach();
    return raw;
}

void api_log(FH2Mod* m, const char* line) { log_line(m ? m->name : "?", line ? line : ""); }

void touch(const char* table) {
    const std::string t = lower(table);
    g_table_names.emplace(t, table);
    g_dirty.insert(t);
}

void api_set_string(FH2Mod* m, const char* table, uint16_t key, const wchar_t* text) {
    if (!m || !table) return;
    std::scoped_lock lk{g_mu};
    m->set[lower(table)][key] = text ? text : L"";
    touch(table);
}

void api_replace_string(FH2Mod* m, const char* table, const wchar_t* from, const wchar_t* to) {
    if (!m || !table || !from) return;
    std::scoped_lock lk{g_mu};
    m->repl[lower(table)][from] = to ? to : L"";
    touch(table);
}

void api_clear_strings(FH2Mod* m, const char* table) {
    if (!m) return;
    std::scoped_lock lk{g_mu};
    if (table) {
        m->set.erase(lower(table));
        m->repl.erase(lower(table));
        touch(table);
    } else {
        for (const auto& [t, _] : m->set) g_dirty.insert(t);
        for (const auto& [t, _] : m->repl) g_dirty.insert(t);
        m->set.clear();
        m->repl.clear();
    }
}

void api_apply_strings(FH2Mod*) { apply_all(); }

void api_set_notice(FH2Mod* m, const char* lines) {
    if (!m) return;
    std::scoped_lock lk{g_mu};
    m->notice = split_lines(lines);
    refresh_notice(m);
}

void* api_game_base() { return find_game(); }

void* api_wait_for_game(uint32_t timeout_ms) {
    const ULONGLONG until = timeout_ms == 0xFFFFFFFFu ? ~0ull : GetTickCount64() + timeout_ms;
    for (;;) {
        if (void* b = find_game()) return b;
        if (GetTickCount64() >= until || launched_game() == Launched::other) return nullptr;
        Sleep(100);
    }
}

// ---- API 2: files, database, other mods ------------------------------------------

int api_set_file(FH2Mod* m, const char* path, const void* data, size_t size) { return set_file(m, path, data, size); }
void api_clear_file(FH2Mod* m, const char* path) { clear_file(m, path); }

void api_add_sql(FH2Mod* m, const char* sql) {
    if (!m || !sql || !*sql) return;
    std::scoped_lock lk{g_mu};
    m->sql.emplace_back(sql);
}

void api_clear_sql(FH2Mod* m) {
    if (!m) return;
    std::scoped_lock lk{g_mu};
    m->sql.clear();
}

void build_database() {
    // Work from a snapshot, so mods can keep adding while it's built.
    std::vector<FH2Mod> snap;
    {
        std::scoped_lock lk{g_mu};
        for (const auto& m : g_mods) {
            FH2Mod c;
            c.name = m->name;
            c.sql  = m->sql;
            snap.push_back(std::move(c));
        }
    }
    std::vector<FH2Mod*> ptrs;
    for (auto& c : snap) ptrs.push_back(&c);
    apply_database(ptrs);
}

void api_apply_database(FH2Mod*) { build_database(); }

struct Interface {
    FH2Mod* owner;
    uint32_t version;
    const void* iface;
};
std::map<std::string, Interface> g_interfaces;
HANDLE g_mods_ready = nullptr;   // set once every fh2mod_init has returned
DWORD g_loader_thread = 0;

void api_publish_interface(FH2Mod* m, const char* name, uint32_t version, const void* iface) {
    if (!m || !name || !*name) return;
    std::scoped_lock lk{g_mu};
    auto it = g_interfaces.find(name);
    if (it != g_interfaces.end() && it->second.owner != m) {
        log_line(m->name, std::string("can't publish ") + name + ": " + it->second.owner->name + " already did");
        return;
    }
    if (!iface) {
        if (it != g_interfaces.end()) g_interfaces.erase(it);
        log_line(m->name, std::string("withdrew ") + name);
        return;
    }
    g_interfaces[name] = {m, version, iface};
    log_line(m->name, std::string("published ") + name + " v" + std::to_string(version));
}

const void* api_get_interface(const char* name, uint32_t min_version) {
    if (!name) return nullptr;
    std::scoped_lock lk{g_mu};
    const auto it = g_interfaces.find(name);
    return it != g_interfaces.end() && it->second.version >= min_version ? it->second.iface : nullptr;
}

int api_wait_for_mods(uint32_t timeout_ms) {
    if (GetCurrentThreadId() == g_loader_thread) {
        if (WaitForSingleObject(g_mods_ready, 0) == WAIT_OBJECT_0) return 1;
        log_line("", "a mod called wait_for_mods from fh2mod_init; call it from your own thread");
        return 0;
    }
    return g_mods_ready && WaitForSingleObject(g_mods_ready, timeout_ms) == WAIT_OBJECT_0;
}

const char* api_mod_version(const char* name) {
    if (!name) return nullptr;
    std::scoped_lock lk{g_mu};
    for (const auto& m : g_mods)
        if (lstrcmpiA(m->name.c_str(), name) == 0) return m->version.c_str();
    return nullptr;
}

// ---- loading mods --------------------------------------------------------------

int api_playing_online() { return g_online ? 1 : 0; }

// ---- online play (FH2MP) ---------------------------------------------------------
//
// FH2MP's launcher hands the game a session in data\bin\fh2mp-session.txt before starting it,
// and removes the file when the game ends (FH2MP's own DLL reads it and never deletes it, as
// Azimuth can start the game twice in one launch). It holds the player's token, so the platform
// only looks for it and never reads it. FH2MP_TOKEN in the environment does the same for FH2MP's
// tests. Either only counts while FH2MP.dll is in fh2-mods and turned on: FH2MP then plays
// online with it, even with a session file a crashed launcher left behind.
bool detect_online(const std::set<std::wstring>& off) {
    std::error_code ec;
    if (!fs::is_regular_file(g_azimuth / "fh2-mods" / "FH2MP.dll", ec) || off.count(L"fh2mp.dll")) return false;
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"FH2MP_TOKEN", env, 8) > 0) return true;
    return fs::is_regular_file(g_azimuth / "data" / "bin" / "fh2mp-session.txt", ec);
}

// Whether `dll` loads when playing online: the player's choice, else not if its author marked it
// offline-only at an earlier launch, else yes. FH2MP itself always does.
bool loads_online(const std::wstring& dll, const std::map<std::wstring, bool>& choices) {
    if (lower_w(dll) == L"fh2mp.dll") return true;
    if (const auto it = choices.find(lower_w(dll)); it != choices.end()) return it->second;
    return !remembered_offline_only(dll);
}

// Whether a DLL exports fh2mod_init, read from the file without loading it: 1 yes, 0 no, -1
// can't tell (then it's loaded as before).
int exports_init_in(const char* file, std::size_t size) {
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file);
        if (size < 0x400 || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 64 ||
            static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
            return -1;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return -1;
        const auto* sec    = IMAGE_FIRST_SECTION(nt);
        const unsigned n   = nt->FileHeader.NumberOfSections;
        auto at = [&](uint32_t rva) -> const char* { // the file offset of an RVA
            for (unsigned i = 0; i < n; ++i)
                if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].SizeOfRawData &&
                    sec[i].PointerToRawData + (rva - sec[i].VirtualAddress) < size)
                    return file + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
            return nullptr;
        };
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return 0;
        const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(at(dir.VirtualAddress));
        const auto* names = ed ? reinterpret_cast<const uint32_t*>(at(ed->AddressOfNames)) : nullptr;
        if (!names) return ed ? 0 : -1;
        for (DWORD i = 0; i < ed->NumberOfNames && i < 100000; ++i) {
            const char* name = at(names[i]);
            if (name && file + size - name > 12 && std::memcmp(name, "fh2mod_init", 12) == 0) return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int exports_init(const fs::path& dll) {
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size{};
    int found = -1;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31))
        if (HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            if (const void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) {
                found = exports_init_in(static_cast<const char*>(view), static_cast<std::size_t>(size.QuadPart));
                UnmapViewOfFile(view);
            }
            CloseHandle(map);
        }
    CloseHandle(f);
    return found;
}

int call_init(FH2ModInitFn init, const FH2ModApi* api, DWORD* code) {
    __try {
        return init(api);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

DWORD WINAPI load_mods(LPVOID) {
    g_loader_thread = GetCurrentThreadId();
    g_mods_ready    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::error_code ec;
    fs::create_directories(g_azimuth / "fh2-mods", ec);
    g_log = _wfopen((g_azimuth / "fh2-mods" / "platform.log").c_str(), L"w");
    {
        FILE* copy = start_session();
        std::scoped_lock lk{g_log_mu};
        g_log_copy = copy;
    }
    plog(std::string("FH2 Mod Platform ") + FH2MOD_PLATFORM_VERSION + " (API " + std::to_string(FH2MOD_API_VERSION) +
         ") in " + narrow(g_azimuth.wstring()));
    plog("loaded as " + narrow(g_self_name) + (g_copies.empty() ? "" : "; " + g_copies));
    plog(session_line());
    plog(g_game.empty() ? "couldn't find the game's Mount folder; game text, file and database changes are off"
                        : "game folder " + narrow(g_game.wstring()));
    // Azimuth 1.0.1 runs other Forza games too. Told to start one of those straight away, the
    // platform does nothing at all.
    if (const std::string other = other_game_on_command_line(); !other.empty()) {
        plog("Azimuth was started for " + other + ", not Forza Horizon 2: no mods are loaded, and the platform "
             "leaves the game alone");
        SetEvent(g_mods_ready);
        return 0;
    }
    start_media_overrides();

    struct Loaded {
        std::wstring file;
        HMODULE mod;
    };
    std::vector<Loaded> loaded;
    std::vector<std::wstring> legacy; // mods from before the platform, loaded once the game is FH2
    std::vector<std::wstring> files;
    for (const auto& e : fs::directory_iterator(g_azimuth / "fh2-mods", ec))
        if (e.is_regular_file(ec) && lower(narrow(e.path().extension().wstring())) == ".dll")
            files.push_back(e.path().filename().wstring());
    std::sort(files.begin(), files.end());
    std::vector<LoadedDll> dlls;
    const auto off = disabled_mods();
    g_online       = detect_online(off);
    const auto online_choice = online_choices();
    if (g_online) plog("started from the FH2MP launcher (online play): mods set to stay off online aren't loaded");
    // A DLL not loaded because the game is online: noted for the MODS tab.
    auto stays_off_online = [&](const std::wstring& f) {
        if (!g_online || loads_online(f, online_choice)) return false;
        plog(narrow(f) + " is set to stay off when playing online; not loaded");
        std::scoped_lock lk{g_mu};
        g_found.back().online_off = true;
        return true;
    };
    // fh2wheel (github.com/iamAzoDx/fh2wheel, racing wheels with force feedback) ships its own
    // d3dcompiler_47.dll, a loader that does nothing but forward to the system compiler and load
    // fh2wheel.dll from the Azimuth folder. Only one d3dcompiler_47.dll can sit next to azimuth.exe,
    // so with the platform's there, the platform loads fh2wheel.dll the same way: as it is, before
    // the mods, from the Azimuth folder. It's listed on the MODS tab and can be turned off there.
    {
        const std::wstring wheel = L"fh2wheel.dll";
        const auto path          = g_azimuth / wheel;
        if (fs::is_regular_file(path, ec)) {
            {
                std::scoped_lock lk{g_mu};
                g_found.push_back({wheel, false, off.count(lower_w(wheel)) != 0, false});
            }
            if (off.count(lower_w(wheel))) {
                plog("fh2wheel.dll (next to azimuth.exe) is turned off on the MODS tab; not loaded");
            } else if (stays_off_online(wheel)) {
            } else if (GetModuleHandleW(path.c_str())) {
                plog("fh2wheel.dll is already loaded (by fh2wheel's own loader?)");
                std::scoped_lock lk{g_mu};
                g_found.back().loaded = true;
            } else if (HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                {
                    std::scoped_lock lk{g_mu};
                    g_found.back().loaded = true;
                }
                const auto* base = reinterpret_cast<const char*>(h);
                const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
                dlls.push_back({wheel, base, nt->OptionalHeader.SizeOfImage});
                const auto v = version_string(h);
                plog("loaded fh2wheel.dll" + (v.empty() ? std::string() : " " + v) +
                     " from next to azimuth.exe (fh2wheel's own loader is replaced by the platform's d3dcompiler_47.dll)");
            } else {
                const DWORD err = GetLastError();
                plog("FAILED to load fh2wheel.dll (error " + std::to_string(err) + ")");
                std::scoped_lock lk{g_mu};
                g_found.back().problem = "Windows couldn't load it (error " + std::to_string(err) + ").";
            }
        }
    }
    for (const auto& f : files) {
        {
            std::scoped_lock lk{g_mu};
            g_found.push_back({f, false, off.count(lower_w(f)) != 0, false});
        }
        if (off.count(lower_w(f))) {
            plog(narrow(f) + " is turned off on the MODS tab; not loaded");
            continue;
        }
        if (stays_off_online(f)) continue;
        // ALTERED_SEARCH_PATH: a mod's own dependencies resolve from fh2-mods first.
        const auto path = (g_azimuth / "fh2-mods" / f).wstring();
        // A mod from before the platform changes the game by itself, so it waits until the game
        // Azimuth starts is known to be FH2 (Azimuth 1.0.1 runs other Forza games too).
        if (exports_init(path) == 0) {
            plog(narrow(f) + " has no fh2mod_init (a mod from before the platform); it runs on its own, and "
                 "loads once the game Azimuth starts is known to be Forza Horizon 2");
            legacy.push_back(f);
            std::scoped_lock lk{g_mu};
            g_found.back().loaded = true; // as far as the MODS tab goes, which only FH2 shows
            continue;
        }
        if (HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            loaded.push_back({f, h});
            {
                std::scoped_lock lk{g_mu};
                g_found.back().loaded   = true;
                g_found.back().has_init = GetProcAddress(h, "fh2mod_init") != nullptr;
            }
            const auto* base = reinterpret_cast<const char*>(h);
            const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            dlls.push_back({f, base, nt->OptionalHeader.SizeOfImage});
            const auto v = version_string(h);
            plog("loaded " + narrow(f) + (v.empty() ? "" : " " + v));
        } else {
            const DWORD err = GetLastError();
            plog("FAILED to load " + narrow(f) + " (error " + std::to_string(err) + ")");
            std::scoped_lock lk{g_mu};
            g_found.back().problem = "Windows couldn't load it (error " + std::to_string(err) + ").";
        }
    }
    if (files.empty()) plog("no mods in fh2-mods");
    {
        // The platform itself, so a crash in it is named too.
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&load_mods), &self);
        if (self) {
            const auto* base = reinterpret_cast<const char*>(self);
            const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
            dlls.push_back({g_self_name + L" (FH2 Mod Platform)", base, nt->OptionalHeader.SizeOfImage});
        }
    }
    install_crash_handler(dlls);
    watch_launched_game(
        [legacy] {
            for (const auto& f : legacy) {
                const auto path = (g_azimuth / "fh2-mods" / f).wstring();
                if (HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                    const auto* base = reinterpret_cast<const char*>(h);
                    const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                        base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
                    add_crash_dll({f, base, nt->OptionalHeader.SizeOfImage});
                    const auto v = version_string(h);
                    plog("loaded " + narrow(f) + (v.empty() ? "" : " " + v) + " (a mod from before the platform)");
                } else {
                    const DWORD err = GetLastError();
                    plog("FAILED to load " + narrow(f) + " (error " + std::to_string(err) + ")");
                    std::scoped_lock lk{g_mu};
                    for (auto& e : g_found)
                        if (e.file == f) e.problem = "Windows couldn't load it (error " + std::to_string(err) + ").";
                }
            }
        },
        [legacy] {
            for (const auto& f : legacy)
                plog(narrow(f) + " isn't loaded: it's a mod from before the platform, which changes the game by "
                     "itself");
        });
    std::thread([] {
        // Azimuth sets up more of itself when Play is pressed.
        if (api_wait_for_game(0xFFFFFFFFu)) {
            Sleep(10000);
            recheck_crash_handler();
        }
    }).detach();

    for (const auto& l : loaded) {
        auto init = reinterpret_cast<FH2ModInitFn>(GetProcAddress(l.mod, "fh2mod_init"));
        if (!init) {
            plog(narrow(l.file) + " has no fh2mod_init (a mod from before the platform); it runs on its own");
            continue;
        }
        {
            std::scoped_lock lk{g_mu};
            g_current_dll = l.file;
        }
        DWORD code   = 0;
        const int rc = call_init(init, &g_api, &code);
        {
            // register_mod turned it away (offline-only, and the game is online): that's not a problem
            std::scoped_lock lk{g_mu};
            bool held = false;
            for (const auto& f : g_found)
                if (lower_w(f.file) == lower_w(l.file)) held = f.online_off;
            if (held && !code) {
                g_current_dll.clear();
                continue;
            }
        }
        std::string problem;
        if (code) {
            char b[16];
            std::snprintf(b, sizeof b, "%08lX", code);
            plog(narrow(l.file) + ": fh2mod_init crashed (exception 0x" + b + ")");
            problem = std::string("It crashed while starting (exception 0x") + b + ").";
        } else if (rc) {
            plog(narrow(l.file) + ": fh2mod_init returned " + std::to_string(rc));
            problem = "It reported a problem while starting (it returned " + std::to_string(rc) + ").";
        }
        std::scoped_lock lk{g_mu};
        g_current_dll.clear();
        if (!problem.empty())
            for (auto& f : g_found)
                if (lower_w(f.file) == lower_w(l.file)) f.problem = problem;
    }

    // First write of everything edited so far, and the title notices.
    {
        std::scoped_lock lk{g_mu};
        g_dirty.insert("main.str");
        g_table_names.emplace("main.str", "Main.str");
    }
    apply_all();
    build_database();
    settings_start(&g_api);
    plog(std::to_string(loaded.size()) + " mod(s) loaded");
    start_save_snapshot();
    write_session_status();
    SetEvent(g_mods_ready);

    // Tables written last time that no mod has edited by now are leftovers
    // (a mod removed, or a feature turned off): clean them up. Wait a little
    // first, since mods often make their edits from their own threads just
    // after starting.
    Sleep(15000);
    {
        std::scoped_lock lk{g_mu};
        for (const auto& t : read_manifest()) {
            bool edited = false;
            for (const auto& m : g_mods) edited = edited || m->set.count(t) || m->repl.count(t);
            if (!edited && t != "main.str") g_dirty.insert(t);
        }
    }
    apply_all();
    {
        std::vector<FH2Mod*> mods;
        std::scoped_lock lk{g_mu};
        for (const auto& m : g_mods) mods.push_back(m.get());
        clean_leftover_files(mods);
    }
    return 0;
}

} // namespace

// ---- the MODS tab's list of mods -----------------------------------------------------
//
// fh2-mods\platform-disabled.txt lists the DLLs turned off on the MODS tab, one per line; the
// loader skips them. fh2-mods\platform-mods.txt remembers each mod's name, version and
// description from the last launch it was loaded, so a turned-off mod still shows by name.

// fh2-mods\platform-online.txt (platform 2.8.0) holds the player's "Load when playing online" for
// the mods they've set it on, as "<DLL><tab>1" or "0"; the others follow their author's
// FH2ModInfo.offline_only, which platform-mods.txt remembers for the mods that aren't loaded.

namespace {
fs::path disabled_path() { return g_azimuth / "fh2-mods" / "platform-disabled.txt"; }
fs::path known_path() { return g_azimuth / "fh2-mods" / "platform-mods.txt"; }
fs::path online_path() { return g_azimuth / "fh2-mods" / "platform-online.txt"; }

std::vector<std::wstring> read_lines(const fs::path& p) {
    std::vector<std::wstring> out;
    FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8");
    if (!f) return out;
    wchar_t line[4096];
    while (std::fgetws(line, 4096, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        if (!l.empty() && l[0] != L'#') out.push_back(l);
    }
    std::fclose(f);
    return out;
}

void write_lines(const fs::path& p, const std::wstring& header, const std::vector<std::wstring>& lines) {
    const fs::path tmp = p.wstring() + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    std::fputws(header.c_str(), f);
    for (const auto& l : lines) {
        std::fputws(l.c_str(), f);
        std::fputws(L"\n", f);
    }
    std::fclose(f);
    MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::vector<std::wstring> split_tabs(const std::wstring& l) {
    std::vector<std::wstring> out(1);
    for (wchar_t c : l)
        if (c == L'\t') out.emplace_back();
        else out.back() += c;
    return out;
}

std::map<std::wstring, bool> online_choices() {
    std::map<std::wstring, bool> out;
    for (const auto& l : read_lines(online_path())) {
        const auto f = split_tabs(l);
        if (f.size() >= 2 && !f[0].empty()) out[lower_w(f[0])] = f[1] != L"0";
    }
    return out;
}

// platform-mods.txt's sixth column: 1 when the mod's author marked it offline-only
bool remembered_offline_only(const std::wstring& dll) {
    for (const auto& l : read_lines(known_path())) {
        const auto f = split_tabs(l);
        if (f.size() > 5 && lower_w(f[0]) == lower_w(dll)) return f[5] == L"1";
    }
    return false;
}
} // namespace

bool playing_online() { return g_online; }

bool fh2mp_installed() {
    std::error_code ec;
    return fs::is_regular_file(g_azimuth / "fh2-mods" / "FH2MP.dll", ec);
}

void set_mod_online(const std::wstring& dll, bool on) {
    auto choices = online_choices();
    choices[lower_w(dll)] = on;
    std::vector<std::wstring> lines;
    for (const auto& [d, v] : choices) lines.push_back(d + L"\t" + (v ? L"1" : L"0"));
    write_lines(online_path(),
                L"# The mods' \"Load when playing online\" on MODS > Installed mods: 1 loads it when the game is started from the "
                L"FH2MP launcher, 0 doesn't.\n",
                lines);
    log_line("", narrow(dll) + (on ? " now loads" : " now stays off") +
                     " when playing online (MODS tab); takes effect the next time the game starts from the FH2MP launcher");
}

std::set<std::wstring> disabled_mods() {
    std::set<std::wstring> out;
    for (const auto& l : read_lines(disabled_path())) out.insert(lower_w(l));
    return out;
}

void set_mod_enabled(const std::wstring& dll, bool on) {
    auto off = disabled_mods();
    if (on) off.erase(lower_w(dll));
    else off.insert(lower_w(dll));
    write_lines(disabled_path(), L"# DLLs in fh2-mods turned off on the MODS tab; the platform doesn't load them.\n",
                std::vector<std::wstring>(off.begin(), off.end()));
    log_line("", narrow(dll) + (on ? " turned back on" : " turned off") + " on the MODS tab; takes effect the next time the game starts");
}

std::string latest_known(const FH2Mod* m) {
    std::scoped_lock lk{g_mu};
    return m ? m->latest : std::string();
}

std::vector<ModEntry> mod_entries() {
    // remembered: dll -> name, version, description
    std::map<std::wstring, std::vector<std::wstring>> known;
    for (const auto& l : read_lines(known_path())) {
        auto f = split_tabs(l);
        if (f.size() >= 3) known[lower_w(f[0])] = f;
    }
    std::vector<ModEntry> out;
    std::vector<std::wstring> remember;
    const auto choices = online_choices();
    std::scoped_lock lk{g_mu};
    for (const auto& f : g_found) {
        ModEntry e;
        e.dll          = f.file;
        e.enabled      = !f.disabled;
        e.problem      = f.problem;
        e.platform_mod = f.has_init;
        e.online_off   = f.online_off;
        bool told      = false;   // registered this launch, so its offline_only is known
        for (const auto* list : {&g_mods, &g_held})
            for (const auto& m : *list)
                if (!told && lower_w(m->dll) == lower_w(f.file)) {
                    e.mod          = list == &g_mods ? m.get() : nullptr;   // a held mod isn't running
                    e.name         = m->name;
                    e.version      = m->version;
                    e.description  = m->description;
                    e.repo         = m->repo;
                    e.offline_only = m->offline_only;
                    e.platform_mod = true;
                    told           = true;
                }
        if (!e.mod && lower_w(f.file) == L"fh2wheel.dll") {
            e.name        = "fh2wheel";
            e.description = "Racing wheels with force feedback, by iamAzoDx. Loaded by the platform from next to azimuth.exe, "
                            "in place of fh2wheel's own d3dcompiler_47.dll.";
            e.repo        = "iamAzoDx/fh2wheel";
        } else if (!told) {
            if (auto it = known.find(lower_w(f.file)); it != known.end()) {
                e.name    = narrow(it->second[1]);
                e.version = narrow(it->second[2]);
                if (it->second.size() > 3) e.description = narrow(it->second[3]);
                if (it->second.size() > 4) e.repo = narrow(it->second[4]);
                if (it->second.size() > 5) e.offline_only = it->second[5] == L"1";
                e.platform_mod = true;
            }
        }
        if (const auto c = choices.find(lower_w(f.file)); c != choices.end()) e.loads_online = c->second;
        else e.loads_online = !e.offline_only;
        if (lower_w(f.file) == L"fh2mp.dll") e.loads_online = true;
        // nothing told us its name: its file's name, without .dll, cut short if it's long
        if (e.name.find_first_not_of(" \t") == std::string::npos) {
            std::wstring stem = f.file;
            if (stem.size() > 4 && lower_w(stem.substr(stem.size() - 4)) == L".dll") stem.resize(stem.size() - 4);
            e.name = narrow(stem);
            if (e.name.size() > 28) {
                std::size_t cut = 25;
                while (cut > 0 && (static_cast<unsigned char>(e.name[cut]) & 0xC0) == 0x80) --cut;  // not mid-letter
                e.name = e.name.substr(0, cut) + "...";
            }
        }
        if (e.mod || e.platform_mod)
            remember.push_back(f.file + L"\t" + widen(e.name) + L"\t" + widen(e.version) + L"\t" + widen(e.description) + L"\t" +
                               widen(e.repo) + L"\t" + (e.offline_only ? L"1" : L"0"));
        out.push_back(e);
    }
    write_lines(known_path(),
                L"# What the MODS tab remembers about each mod (DLL, name, version, description, GitHub repo, offline only).\n",
                remember);
    return out;
}

} // namespace fh2platform

// Which copy of the platform runs when Azimuth loads two: see this_copy_runs.
extern "C" __declspec(dllexport) uint32_t fh2mod_platform_version() { return fh2platform::packed_version(); }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    using namespace fh2platform;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    wchar_t self[MAX_PATH], exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(instance, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    // Only act inside Azimuth (or Mezcal's game exe), never in other programs
    // that happen to load this DLL from this folder.
    const DWORD m       = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* base = exe;
    for (DWORD i = 0; i < m; ++i)
        if (exe[i] == L'\\' || exe[i] == L'/') base = exe + i + 1;
    if (lstrcmpiW(base, L"azimuth.exe") && lstrcmpiW(base, L"forza_durango_release_final.exe")) return TRUE;

    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    g_azimuth   = fs::path{self}.parent_path();
    g_self      = instance;
    g_self_name = shipped_name(fs::path{self}.filename().wstring());
    // Nothing below may load DLLs or touch the file system much: still
    // under the loader lock. The rest happens on the thread.
    if (HANDLE t = CreateThread(
            nullptr, 0,
            [](LPVOID) -> DWORD {
                // Another copy of the platform may be loaded too (Azimuth 1.0.1 imports both names).
                if (!this_copy_runs()) return 0;
                // azimuth.exe --game=DIR starts the game in DIR, when that's FH2.
                const fs::path started = command_line_game_folder();
                g_game         = find_game_mount(g_azimuth, holds_fh2_exe(started) ? started : fs::path{});
                g_azimuth_str  = g_azimuth.wstring();
                g_game_str     = g_game.wstring();
                g_api.size     = sizeof(FH2ModApi);
                g_api.api_version      = FH2MOD_API_VERSION;
                g_api.platform_version = FH2MOD_PLATFORM_VERSION;
                g_api.azimuth_dir      = g_azimuth_str.c_str();
                g_api.game_dir         = g_game_str.c_str();
                g_api.register_mod     = api_register;
                g_api.log              = api_log;
                g_api.set_string       = api_set_string;
                g_api.replace_string   = api_replace_string;
                g_api.clear_strings    = api_clear_strings;
                g_api.apply_strings    = api_apply_strings;
                g_api.set_notice       = api_set_notice;
                g_api.game_base        = api_game_base;
                g_api.wait_for_game    = api_wait_for_game;
                g_api.set_file          = api_set_file;
                g_api.clear_file        = api_clear_file;
                g_api.add_sql           = api_add_sql;
                g_api.clear_sql         = api_clear_sql;
                g_api.apply_database    = api_apply_database;
                g_api.publish_interface = api_publish_interface;
                g_api.get_interface     = api_get_interface;
                g_api.wait_for_mods     = api_wait_for_mods;
                g_api.mod_version       = api_mod_version;
                fill_hook_api(g_api);
                g_api.add_setting       = settings_add;
                g_api.get_setting       = settings_get;
                g_api.set_setting       = settings_set;
                g_api.get_slider        = settings_get_slider;
                g_api.set_slider        = settings_set_slider;
                g_api.add_page          = pages_add;
                g_api.set_page_rows     = pages_set_rows;
                g_api.set_page_xaml     = pages_set_xaml;
                g_api.get_page_slot     = pages_get_slot;
                g_api.set_page_slot     = pages_set_slot;
                g_api.focus_page_element = pages_focus;
                g_api.set_page_tile_text = pages_set_tile_text;
                g_api.set_page_tile_colors = pages_set_tile_colors;
                g_api.add_title_entry   = title_add;
                g_api.title_started_by  = title_started_by;
                g_api.playing_online    = api_playing_online;
                g_api.session_id        = session_id;
                g_api.session_log_path  = session_log_path;
                g_api.request_save_snapshot = request_save_snapshot;
                g_api.add_dump_memory   = add_dump_memory;
                g_api.add_hotkey        = hotkey_add;
                g_api.add_command       = command_add;
                return load_mods(nullptr);
            },
            nullptr, 0, nullptr))
        CloseHandle(t);
    return TRUE;
}

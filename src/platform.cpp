// FH2 Mod Platform -- d3dcompiler_47.dll proxy for Azimuth.
//
// azimuth.exe statically imports d3dcompiler_47.dll (for D3DCompile) and
// Windows looks next to the exe first, so this DLL gets loaded at startup
// without touching any of Azimuth's own files, and without fighting Forza
// Horizon 2 Extra Options (which owns winmm.dll). Every D3DCompiler export
// is a PE forwarder to the real System32 DLL (d3dcompiler_47.def for
// llvm-mingw, msvc_exports.inc for MSVC), so shader compiles never touch it.
//
// On a thread of its own (outside the loader lock), it loads every DLL in
// fh2-mods and calls each one's fh2mod_init with the API in fh2mod.h:
// string-table edits merged across mods, title-screen notices, update
// checks, logging and finding the game. Mods without fh2mod_init (built
// before the platform) still load; they just start themselves from DllMain.
// Everything is logged to fh2-mods\platform.log.

#include <windows.h>

#include "platform.hpp"
#include "strings.hpp"
#include "updates.hpp"

#ifdef _MSC_VER
#include "msvc_exports.inc"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
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
};
std::vector<Found> g_found;

std::mutex g_log_mu;
FILE* g_log = nullptr;

FH2ModApi g_api{};

} // namespace

// ---- log -----------------------------------------------------------------------

void log_line(const std::string& who, const std::string& text) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::scoped_lock lk{g_log_mu};
    if (!g_log) return;
    std::fprintf(g_log, "%02u:%02u:%02u.%03u %s%s%s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                 who.empty() ? "" : "[", who.c_str(), who.empty() ? "" : "] ");
    std::fputs(text.c_str(), g_log);
    std::fputc('\n', g_log);
    std::fflush(g_log);
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
    m->wname           = widen(m->name);
    FH2Mod* raw        = m.get();
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
        if (GetTickCount64() >= until) return nullptr;
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
    plog(std::string("FH2 Mod Platform ") + FH2MOD_PLATFORM_VERSION + " (API " + std::to_string(FH2MOD_API_VERSION) +
         ") in " + narrow(g_azimuth.wstring()));
    plog(g_game.empty() ? "couldn't find the game's Mount folder; game text, file and database changes are off"
                        : "game folder " + narrow(g_game.wstring()));

    struct Loaded {
        std::wstring file;
        HMODULE mod;
    };
    std::vector<Loaded> loaded;
    std::vector<std::wstring> files;
    for (const auto& e : fs::directory_iterator(g_azimuth / "fh2-mods", ec))
        if (e.is_regular_file(ec) && lower(narrow(e.path().extension().wstring())) == ".dll")
            files.push_back(e.path().filename().wstring());
    std::sort(files.begin(), files.end());
    std::vector<LoadedDll> dlls;
    const auto off = disabled_mods();
    for (const auto& f : files) {
        {
            std::scoped_lock lk{g_mu};
            g_found.push_back({f, false, off.count(lower_w(f)) != 0, false});
        }
        if (off.count(lower_w(f))) {
            plog(narrow(f) + " is turned off on the MODS tab; not loaded");
            continue;
        }
        // ALTERED_SEARCH_PATH: a mod's own dependencies resolve from fh2-mods first.
        const auto path = (g_azimuth / "fh2-mods" / f).wstring();
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
            plog("FAILED to load " + narrow(f) + " (error " + std::to_string(GetLastError()) + ")");
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
            dlls.push_back({L"d3dcompiler_47.dll (FH2 Mod Platform)", base, nt->OptionalHeader.SizeOfImage});
        }
    }
    install_crash_handler(dlls);
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
        if (code)
            plog(narrow(l.file) + ": fh2mod_init crashed (exception 0x" + [&] {
                char b[16];
                std::snprintf(b, sizeof b, "%08lX", code);
                return std::string{b};
            }() + ")");
        else if (rc)
            plog(narrow(l.file) + ": fh2mod_init returned " + std::to_string(rc));
        std::scoped_lock lk{g_mu};
        g_current_dll.clear();
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

namespace {
fs::path disabled_path() { return g_azimuth / "fh2-mods" / "platform-disabled.txt"; }
fs::path known_path() { return g_azimuth / "fh2-mods" / "platform-mods.txt"; }

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
} // namespace

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
    std::scoped_lock lk{g_mu};
    for (const auto& f : g_found) {
        ModEntry e;
        e.dll          = f.file;
        e.enabled      = !f.disabled;
        e.platform_mod = f.has_init;
        for (const auto& m : g_mods)
            if (lower_w(m->dll) == lower_w(f.file)) {
                e.mod         = m.get();
                e.name        = m->name;
                e.version     = m->version;
                e.description = m->description;
                e.repo        = m->repo;
                e.platform_mod = true;
                break;
            }
        if (!e.mod) {
            if (auto it = known.find(lower_w(f.file)); it != known.end()) {
                e.name    = narrow(it->second[1]);
                e.version = narrow(it->second[2]);
                if (it->second.size() > 3) e.description = narrow(it->second[3]);
                if (it->second.size() > 4) e.repo = narrow(it->second[4]);
                e.platform_mod = true;
            } else {
                e.name = narrow(f.file.substr(0, f.file.size() > 4 ? f.file.size() - 4 : f.file.size()));
            }
        }
        if (e.mod || e.platform_mod)
            remember.push_back(f.file + L"\t" + widen(e.name) + L"\t" + widen(e.version) + L"\t" + widen(e.description) + L"\t" +
                               widen(e.repo));
        out.push_back(e);
    }
    write_lines(known_path(), L"# What the MODS tab remembers about each mod (DLL, name, version, description, GitHub repo).\n", remember);
    return out;
}

} // namespace fh2platform

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    using namespace fh2platform;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
    wchar_t self[MAX_PATH], exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(instance, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    // Only act inside Azimuth (or Mezcal's game exe), never in other programs
    // that happen to load d3dcompiler_47.dll from this folder.
    const DWORD m       = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* base = exe;
    for (DWORD i = 0; i < m; ++i)
        if (exe[i] == L'\\' || exe[i] == L'/') base = exe + i + 1;
    if (lstrcmpiW(base, L"azimuth.exe") && lstrcmpiW(base, L"forza_durango_release_final.exe")) return TRUE;

    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    g_azimuth = fs::path{self}.parent_path();
    // Nothing below may load DLLs or touch the file system much: still
    // under the loader lock. The rest happens on the thread.
    if (HANDLE t = CreateThread(
            nullptr, 0,
            [](LPVOID) -> DWORD {
                g_game         = find_game_mount(g_azimuth);
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
                return load_mods(nullptr);
            },
            nullptr, 0, nullptr))
        CloseHandle(t);
    return TRUE;
}

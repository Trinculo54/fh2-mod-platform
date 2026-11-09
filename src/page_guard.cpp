// Pages of a mod's own design, made safe to open (platform 2.9.0); see page_guard.hpp.
//
// The crash guard's file, fh2-mods\platform-pages.txt (format in xaml_safety.cpp, guard_text), is written by the
// game's UI thread as a page opens: CreateFileW and WriteFile only, small, and not flushed (a crash of the game
// doesn't lose what Windows has been given). The mark is taken off once the page has been up for 10 s, or as soon as
// the player leaves it, so a session that ends any other way leaves nothing behind. A mark found at the next start
// leaves the page out only by the HUD layer's crash marker's rule: a crash report in fh2-mods\crashes newer than the
// mark, or the page left opening in two sessions running (the player closing Azimuth's window as a page opens leaves
// a mark too, and that alone isn't a crash).
#include "page_guard.hpp"

#include "platform.hpp"
#include "strings.hpp"
#include "updates.hpp"
#include "xaml_safety.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::string lower_slash(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return s;
}

// ---- the game's files, read once ----

std::mutex g_check_mu;   // the check runs under it, so its lookups need no lock of their own
bool g_tried = false;
bool g_have  = false;    // the keys were read
AppKeys g_app;
std::map<std::string, std::optional<std::set<std::string>>> g_zips;   // a texture zip (lower case) -> its entries, or none

void load_keys() {   // under g_check_mu
    g_tried                 = true;
    const ULONGLONG started = GetTickCount64();
    const fs::path mount    = game_mount();
    if (mount.empty()) {
        plog("set_page_xaml: the game folder wasn't found, so pages of a mod's own design aren't checked (or shown)");
        return;
    }
    const fs::path zip = mount / L"media" / L"UI.zip";
    std::ifstream in{zip, std::ios::binary};
    const std::vector<char> ui((std::istreambuf_iterator<char>(in)), {});
    std::string why;
    g_have = load_app_keys(
        [&](const std::string& name) {
            const std::vector<char> d = unzip_from(ui, name);
            return std::string(d.begin(), d.end());
        },
        g_app, why);
    if (!g_have) {
        plog("set_page_xaml: pages of a mod's own design aren't checked this session, because " + why + " (" + narrow(zip.wstring()) + ")");
        return;
    }
    plog("set_page_xaml: pages of a mod's own design are checked against " + std::to_string(g_app.keys.size()) + " resource keys from UI.zip's " +
         std::to_string(g_app.dictionaries) + " application dictionaries and the game's texture zips (read in " +
         std::to_string(GetTickCount64() - started) + " ms)");
}

// The media folders the game reads, in its order: this Azimuth folder's own layer or mediaoverride (override_root),
// the car packs (on the temp drive, with the layer), mediaoverride, mediapc, then media itself.
bool has_loose(const TextureSpot& s) {
    const fs::path mount = game_mount();
    if (mount.empty()) return false;
    std::vector<fs::path> layers{override_root()};
    if (instance_layer())
        for (const CarPack& p : car_packs()) layers.push_back(p.mirror);
    for (const wchar_t* m : {L"mediaoverride", L"mediapc", L"media"}) layers.push_back(mount / m);
    const std::wstring rel = widen(loose_texture_path(s));
    for (const fs::path& l : layers) {
        if (l.empty()) continue;
        const DWORD a = GetFileAttributesW((l / rel).c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return true;
    }
    return false;
}

int texture(const TextureSpot& s) {   // under g_check_mu
    const std::string zip = lower_slash(s.zip);
    auto it               = g_zips.find(zip);
    if (it == g_zips.end()) {
        std::vector<std::string> names;
        std::optional<std::set<std::string>> entries;
        if (zip_names(game_mount() / L"media" / L"ui" / L"textures" / widen(s.zip), names)) {
            entries.emplace();
            for (const std::string& n : names) entries->insert(lower_slash(n));
        }
        it = g_zips.emplace(zip, std::move(entries)).first;
    }
    if (it->second && it->second->count(lower_slash(s.entry))) return 1;
    // not in the zip: the game also reads it as a loose file from any media folder it reads (a mod's set_file, a car
    // pack, mediaoverride, or a file the game ships loose), so look in each, as it is now (case doesn't matter there)
    if (has_loose(s)) return 1;
    return it->second ? 0 : -1;
}

// ---- the crash guard ----

std::mutex g_guard_mu;
std::vector<GuardEntry> g_guard;   // this session's entries, as the file has them
bool g_guard_warned = false;       // the file couldn't be written, said once
unsigned g_opening  = 0;           // counts the pages opened, so a page's 10 s don't clear a later page's mark
std::vector<GuardEntry> g_carried; // pages left opening last session with nothing to show a crash: a mark now counts them

// The newest crash-*.txt in fh2-mods\crashes, as the HUD layer's crash marker looks for it.
CrashReport newest_crash() {
    CrashReport r;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((azimuth_dir() / L"fh2-mods" / L"crashes" / L"crash-*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    do {
        const std::uint64_t t = (static_cast<std::uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && t > r.time) {
            r.time = t;
            r.name = narrow(fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return r;
}

std::uint64_t file_time(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
}

std::uint64_t now_utc() {
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

fs::path guard_path() { return azimuth_dir() / L"fh2-mods" / L"platform-pages.txt"; }

// A DLL's size and time in fh2-mods: "<bytes>-<FILETIME in hex>", or "" if it isn't there.
std::string dll_stamp(const std::string& dll) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (dll.empty() || !GetFileAttributesExW((azimuth_dir() / L"fh2-mods" / widen(dll)).c_str(), GetFileExInfoStandard, &a)) return "";
    char b[64];
    std::snprintf(b, sizeof b, "%llu-%08lX%08lX", (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
                  a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
    return b;
}

std::string today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[16];
    std::snprintf(b, sizeof b, "%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    return b;
}

bool same_dll(const std::string& a, const std::string& b) { return lower_slash(a) == lower_slash(b); }

void write_guard() {   // under g_guard_mu
    const fs::path p = guard_path();
    if (g_guard.empty()) {
        DeleteFileW(p.c_str());
        return;
    }
    const std::string text = guard_text(g_guard);
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w  = 0;
    const bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok && !g_guard_warned) {
        g_guard_warned = true;
        plog("pages: fh2-mods\\platform-pages.txt couldn't be written (error " + std::to_string(GetLastError()) +
             "), so a page that crashes the game as it opens isn't left out next time");
    }
}

void clear_marks() {   // under g_guard_mu
    const std::size_t before = g_guard.size();
    std::erase_if(g_guard, [](const GuardEntry& e) { return e.state == "opening"; });
    if (g_guard.size() != before) write_guard();
}

} // namespace

bool page_fragment_ok(const char* resources, const char* content, const char* help, std::string& why) {
    std::scoped_lock lk{g_check_mu};
    if (!g_tried) load_keys();
    if (!g_have) return true;
    FragmentCheck c;
    return check_page_fragment(g_app, resources ? resources : "", content ? content : "", help ? help : "", texture, c, why);
}

void page_guard_start() {
    std::string text;
    {
        std::ifstream in{guard_path(), std::ios::binary};
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    std::vector<std::string> lines;
    std::vector<GuardEntry> carried;
    const std::vector<GuardEntry> last = parse_guard(text);
    std::vector<GuardEntry> now        = guard_at_start(last, dll_stamp, newest_crash(), file_time(guard_path()), carried, lines);
    for (const std::string& l : lines) plog("pages: " + l);
    std::scoped_lock lk{g_guard_mu};
    g_guard   = std::move(now);
    g_carried = std::move(carried);
    if (!text.empty()) write_guard();
}

bool page_guard_skipped(const FH2Mod* mod, const std::string& page) {
    if (!mod) return false;
    const std::string dll = narrow(mod->dll);
    std::scoped_lock lk{g_guard_mu};
    for (const GuardEntry& e : g_guard)
        if (e.state == "skip" && e.page == page && same_dll(e.dll, dll)) return true;
    return false;
}

void page_guard_opening(const FH2Mod* mod, const std::string& page) {
    if (!mod || mod->dll.empty()) return;
    const std::string dll = narrow(mod->dll);
    GuardEntry mark{"opening", dll, page, dll_stamp(dll), today()};
    if (mark.stamp.empty()) return;
    mark.marked = now_utc();
    std::scoped_lock lk{g_guard_mu};
    ++g_opening;
    // left opening last session too (with no crash report): this mark is the second session running, once
    for (auto it = g_carried.begin(); it != g_carried.end(); ++it)
        if (it->page == page && same_dll(it->dll, dll)) {
            if (it->stamp == mark.stamp) mark.sessions = it->sessions + 1;
            g_carried.erase(it);
            break;
        }
    std::erase_if(g_guard, [&](const GuardEntry& e) { return e.page == page && same_dll(e.dll, dll); });
    g_guard.push_back(mark);
    write_guard();
}

void page_guard_loaded() {
    unsigned gen = 0;
    {
        std::scoped_lock lk{g_guard_mu};
        if (std::none_of(g_guard.begin(), g_guard.end(), [](const GuardEntry& e) { return e.state == "opening"; })) return;
        gen = g_opening;
    }
    // 10 s on screen, as the HUD layer's marker has it: a missing image only fails once it's drawn
    std::thread([gen] {
        Sleep(10000);
        std::scoped_lock lk{g_guard_mu};
        if (g_opening == gen) clear_marks();
    }).detach();
}

void page_guard_closed() {
    std::scoped_lock lk{g_guard_mu};
    clear_marks();
}

} // namespace fh2platform

// File overrides: mods replace files the game reads from game:\media by
// writing them to Extra Options' mediaoverride folder (which it mounts over
// media), or from 2.9.0 to this Azimuth folder's own layer (car_packs.cpp).
// One mod per file. fh2-mods\platform-files.txt lists the files the
// platform wrote, for whom and in which roots ("path<TAB>mod<TAB>roots"; 2.8.0
// wrote the first two only, and reads the rest as part of the mod's name), so
// ones no mod replaces any more (a mod was removed, or a feature turned off)
// get deleted from wherever they were written.
#include "platform.hpp"
#include "updates.hpp"
#include "xaml_safety.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::recursive_mutex g_files_mu;
std::map<std::wstring, FH2Mod*> g_owner; // lower-case relative path -> mod

fs::path files_manifest() { return azimuth_dir() / "fh2-mods" / "platform-files.txt"; }

std::wstring lower_w(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

// "drivatarglobalcache/DefaultCache.bin" -> L"drivatarglobalcache\DefaultCache.bin",
// or empty if it isn't a plain relative path.
std::wstring normalize(const char* media_path) {
    if (!media_path) return {};
    std::wstring p = widen(media_path);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (!p.empty() && p.front() == L'\\') p.erase(p.begin());
    if (p.size() > 6 && lower_w(p.substr(0, 6)) == L"media\\") p.erase(0, 6);
    if (p.empty() || p.back() == L'\\' || p.find(L':') != std::wstring::npos) return {};
    for (std::size_t i = 0, start = 0; i <= p.size(); ++i)
        if (i == p.size() || p[i] == L'\\') {
            const auto part = p.substr(start, i - start);
            if (part.empty() || part == L"." || part == L"..") return {};
            start = i + 1;
        }
    return p;
}

// A file the platform wrote: for whom, and in which roots (platform.hpp).
struct Written {
    std::string who;
    unsigned roots = kRootBoth;
};
using Manifest = std::map<std::wstring, Written>; // path -> who and where, as written last time

Manifest read_files_manifest() {
    Manifest out;
    std::ifstream in{files_manifest()};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos || !tab) continue;
        const auto roots = line.find('\t', tab + 1);
        Written& w = out[widen(line.substr(0, tab))];
        w.who      = line.substr(tab + 1, roots == std::string::npos ? std::string::npos : roots - tab - 1);
        w.roots    = roots == std::string::npos ? kRootBoth : parse_roots(line.substr(roots + 1));
    }
    return out;
}

void write_files_manifest(const Manifest& m) {
    std::ofstream out{files_manifest(), std::ios::trunc};
    for (const auto& [path, w] : m) out << narrow(path) << "\t" << w.who << "\t" << root_names(w.roots) << "\n";
}

Manifest::iterator find_ci(Manifest& m, const std::wstring& path) {
    const auto key = lower_w(path);
    return std::find_if(m.begin(), m.end(), [&](const auto& e) { return lower_w(e.first) == key; });
}

bool same_content(const fs::path& f, const void* data, std::size_t size) {
    std::error_code ec;
    if (fs::file_size(f, ec) != size || ec) return false;
    std::ifstream in{f, std::ios::binary};
    std::vector<char> d((std::istreambuf_iterator<char>(in)), {});
    return d.size() == size && (size == 0 || std::memcmp(d.data(), data, size) == 0);
}

bool write_file(const fs::path& target, const void* data, std::size_t size) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    auto tmp = target;
    tmp += L".platform-tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out) return false;
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out) return false;
    }
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool remove_file(const fs::path& target) {
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(target.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ||
           GetLastError() == ERROR_PATH_NOT_FOUND;
}

// The layer's zipmanifest.xml is the platform's own this session, merged with the car packs' zips (car_packs.cpp).
bool merged_manifest(unsigned root, const std::wstring& rel) {
    return root == kRootLayer && lower_w(rel) == L"zipmanifest.xml" && car_packs_manifest();
}

// Deletes the platform's copies of `rel` in every root `roots` names: 0, or the Windows error of one that failed.
DWORD remove_copies(const std::wstring& rel, unsigned roots) {
    DWORD error = 0;
    for (const unsigned r : {kRootOverride, kRootLayer}) {
        if (!(roots & r)) continue;
        if (merged_manifest(r, rel)) {   // a mod's from a session without packs: the one there now isn't the mod's
            plog("files: left media\\zipmanifest.xml in this folder's layer: it's the platform's this session, with the car packs' zips");
            continue;
        }
        if (!remove_file(root_folder(r) / rel) && !error) error = GetLastError();
    }
    return error;
}

std::string shown(const std::wstring& rel) { return "media\\" + narrow(rel); }

fs::path old_overrides() { return azimuth_dir() / "fh2-mods" / "platform-old-overrides"; }

} // namespace

int move_old_override(unsigned root, const fs::path& rel, std::string& error) {
    const fs::path from = root_folder(root) / rel;
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) return 0;
    const std::string name = root_names(root);
    const fs::path to      = old_overrides() / name / rel;
    fs::create_directories(to.parent_path(), ec);
    SetFileAttributesW(from.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(to.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        error = "error " + std::to_string(GetLastError());
        return -1;
    }
    // A record for the player of what went where, and when.
    SYSTEMTIME t;
    GetLocalTime(&t);
    char when[32];
    std::snprintf(when, sizeof when, "%04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    std::ofstream{old_overrides() / "moved.txt", std::ios::app} << when << "\t" << name << "\t" << narrow(rel.wstring()) << "\n";
    return 1;
}

void move_old_overrides() {
    if (game_mount().empty()) return;
    {
        std::scoped_lock lk{g_files_mu};
        auto manifest     = read_files_manifest();
        const unsigned at = current_root();
        bool changed      = false;
        std::size_t moved = 0;
        for (auto& [rel, w] : manifest) {
            unsigned roots = at; // and those a copy couldn't be moved from, to try again next time
            for (const unsigned r : {kRootOverride, kRootLayer}) {
                if (!(w.roots & r) || r == at) continue;
                std::string error;
                const int done = move_old_override(r, rel, error);
                if (done > 0) {
                    ++moved;
                    plog("files: moved " + root_names(r) + "\\" + narrow(rel) + " to fh2-mods\\platform-old-overrides\\" +
                         root_names(r));
                } else if (done < 0) {
                    roots |= r;
                    plog("files: couldn't move " + root_names(r) + "\\" + narrow(rel) + " to fh2-mods\\platform-old-overrides (" +
                         error + "); trying again next launch");
                }
            }
            changed = changed || roots != w.roots;
            w.roots = roots;
        }
        if (changed) write_files_manifest(manifest);
        if (moved)
            plog("files: " + std::to_string(moved) + " file(s) the platform wrote to " + root_shown(kRootBoth & ~at) +
                 " before (an older platform, or a session that wrote there) were moved to fh2-mods\\platform-old-overrides\\" +
                 root_names(kRootBoth & ~at) + ", as this session's go to " + root_shown(at) +
                 "; moved.txt there lists them");
    }
    move_old_tables();
}

int set_file(FH2Mod* mod, const char* media_path, const void* data, std::size_t size) {
    if (!mod || (!data && size)) return 0;
    const std::wstring rel = normalize(media_path);
    if (rel.empty()) {
        log_line(mod->name, std::string("set_file: \"") + (media_path ? media_path : "") +
                                "\" isn't a path under media");
        return 0;
    }
    const std::wstring key = lower_w(rel);
    if (key.rfind(L"stripped\\stringtables\\", 0) == 0 || key == L"stripped\\gamedbrc.slt") {
        log_line(mod->name, "set_file: " + shown(rel) +
                                " is managed by the platform; use the string-table or database functions");
        return 0;
    }
    if (game_mount().empty()) {
        log_line(mod->name, "set_file: the game folder wasn't found, so " + shown(rel) + " can't be replaced");
        return 0;
    }
    // The car packs' merged zipmanifest.xml (written before any mod starts) lists their zips: a mod's would take them
    // out, at every start, while the database still has their cars.
    if (merged_manifest(current_root(), key)) {
        log_line(mod->name, "set_file: " + shown(rel) + " isn't replaced: this session the platform writes it to this Azimuth "
                                "folder's own layer, with the car packs' zips (fh2-cars) added to the game's. A zip of a mod's "
                                "own can be added the same way, as a car pack");
        return 0;
    }
    std::scoped_lock lk{g_files_mu};
    if (stopped_online(mod)) {   // checked under the lock: go_online sets it before it clears the mod's files
        log_line(mod->name, "set_file: " + shown(rel) + " isn't replaced: this mod was stopped for online play (FH2MP went "
                                "online after it had started)");
        return 0;
    }
    if (auto it = g_owner.find(key); it != g_owner.end() && it->second != mod) {
        log_line(mod->name, "can't replace " + shown(rel) + ": " + it->second->name + " already does");
        return 0;
    }
    const fs::path target = override_root() / rel;
    auto manifest         = read_files_manifest();
    auto known            = find_ci(manifest, rel);
    std::error_code ec;
    if (known == manifest.end() && fs::exists(target, ec)) {
        // Someone put a file here by hand (or a mod from before the
        // platform did): keep a copy rather than losing it.
        auto bak = target;
        bak += L".bak";
        if (!fs::exists(bak, ec) && CopyFileW(target.c_str(), bak.c_str(), TRUE))
            log_line(mod->name, "kept the existing " + shown(rel) + " override as " + narrow(bak.filename().wstring()));
    }
    const bool same = same_content(target, data, size);
    if (!same && !write_file(target, data, size)) {
        log_line(mod->name, "FAILED to write " + narrow(target.wstring()) + " (error " +
                                std::to_string(GetLastError()) + ")");
        return 0;
    }
    g_owner[key] = mod;
    mod->files.insert(key);
    if (first_write_begun()) note_ran(mod, kRanEdits);   // the game may read it from now on (owns_files before)
    // Where it is now, and any other root a copy of it couldn't be moved from at start (move_old_overrides)
    unsigned roots = current_root();
    if (known != manifest.end()) {
        roots |= known->second.roots;
        manifest.erase(known);
    }
    manifest[rel] = {mod->name, roots};
    write_files_manifest(manifest);
    log_line(mod->name, (same ? "replaces " : "replaced ") + shown(rel) + " (" + std::to_string(size) + " bytes)");
    return 1;
}

int mod_set_file(FH2Mod* mod, const char* media_path, const void* data, std::size_t size) {
    // The platform's own UI files (platform 2.9.0): before, the first to set one won, and a mod's fh2mod_init runs
    // before the platform writes its menus, so a mod's A_PauseMenu.xaml took the MODS tab away.
    if (const PlatformUiFile* own = mod && media_path ? platform_ui_file(media_path) : nullptr) {
        std::string shown_path = media_path;
        std::replace(shown_path.begin(), shown_path.end(), '/', '\\');
        log_line(mod->name, "set_file: \"" + shown_path + "\" is the FH2 Mod Platform's own " + own->path + " (" + own->what +
                                "), which no mod can replace; it wasn't changed");
        return 0;
    }
    return set_file(mod, media_path, data, size);
}

bool files_locked(const std::function<bool()>& f) {
    std::scoped_lock lk{g_files_mu};
    return f();
}

bool owns_files(FH2Mod* mod) {
    std::scoped_lock lk{g_files_mu};
    return mod && !mod->files.empty();
}

void clear_file(FH2Mod* mod, const char* media_path) {
    if (!mod) return;
    std::scoped_lock lk{g_files_mu};
    std::vector<std::wstring> which;
    if (media_path) {
        const auto rel = normalize(media_path);
        if (!rel.empty() && mod->files.count(lower_w(rel))) which.push_back(rel);
    } else {
        which.assign(mod->files.begin(), mod->files.end());
    }
    if (which.empty()) return;
    auto manifest = read_files_manifest();
    for (const auto& rel : which) {
        const auto key = lower_w(rel);
        const auto it  = find_ci(manifest, rel);
        // the path as the mod gave it (the manifest has it; `which` is lower case when every file goes)
        const std::wstring as_given = it != manifest.end() ? it->first : rel;
        // from every root it was written to
        if (const DWORD e = remove_copies(rel, it != manifest.end() ? it->second.roots | current_root() : current_root()); !e)
            log_line(mod->name, "stopped replacing " + shown(as_given));
        else
            log_line(mod->name, "couldn't delete the " + shown(as_given) + " override (error " + std::to_string(e) + ")");
        mod->files.erase(key);
        g_owner.erase(key);
        if (it != manifest.end()) manifest.erase(it);
    }
    write_files_manifest(manifest);
}

void remove_leftover_file(const char* media_path) {
    const std::wstring rel = normalize(media_path);
    if (rel.empty() || game_mount().empty()) return;
    std::scoped_lock lk{g_files_mu};
    if (g_owner.count(lower_w(rel))) return;
    auto manifest = read_files_manifest();
    auto it       = find_ci(manifest, rel);
    if (it == manifest.end()) return;
    if (!remove_copies(rel, it->second.roots))
        plog("removed the " + shown(rel) + " override (" + it->second.who + " no longer replaces it)");
    manifest.erase(it);
    write_files_manifest(manifest);
}

void clean_leftover_files(const std::vector<FH2Mod*>&) {
    if (game_mount().empty()) return;
    std::scoped_lock lk{g_files_mu};
    auto manifest = read_files_manifest();
    bool changed  = false;
    for (auto it = manifest.begin(); it != manifest.end();) {
        if (g_owner.count(lower_w(it->first))) {
            ++it;
            continue;
        }
        if (!remove_copies(it->first, it->second.roots))
            plog("removed the " + shown(it->first) + " override (" + it->second.who + " no longer replaces it)");
        it      = manifest.erase(it);
        changed = true;
    }
    if (changed) write_files_manifest(manifest);
}

} // namespace fh2platform

// File overrides: mods replace files the game reads from game:\media by
// writing them to Extra Options' mediaoverride folder (which it mounts over
// media). One mod per file. fh2-mods\platform-files.txt lists the files the
// platform wrote and for whom, so ones no mod replaces any more (a mod was
// removed, or a feature turned off) get deleted.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
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

// path -> mod name, as written last time
std::map<std::wstring, std::string> read_files_manifest() {
    std::map<std::wstring, std::string> out;
    std::ifstream in{files_manifest()};
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos || !tab) continue;
        out[widen(line.substr(0, tab))] = line.substr(tab + 1);
    }
    return out;
}

void write_files_manifest(const std::map<std::wstring, std::string>& m) {
    std::ofstream out{files_manifest(), std::ios::trunc};
    for (const auto& [path, who] : m) out << narrow(path) << "\t" << who << "\n";
}

std::map<std::wstring, std::string>::iterator find_ci(std::map<std::wstring, std::string>& m,
                                                      const std::wstring& path) {
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

std::string shown(const std::wstring& rel) { return "media\\" + narrow(rel); }

} // namespace

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
    std::scoped_lock lk{g_files_mu};
    if (auto it = g_owner.find(key); it != g_owner.end() && it->second != mod) {
        log_line(mod->name, "can't replace " + shown(rel) + ": " + it->second->name + " already does");
        return 0;
    }
    const fs::path target = game_mount() / L"mediaoverride" / rel;
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
    if (known != manifest.end()) manifest.erase(known);
    manifest[rel] = mod->name;
    write_files_manifest(manifest);
    log_line(mod->name, (same ? "replaces " : "replaced ") + shown(rel) + " (" + std::to_string(size) + " bytes)");
    return 1;
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
        if (remove_file(game_mount() / L"mediaoverride" / rel))
            log_line(mod->name, "stopped replacing " + shown(rel));
        else
            log_line(mod->name, "couldn't delete the " + shown(rel) + " override (error " +
                                    std::to_string(GetLastError()) + ")");
        mod->files.erase(key);
        g_owner.erase(key);
        if (auto it = find_ci(manifest, rel); it != manifest.end()) manifest.erase(it);
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
    if (remove_file(game_mount() / L"mediaoverride" / rel))
        plog("removed the " + shown(rel) + " override (" + it->second + " no longer replaces it)");
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
        if (remove_file(game_mount() / L"mediaoverride" / it->first))
            plog("removed the " + shown(it->first) + " override (" + it->second + " no longer replaces it)");
        it      = manifest.erase(it);
        changed = true;
    }
    if (changed) write_files_manifest(manifest);
}

} // namespace fh2platform

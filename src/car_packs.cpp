// Car packs (prototype) and this Azimuth folder's own media layer.
//
// Every Azimuth folder that starts the same game folder shares its mediaoverride, so one
// install's mods show up in another's game. Without Extra Options the platform mounts the
// folders itself (media_overrides.cpp), so it can also mount folders of this Azimuth
// folder's own, ahead of mediaoverride.
//
// The game can only read through Azimuth's drives: game:\ (the Mount folder, where a path with
// ".." stops the game) and T:\, its temporary drive, which Azimuth 1.0 keeps in
// <Azimuth>\data\titles\1A96545B\tmp and doesn't empty between starts. So this folder's own
// files go there:
//   T:\fh2-platform\media\          what the platform writes: the Mods pages, string tables,
//                                   file overrides, the merged zip manifest, the database
//   T:\fh2-platform\packs\<pack>\   a pack's media folder, as hard links to its files (copies
//                                   when the pack is on another drive)
// and the packs themselves are in <Azimuth>\fh2-cars\<pack>\media\, laid out as under media
// (cars\<name>.zip and so on). A pack's zips are added to the game's zipmanifest.xml (the line
// for each, from the zip's own central directory), and its <pack>\*.sql run on the game
// database with the mods' SQL (database.cpp), each pack in a transaction of its own, so packs
// never write over each other's files.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::vector<CarPack> g_packs;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string read_all(const fs::path& f) {
    std::ifstream in{f, std::ios::binary};
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

bool write_all(const fs::path& f, const std::string& data) {
    std::error_code ec;
    fs::create_directories(f.parent_path(), ec);
    auto tmp = f;
    tmp += L".platform-tmp";
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// zipmanifest.xml's line for a zip: where its central directory starts, its size up to the end
// of the file (the end record included), and its entry count.
struct ZipDir {
    uint64_t start = 0, size = 0;
    unsigned entries = 0;
};

bool zip_dir(const fs::path& zip, ZipDir& out) {
    std::ifstream in{zip, std::ios::binary};
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const uint64_t size = static_cast<uint64_t>(in.tellg());
    const uint64_t tail = std::min<uint64_t>(size, 22 + 65535);
    std::string t(static_cast<std::size_t>(tail), '\0');
    in.seekg(static_cast<std::streamoff>(size - tail));
    in.read(t.data(), static_cast<std::streamsize>(tail));
    if (!in || t.size() < 22) return false;
    for (std::size_t i = t.size() - 21; i-- > 0;) {
        if (std::memcmp(t.data() + i, "PK\x05\x06", 4) != 0) continue;
        uint16_t n = 0;
        uint32_t cd_size = 0, cd_off = 0;
        std::memcpy(&n, t.data() + i + 10, 2);
        std::memcpy(&cd_size, t.data() + i + 12, 4);
        std::memcpy(&cd_off, t.data() + i + 16, 4);
        const uint64_t eocd = size - tail + i;
        if (uint64_t{cd_off} + cd_size != eocd) return false; // zip64 or data after the directory
        out = {cd_off, size - cd_off, n};
        return true;
    }
    return false;
}

std::string manifest_line(const std::string& game_path, const ZipDir& d) {
    return "  <Zip version=\"1\" path=\"" + game_path + "\" priority=\"80\" dirstart=\"" + std::to_string(d.start) +
           "\" dirsize=\"" + std::to_string(d.size) + "\" direntries=\"" + std::to_string(d.entries) + "\" />";
}

// The path="..." of a <Zip> line, lower case, or "".
std::string zip_line_path(const std::string& line) {
    const auto z = line.find("<Zip ");
    if (z == std::string::npos) return {};
    const auto p = line.find("path=\"", z);
    if (p == std::string::npos) return {};
    const auto e = line.find('"', p + 6);
    return e == std::string::npos ? std::string{} : lower(line.substr(p + 6, e - p - 6));
}

void write_manifest() {
    const fs::path out = instance_media() / "zipmanifest.xml";
    std::error_code ec;
    std::vector<std::pair<std::string, ZipDir>> zips; // game path (lower case), directory
    for (const auto& pack : g_packs)
        for (const auto& e : fs::recursive_directory_iterator(pack.media, ec)) {
            if (!e.is_regular_file(ec) || lower(narrow(e.path().extension().wstring())) != ".zip") continue;
            ZipDir d;
            const std::string game_path =
                "game:\\media\\" + lower(narrow(fs::relative(e.path(), pack.media, ec).wstring()));
            if (!zip_dir(e.path(), d)) {
                plog("car packs: " + pack.name + ": couldn't read the zip directory of " + game_path + "; left out");
                continue;
            }
            zips.erase(std::remove_if(zips.begin(), zips.end(), [&](const auto& z) { return z.first == game_path; }),
                       zips.end());
            zips.emplace_back(game_path, d);
        }
    if (zips.empty()) {
        if (fs::remove(out, ec)) plog("car packs: no pack zips; removed the merged zipmanifest.xml");
        return;
    }
    // Built on the manifest the game would read otherwise, so a pack in mediapc or mediaoverride keeps its cars.
    fs::path base;
    for (const char* dir : {"mediaoverride", "mediapc", "media"})
        if (fs::exists(game_mount() / dir / "zipmanifest.xml", ec)) {
            base = game_mount() / dir / "zipmanifest.xml";
            break;
        }
    const std::string text = base.empty() ? std::string{} : read_all(base);
    if (text.find("</ZipFiles>") == std::string::npos) {
        plog("car packs: couldn't read the game's zipmanifest.xml; pack zips aren't added");
        return;
    }
    std::string merged;
    std::size_t replaced = 0, pos = 0;
    while (pos < text.size()) {
        const auto nl   = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos + 1);
        pos              = nl == std::string::npos ? text.size() : nl + 1;
        const std::string p = zip_line_path(line);
        if (!p.empty() && std::any_of(zips.begin(), zips.end(), [&](const auto& z) { return z.first == p; })) {
            ++replaced;
            continue;
        }
        if (line.find("</ZipFiles>") != std::string::npos)
            for (const auto& [p2, d] : zips) merged += manifest_line(p2, d) + "\r\n";
        merged += line;
    }
    if (read_all(out) == merged) {
        plog("car packs: zipmanifest.xml unchanged (" + std::to_string(zips.size()) + " pack zip(s))");
        return;
    }
    if (!write_all(out, merged)) {
        plog("car packs: couldn't write " + narrow(out.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    plog("car packs: wrote zipmanifest.xml from " + narrow(base.wstring()) + " with " + std::to_string(zips.size()) +
         " pack zip(s) (" + std::to_string(replaced) + " of them replacing the game's line)");
    for (const auto& [p, d] : zips)
        plog("car packs:   " + p + " dirstart " + std::to_string(d.start) + " dirsize " + std::to_string(d.size) +
             " entries " + std::to_string(d.entries));
}

// A pack's SQL: its *.sql files (next to its media folder), by file name.
std::vector<std::string> read_sql(const CarPack& pack) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(pack.root, ec))
        if (e.is_regular_file(ec) && lower(narrow(e.path().extension().wstring())) == ".sql") files.push_back(e.path());
    std::sort(files.begin(), files.end(), [](const fs::path& x, const fs::path& y) {
        return lower(narrow(x.filename().wstring())) < lower(narrow(y.filename().wstring()));
    });
    std::vector<std::string> sql;
    for (const auto& f : files) sql.push_back(read_all(f));
    return sql;
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    // a copy: same size and time
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec || sa != sb) return false;
    const auto ta = fs::last_write_time(a, ec);
    if (ec) return false;
    const auto tb = fs::last_write_time(b, ec);
    return !ec && ta == tb;
}

// The files under a folder, relative to it, in lower case.
std::vector<std::pair<std::string, fs::path>> files_under(const fs::path& dir) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) {
            const fs::path rel = fs::relative(it->path(), dir, ec);
            if (!ec) out.emplace_back(lower(narrow(rel.wstring())), rel);
        }
    return out;
}

// Puts a pack's files on the temp drive, where the game can read them: hard links to them, or
// copies when the pack is on another drive. Kept in step with the pack at every start.
void mirror_pack(const CarPack& pack) {
    std::error_code ec;
    std::size_t linked = 0, copied = 0, kept = 0, removed = 0, failed = 0;
    const auto want = files_under(pack.media);
    for (const auto& [key, rel] : want) {
        const fs::path from = pack.media / rel, to = pack.mirror / rel;
        if (same_file(from, to)) {
            ++kept;
            continue;
        }
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);
        if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) ++linked;
        else if (CopyFileW(from.c_str(), to.c_str(), FALSE)) ++copied;
        else {
            ++failed;
            plog("car packs: " + pack.name + ": couldn't put " + narrow(rel.wstring()) + " on the temp drive (error " +
                 std::to_string(GetLastError()) + ")");
        }
    }
    for (const auto& [key, rel] : files_under(pack.mirror))
        if (std::none_of(want.begin(), want.end(), [&](const auto& w) { return w.first == key; }) &&
            fs::remove(pack.mirror / rel, ec))
            ++removed;
    plog("car packs: " + pack.name + " on the temp drive: " + std::to_string(kept) + " file(s) as they were, " +
         std::to_string(linked) + " linked, " + std::to_string(copied) + " copied, " + std::to_string(removed) +
         " removed" + (failed ? ", " + std::to_string(failed) + " FAILED" : std::string{}));
}

// A pack's folder name on the temp drive: its own when the game can spell it.
std::string mirror_name(const std::string& name, std::size_t index) {
    const bool plain = !name.empty() && name.size() <= 40 && std::all_of(name.begin(), name.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
    });
    return plain ? name : "pack-" + std::to_string(index + 1);
}

} // namespace

fs::path temp_drive() { return azimuth_dir() / L"data" / L"titles" / L"1A96545B" / L"tmp"; }

std::string temp_drive_spelling(const fs::path& folder) {
    std::error_code ec;
    const fs::path rel = fs::relative(folder, temp_drive(), ec);
    return ec || rel.empty() ? std::string{} : "T:\\" + narrow(rel.wstring()) + "\\";
}

fs::path instance_media() { return temp_drive() / L"fh2-platform" / L"media"; }

bool instance_layer() {
    std::error_code ec;
    return !game_mount().empty() && extra_options_dir().empty() && fs::is_directory(azimuth_dir() / L"data" / L"titles", ec);
}

fs::path override_root() {
    return instance_layer() ? instance_media() : game_mount() / "mediaoverride";
}

const std::vector<CarPack>& car_packs() { return g_packs; }

void prepare_car_packs() {
    std::error_code ec;
    fs::create_directories(instance_media(), ec);
    g_packs.clear();
    const fs::path root = azimuth_dir() / "fh2-cars";
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec) || !fs::is_directory(e.path() / "media", ec)) continue;
        g_packs.push_back({narrow(e.path().filename().wstring()), e.path(), e.path() / "media", {}});
    }
    std::sort(g_packs.begin(), g_packs.end(), [](const CarPack& a, const CarPack& b) { return lower(a.name) < lower(b.name); });
    plog("car packs: " + std::to_string(g_packs.size()) + " pack(s) in " + narrow(root.wstring()));
    const fs::path mirrors = temp_drive() / L"fh2-platform" / L"packs";
    for (std::size_t i = 0; i < g_packs.size(); ++i) g_packs[i].mirror = mirrors / mirror_name(g_packs[i].name, i);
    // Packs that are gone: their links (or copies) go too.
    for (const auto& e : fs::directory_iterator(mirrors, ec))
        if (std::none_of(g_packs.begin(), g_packs.end(),
                         [&](const CarPack& p) { return lower(narrow(p.mirror.filename().wstring())) == lower(narrow(e.path().filename().wstring())); }) &&
            fs::remove_all(e.path(), ec) != static_cast<std::uintmax_t>(-1))
            plog("car packs: removed " + narrow(e.path().filename().wstring()) + " from the temp drive (no such pack now)");
    for (auto& pack : g_packs) {
        mirror_pack(pack);
        pack.sql = read_sql(pack);
    }
    write_manifest();
}

} // namespace fh2platform

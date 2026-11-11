// Car packs, and this Azimuth folder's own media layer.
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
//                                   when the pack is on another drive, and for a car zip whose
//                                   carbin gets the car's part Ids, place_car_ids)
// and the packs themselves are in <Azimuth>\fh2-cars\<pack>\media\, laid out as under media
// (cars\<name>.zip and so on). A pack's zips are added to the game's zipmanifest.xml (the line
// for each, from the zip's own central directory), and its <pack>\*.sql run on the game
// database with the mods' SQL (database.cpp), each pack in a transaction of its own, so packs
// never write over each other's files. A pack's pack.ini lists the cars it adds; their Ids are
// given when the database is built (its SQL says {car:<media name>} where one goes), and kept.
//
// A car can have an engine sound of its own (pack.ini [car]: engine_sound, engine_sound_base, engine_synth). The game
// loads PG_<engine MediaName>.bank, but finds the bank's events only under a stock car's sound name (FMOD Studio looks
// event paths up in the master strings bank, which holds only the stock PG_ names): an engine named anything else is
// silent. So the pack's SQL names a stock sound (engine_sound_base). The platform links that PG_<engine_sound_base>.bank
// into the pack's temp-drive folder under engine_sound, and lists the car (with its media name) in
// fh2-mods\platform-engine-synths.txt for an engine sound mod: SynthWrapper (0.7.0) renames the car's engine to
// engine_sound when its synth is complete, plays engine_synth for it, and gives the game engine_sound_base's events.
// Without such a mod the car plays its stock engine_sound_base; a real base car keeps its own sound either way.
//
// With Extra Options loaded (platform 2.9.0, eo_packs), Extra Options mounts mediaoverride and mediapc itself and the
// platform chains its file lookup behind Extra Options' (media_overrides.cpp). The packs' mirrors are the same, and
// their merged zipmanifest.xml goes to a folder of its own, T:\fh2-platform\eo-media, mounted ahead of everything else;
// the platform's other files still go to mediaoverride. The pack folders are below Extra Options' mediaoverride, mediapc
// and carbin cache then, so a pack with a file that's also at the same game path in <Mount>\mediaoverride, \mediapc or
// a mod of Extra Options' is left out whole: that copy could be read with the pack's zip line. The packs' SQL isn't run
// yet in this mode, so their cars aren't in the game database.
#include "car_ids.hpp"
#include "platform.hpp"
#include "text.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::vector<CarPack> g_packs;
std::string g_unread;                         // why fh2-cars' packs aren't read this session
std::string g_unread_why;                     // and the reason alone (a message from later())
std::atomic<bool> g_merged{false};            // the layer's (or eo-media's) zipmanifest.xml is the platform's, with the packs' zips
std::mutex g_problem_mu;
std::map<std::string, std::string> g_problems; // pack -> why it isn't working
std::atomic<bool> g_lost{false};              // with Extra Options: the packs aren't in the game this session after all

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

// The merged zipmanifest.xml, in `root`: this folder's layer, or with Extra Options eo-media. Written only when it
// changes, to a temporary file first and then moved over the old one.
void write_manifest(const fs::path& root) {
    const fs::path out = root / "zipmanifest.xml";
    std::error_code ec;
    std::vector<std::pair<std::string, ZipDir>> zips; // game path (lower case), directory
    // Where two packs have the same zip, the game reads the first one's (media_overrides.cpp mounts
    // them in name order), so its line is the one used.
    std::map<std::string, std::string> from; // game path -> the pack it's from
    for (const auto& pack : g_packs)
        for (const auto& e : fs::recursive_directory_iterator(pack.left_out ? fs::path{} : pack.media, ec)) {   // none of a pack left out
            if (!e.is_regular_file(ec) || lower(narrow(e.path().extension().wstring())) != ".zip") continue;
            ZipDir d;
            const std::string game_path =
                "game:\\media\\" + lower(narrow(fs::relative(e.path(), pack.media, ec).wstring()));
            if (const auto f = from.find(game_path); f != from.end()) {
                plog("car packs: " + pack.name + ": " + game_path + " is in " + f->second + " too, whose zip is read");
                continue;
            }
            from[game_path] = pack.name;
            if (!zip_dir(e.path(), d)) {
                plog("car packs: " + pack.name + ": couldn't read the zip directory of " + game_path + "; left out");
                note_car_pack_problem(pack.name, later("The game can't read its {0}.", {game_path.substr(12)}));
                continue;
            }
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
    const std::string where = root == instance_media() ? "" : " in " + temp_drive_spelling(root);   // eo-media's says so
    if (read_all(out) == merged) {
        g_merged = true;
        plog("car packs: zipmanifest.xml" + where + " unchanged (" + std::to_string(zips.size()) + " pack zip(s))");
        return;
    }
    if (!write_all(out, merged)) {
        plog("car packs: couldn't write " + narrow(out.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
        return;
    }
    g_merged = true;
    plog("car packs: wrote zipmanifest.xml" + where + " from " + narrow(base.wstring()) + " with " + std::to_string(zips.size()) +
         " pack zip(s) (" + std::to_string(replaced) + " of them replacing the game's line)");
    for (const auto& [p, d] : zips)
        plog("car packs:   " + p + " dirstart " + std::to_string(d.start) + " dirsize " + std::to_string(d.size) +
             " entries " + std::to_string(d.entries));
}

// pack.ini, next to the media folder: [pack] name= and version=, and a [car <media name>] section
// for each car the pack adds, whose name= the MODS tab shows and whose id= is the Id it asks for
// (car_ids.cpp); the rest is for the tool that builds the pack. Without it a pack adds no car, and
// its SQL runs as it is.
void read_pack_ini(CarPack& pack) {
    const std::wstring ini = (pack.root / L"pack.ini").wstring();
    std::error_code ec;
    if (!fs::is_regular_file(ini, ec)) return;
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"pack", L"name", L"", buf, 1024, ini.c_str());
    pack.title = narrow(buf);
    GetPrivateProfileStringW(L"pack", L"version", L"", buf, 1024, ini.c_str());
    pack.version = narrow(buf);
    std::vector<wchar_t> names(32768);
    const DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    for (const wchar_t* s = names.data(); s < names.data() + n && *s; s += wcslen(s) + 1) {
        const std::string section = narrow(s);
        if (lower(section.substr(0, 4)) != "car ") continue;
        std::string car = section.substr(4);
        while (!car.empty() && car.front() == ' ') car.erase(car.begin());
        const bool ok = !car.empty() && car.size() <= 64 && std::all_of(car.begin(), car.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
        if (ok) {
            pack.cars.push_back(car);
            GetPrivateProfileStringW(s, L"name", L"", buf, 1024, ini.c_str());
            pack.car_names.push_back(narrow(buf));
            GetPrivateProfileStringW(s, L"id", L"", buf, 1024, ini.c_str());
            pack.wanted_ids.push_back(narrow(buf));
            CarPack::EngineSound es;
            es.car = car;
            GetPrivateProfileStringW(s, L"engine_sound", L"", buf, 1024, ini.c_str());
            es.sound = narrow(buf);
            GetPrivateProfileStringW(s, L"engine_sound_base", L"", buf, 1024, ini.c_str());
            es.base = narrow(buf);
            GetPrivateProfileStringW(s, L"engine_synth", L"", buf, 1024, ini.c_str());
            es.synth = narrow(buf);
            const auto plain = [](const std::string& n) {
                return !n.empty() && n.size() <= 64 && std::all_of(n.begin(), n.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
                });
            };
            if (!es.base.empty()) {
                if (!plain(es.sound) || !plain(es.base) || lower(es.sound) == lower(es.base))
                    plog("car packs: " + pack.name + ": " + car + "'s engine_sound / engine_sound_base aren't two different "
                         "plain names; its engine sound is left as its SQL has it");
                else
                    pack.engine_sounds.push_back(es);
            }
        } else {
            plog("car packs: " + pack.name + ": pack.ini's [" + section + "] isn't a car's media name; left out");
        }
    }
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
    std::vector<std::string> banks; // the engine sound banks link_engine_sounds() puts here
    for (const auto& es : pack.engine_sounds) banks.push_back(lower("audio\\fmodanthem\\durango\\pg_" + es.sound + ".bank"));
    for (const auto& [key, rel] : files_under(pack.mirror))
        if (std::none_of(want.begin(), want.end(), [&](const auto& w) { return w.first == key; }) &&
            std::find(banks.begin(), banks.end(), key) == banks.end() && fs::remove(pack.mirror / rel, ec))
            ++removed;
    plog("car packs: " + pack.name + " on the temp drive: " + std::to_string(kept) + " file(s) as they were, " +
         std::to_string(linked) + " linked, " + std::to_string(copied) + " copied, " + std::to_string(removed) +
         " removed" + (failed ? ", " + std::to_string(failed) + " FAILED" : std::string{}));
    if (failed)
        note_car_pack_problem(pack.name, later("{0} of its files couldn't be put on the game's temp drive.", {std::to_string(failed)}));
}

// A car's engine sound bank: the base car's bank, linked (or copied) into the pack's temp-drive folder under the car's
// own sound name, where the game looks for media\Audio\FmodAnthem\DURANGO\PG_<name>.bank.
void link_engine_sounds(const CarPack& pack) {
    for (const auto& es : pack.engine_sounds) {
        std::error_code ec;
        fs::path from;
        for (const char* dir : {"mediaoverride", "mediapc", "media"})
            if (const fs::path f = game_mount() / dir / "audio" / "fmodanthem" / "durango" / fs::path{"PG_" + es.base + ".bank"};
                fs::is_regular_file(f, ec)) {
                from = f;
                break;
            }
        const fs::path to = pack.mirror / "audio" / "fmodanthem" / "durango" / fs::path{"PG_" + es.sound + ".bank"};
        if (from.empty()) {
            plog("car packs: " + pack.name + ": " + es.car + "'s engine_sound_base " + es.base + " has no PG_" + es.base +
                 ".bank in the game; the car has no engine sound");
            note_car_pack_problem(pack.name, later("{0} has no engine sound: the game has no {1} sound.", {es.car, es.base}));
            fs::remove(to, ec);
            continue;
        }
        if (same_file(from, to)) continue;
        fs::create_directories(to.parent_path(), ec);
        fs::remove(to, ec);
        if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr) || CopyFileW(from.c_str(), to.c_str(), FALSE))
            plog("car packs: " + pack.name + ": " + es.car + "'s engine sound PG_" + es.sound + ".bank is " + es.base +
                 "'s, on the temp drive");
        else {
            plog("car packs: " + pack.name + ": couldn't put PG_" + es.sound + ".bank on the temp drive (error " +
                 std::to_string(GetLastError()) + ")");
            note_car_pack_problem(pack.name, later("{0}'s engine sound couldn't be put on the game's temp drive.", {es.car}));
        }
    }
}

uint32_t crc32(const std::string& s) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char b : s) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint32_t u32_at(const std::string& s, std::size_t o) {
    uint32_t v = 0;
    if (o + 4 <= s.size()) std::memcpy(&v, s.data() + o, 4);
    return v;
}

// A zip entry's data, uncompressed: stored, or deflated in stored blocks only (what make_car_pack.py
// writes for a carbin whose bytes are set later). False for anything else.
bool stored_data(const std::string& data, uint16_t method, std::string& out) {
    out.clear();
    if (method == 0) {
        out = data;
        return true;
    }
    if (method != 8) return false;
    for (std::size_t p = 0;;) {
        if (p + 5 > data.size() || (data[p] & 6) != 0) return false;
        uint16_t len = 0, nlen = 0;
        std::memcpy(&len, data.data() + p + 1, 2);
        std::memcpy(&nlen, data.data() + p + 3, 2);
        if ((len ^ nlen) != 0xFFFF || p + 5 + len > data.size()) return false;
        out.append(data, p + 5, len);
        const bool last = data[p] & 1;
        p += 5 + len;
        if (last) return p == data.size();
    }
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

NoLayer instance_layer_missing() {
    // Worked out once, so every part of the platform goes by the same answer all session: start_media_overrides
    // asks first, before anything is written, and logs it. data\titles\1A96545B is where Azimuth keeps the game's
    // T:\ drive (its tmp folder); a fresh Azimuth folder only has it once the game has been started, so asking
    // again later in the session could move later writes away from what was mounted. tmp\Turn10Temp.scratch is a
    // folder the game itself makes on T:\, so it shows that T:\ is that folder (every Azimuth folder here that has
    // run the game has it, on Azimuth 0.9.10 and 1.0.x alike).
    static const NoLayer why = [] {
        std::error_code ec;
        if (game_mount().empty()) return NoLayer::no_game;
        // Extra Options counted as loaded under Wine (extra_options_expected) mounts mediaoverride, not this layer.
        if (!extra_options_dir().empty() || extra_options_expected()) return NoLayer::extra_options;
        if (!fs::is_directory(temp_drive().parent_path(), ec)) return NoLayer::no_titles;
        if (!fs::is_directory(temp_drive() / L"Turn10Temp.scratch", ec)) return NoLayer::no_scratch;
        return NoLayer::none;
    }();
    return why;
}

bool instance_layer() { return instance_layer_missing() == NoLayer::none; }

namespace {

// Extra Options' own mods (its mods loader) that have a media folder: mods\Forza Horizon 2\<mod>\ from 0.6, and
// mods\<mod>\ as 0.4 has them (0.6 moves those as it starts). A folder that folder's loadorder.txt lists as "#<mod>"
// is turned off; one it doesn't list at all, Extra Options adds to the list as it starts, turned on.
struct EoMod {
    std::string name;   // as the log shows it: "Forza Horizon 2\<mod>" or "<mod>"
    fs::path media;
    fs::path root;
};

// `data_too`: also those without a media folder (a mod can be a database.json alone).
std::vector<EoMod> eo_mods(bool data_too = false) {
    std::vector<EoMod> out;
    const fs::path mods = azimuth_dir() / L"mods";
    for (const bool newer : {true, false}) {
        const fs::path dir = newer ? mods / L"Forza Horizon 2" : mods;
        std::set<std::string> off;
        std::istringstream order{read_all(dir / L"loadorder.txt")};
        for (std::string l; std::getline(order, l);) {
            if (l.rfind("\xEF\xBB\xBF", 0) == 0) l.erase(0, 3);
            const auto first = l.find_first_not_of(" \t");
            if (first == std::string::npos || l[first] != '#') continue;
            l.erase(0, first + 1);
            l.erase(0, std::min(l.size(), l.find_first_not_of(" \t")));
            l.erase(l.find_last_not_of(" \t\r") + 1);
            off.insert(lower(l));
        }
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = narrow(e.path().filename().wstring());
            if (!e.is_directory(ec) || off.count(lower(name)) || (!data_too && !fs::is_directory(e.path() / L"media", ec))) continue;
            if (!newer && (lower(name) == "forza horizon 2" || lower(name) == "forza motorsport 5" ||
                           lower(name) == "forza horizon 3" || lower(name) == "cache"))
                continue;   // its own folders, not mods
            out.push_back({(newer ? "Forza Horizon 2\\" : "") + name, e.path() / L"media", e.path()});
        }
    }
    return out;
}

// In JSON text `t`, from `from` to below `to`: where the value of the next "key": starts, or npos.
std::size_t json_value(const std::string& t, const std::string& key, std::size_t from, std::size_t to) {
    const std::string q = "\"" + key + "\"";
    const auto spaces  = [&](std::size_t i) {
        while (i < to && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
        return i;
    };
    for (std::size_t at = t.find(q, from); at != std::string::npos && at < to; at = t.find(q, at + 1))
        if (const std::size_t i = spaces(at + q.size()); i < to && t[i] == ':') return spaces(i + 1);
    return std::string::npos;
}

// One past the closing bracket of the JSON object or array that starts at `open` (strings skipped), or the text's end.
std::size_t json_end(const std::string& t, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '"') {
            for (++i; i < t.size() && t[i] != '"'; ++i)
                if (t[i] == '\\') ++i;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if ((c == '}' || c == ']') && --depth == 0) {
            return i + 1;
        }
    }
    return t.size();
}

int packs_in_fh2_cars() {
    std::error_code ec;
    int n = 0;
    for (const auto& e : fs::directory_iterator(azimuth_dir() / "fh2-cars", ec))
        n += e.is_directory(ec) && fs::is_directory(e.path() / "media", ec);
    return n;
}

struct EoDecision {
    bool on = false;
    std::string why;   // why not, for the player (a message from later()), or ""
};

// Logged as it's made: what Extra Options is, then whether the packs are read with it, and why not.
EoDecision decide_eo_packs() {
    EoDecision d;
    if (extra_options_dir().empty()) {
        if (extra_options_expected())   // under Wine, counted as loaded: what it does can't be seen, so as in rc1
            d.why = later("Extra Options is loaded.");
        return d;
    }
    const ExtraOptionsFacts eo = extra_options_facts();
    const std::string status   = extra_options_status();
    char built[96] = "its PE header couldn't be read";
    if (eo.lo) {
        const time_t t = eo.timestamp;
        tm utc{};
        if (eo.timestamp && gmtime_s(&utc, &t) == 0)
            std::snprintf(built, sizeof built, "PE time stamp 0x%08X (%04d-%02d-%02d %02d:%02d UTC)", eo.timestamp,
                          utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min);
        else
            std::snprintf(built, sizeof built, "PE time stamp 0x%08X", eo.timestamp);
    }
    plog("car packs: Extra Options is " + narrow(eo.module.wstring()) + " (version " +
         (eo.version.empty() ? std::string("unknown: its image doesn't say") : eo.version) + ", " + built + ", " +
         std::to_string(eo.file_size) + " bytes); its status now: \"" + status + "\"");
    const auto off = [&](const std::string& why, const std::string& log) {
        d.why = why;
        if (const int n = packs_in_fh2_cars())
            plog("car packs: with Extra Options loaded, the " + std::to_string(n) + " pack(s) in fh2-cars aren't read this session: " + log);
        return d;
    };
    const fs::path ini = extra_options_dir() / L"FH2_ExtraOptions.ini";
    std::error_code ec;
    if (!fs::is_regular_file(ini, ec))
        return off(later("Extra Options can't read its FH2_ExtraOptions.ini, so it doesn't start."),
                   "there's no FH2_ExtraOptions.ini next to Extra Options' winmm.dll, so it refuses to start");
    // As Extra Options reads them: 0 or 1, [General] being an older name for [Global]. Its media overrides are off when
    // the line isn't there.
    const auto flag = [&](const wchar_t* section, const wchar_t* key, int fallback) {
        return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, ini.c_str()));
    };
    if (flag(L"Global", L"Enabled", 1) == 0 || flag(L"General", L"Enabled", 1) == 0)
        return off(later("Extra Options is switched off in its FH2_ExtraOptions.ini ([Global] Enabled=0)."),
                   "FH2_ExtraOptions.ini says [Global] Enabled=0, so Extra Options makes no changes");
    if (status.rfind("REFUSED", 0) == 0 || status.rfind("DISABLED", 0) == 0 || status.rfind("INACTIVE", 0) == 0)
        return off(later("Extra Options made no changes to the game: {0}", {status}), "Extra Options has already given up: " + status);
    if (game_mount().empty()) return off({}, "the game's Mount folder wasn't found");
    // Online, the game has to read FH2MP's copy of the server's database in mediaoverride, which the packs' folder
    // would hide. Offline in the same folder (no session from FH2MP's launcher), the packs are read as anywhere else.
    if (std::string why; fh2mp_has_session(&why))
        return off(later("This game was started for online play with FH2MP. With Extra Options, car packs are only read when you play offline."),
                   "FH2MP has a session to play online with (" + why + "), and online the game has to read FH2MP's copy of the "
                   "server's database in mediaoverride, which the packs' folder would hide");
    if (fs::is_directory(azimuth_dir() / L"fh2mp", ec))
        plog("car packs: FH2MP is in this Azimuth folder but has no session to play online with, so this game is offline and the "
             "packs can be read; started from the FH2MP launcher, they aren't");
    const bool media = flag(L"Files", L"EnableMediaOverrides", 0) != 0;
    const std::vector<EoMod> mods = eo_mods();
    plog("car packs: Extra Options' media overrides are " + std::string(media ? "on" : "off") +
         " (EnableMediaOverrides in FH2_ExtraOptions.ini), and it has " + std::to_string(mods.size()) +
         " mod(s) of its own with a media folder, turned on");
    if (!media && mods.empty())
        return off(later("Extra Options' media overrides are off (EnableMediaOverrides=0 in FH2_ExtraOptions.ini) and it has no "
                         "mods, so it doesn't let the game read other folders."),
                   "Extra Options' media overrides are off and it has no mods of its own, so it mounts no folders at all");
    if (!fs::is_directory(temp_drive().parent_path(), ec))
        return off(later("This Azimuth folder has no data\\titles\\1A96545B folder yet: Azimuth makes it the first time the game starts."),
                   "this Azimuth folder has no data\\titles\\1A96545B yet (Azimuth makes it the first time the game starts)");
    if (!fs::is_directory(temp_drive() / L"Turn10Temp.scratch", ec))
        return off(later("This Azimuth folder has no data\\titles\\1A96545B\\tmp\\Turn10Temp.scratch folder yet: the game makes it "
                         "the first time it starts."),
                   "there's no data\\titles\\1A96545B\\tmp\\Turn10Temp.scratch yet (the game makes it on its T:\\ drive the first "
                   "time it starts)");
    if (!packs_in_fh2_cars()) {
        plog("car packs: Extra Options is loaded and fh2-cars has no pack, so there's nothing to read with it");
        return d;
    }
    d.on = true;
    return d;
}

const EoDecision& eo_decision() {
    // Once, as instance_layer: the whole session goes by it.
    static const EoDecision d = decide_eo_packs();
    return d;
}

} // namespace

bool eo_packs() { return eo_decision().on; }

// Extra Options runs its mods' database.json on the database the game has opened, which with the packs is theirs: a
// row it inserts with a pack car's Id would be refused, moved or shared, as that mod's policy says. So the cars its
// turned-on mods insert in the packs' range are read from the text here: Data_Car's "insert" rows' "Id".
// shortcut: reads Extra Options' format 2 (tables by name); a format 1 file reserves nothing, as before this was added.
std::map<int64_t, car_ids::Reserved> eo_mod_car_ids() {
    constexpr auto npos = std::string::npos;
    std::map<int64_t, car_ids::Reserved> out;
    for (const EoMod& m : eo_mods(true)) {
        // Its database: the one mod.json names (null for none), else database.json.
        fs::path file = m.root / L"database.json";
        if (const std::string manifest = read_all(m.root / L"mod.json"); !manifest.empty())
            if (const std::size_t v = json_value(manifest, "database", 0, manifest.size()); v != npos) {
                if (manifest[v] != '"') continue;
                file = m.root / widen(manifest.substr(v + 1, manifest.find('"', v + 1) - v - 1));
            }
        const std::string t     = read_all(file);
        const std::size_t tables = json_value(t, "tables", 0, t.size());
        if (tables == npos || t[tables] != '{') continue;
        const std::size_t table = json_value(t, "Data_Car", tables, json_end(t, tables));
        if (table == npos || t[table] != '{') continue;
        const std::size_t rows = json_value(t, "insert", table, json_end(t, table));
        if (rows == npos || t[rows] != '[') continue;
        const std::size_t end    = json_end(t, rows);
        const std::size_t policy = json_value(t, "id_conflicts", 0, t.size());
        const bool shared        = policy != npos && t.compare(policy, 11, "\"last_wins\"") == 0;
        std::string list;
        for (std::size_t v = json_value(t, "Id", rows, end); v != npos; v = json_value(t, "Id", v, end)) {
            const long long id = std::strtoll(t.c_str() + v, nullptr, 10);
            if (id < car_ids::kFirstPackCar || id >= car_ids::kFirstHandMade) continue;
            auto [at, fresh] = out.emplace(id, car_ids::Reserved{"Data_Car " + std::to_string(id) + " in its mod " + m.name, shared});
            if (!fresh) at->second.shared = at->second.shared || shared;
            list += (list.empty() ? "" : ", ") + std::to_string(id);
        }
        if (!list.empty())
            plog("car packs: Extra Options' mod " + m.name + " adds car(s) with Id " + list + (shared ? ", sharing rows by Id (id_conflicts last_wins)" : "") +
                 "; no new pack car is given one of those");
    }
    return out;
}

std::string eo_packs_off_reason() { return eo_decision().why; }

bool car_packs_mountable() { return instance_layer() || eo_packs(); }

fs::path eo_media() { return temp_drive() / L"fh2-platform" / L"eo-media"; }

namespace {

// eo-media is the platform's alone, and is read ahead of every other folder: anything in it but what this build writes
// there is removed, so the game never reads a file there that another build or tool left. Its own: zipmanifest.xml, and
// in Stripped the database (kept from one start to the next, so it's only built again when something changed), the
// copy of the car Ids' record. The cars' names (Stripped\StringTables) go at every start and are written again.
void clean_eo_media() {
    const auto sweep = [](const fs::path& dir, std::initializer_list<const char*> files, const char* folder) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const std::string name = lower(narrow(e.path().filename().wstring()));
            std::error_code ec2;
            if (e.is_directory(ec2) ? name == folder : std::find_if(files.begin(), files.end(), [&](const char* f) { return name == f; }) != files.end())
                continue;
            fs::remove_all(e.path(), ec2);
            plog("car packs: " + std::string(ec2 ? "couldn't remove " : "removed ") + narrow(e.path().filename().wstring()) + " from " +
                 temp_drive_spelling(dir) + (ec2 ? " (" + ec2.message() + ")" : std::string{}) + ": only the platform's own files go there");
        }
    };
    const fs::path dir = eo_media();
    std::error_code ec;
    fs::create_directories(dir, ec);
    sweep(dir, {"zipmanifest.xml"}, "stripped");
    sweep(dir / L"Stripped", {"gamedbrc.slt", "platform-car-ids.txt"}, "");
}

// With Extra Options, its mediaoverride, mediapc and carbin cache are mounted ahead of the pack folders, and its mods'
// zips and carbin copies can be read for a pack's files too. So a pack with a file that's also at the same game path in
// <Mount>\mediaoverride, <Mount>\mediapc or a mod of Extra Options' that's turned on is left out whole, SQL included: the
// game could read that copy with the pack's zip line (the null read at game+0xFD1927), or with other part Ids. The
// other packs still work. The engine sound banks the platform links into a pack's folder count as its files, and a
// folder named as one of its zips (cars\X beside cars\X.zip) counts too: the game reads loose files there in place of
// the zip's.
struct Shadows {
    std::vector<std::pair<std::string, fs::path>> folders;   // shown as, folder
    std::map<std::string, std::string> eo;                   // a path under media (lower case) -> the Extra Options mod with it
};

Shadows shadows() {
    Shadows s;
    s.folders = {{"mediaoverride", game_mount() / L"mediaoverride"}, {"mediapc", game_mount() / L"mediapc"}};
    for (const EoMod& m : eo_mods())
        for (const auto& [key, rel] : files_under(m.media)) s.eo.emplace(key, m.name);
    return s;
}

bool left_out_shadowed(CarPack& pack, const Shadows& s) {
    // A mod of Extra Options' with a zipmanifest.xml of its own: the packs' merged one, built on the game's, mediapc's or
    // mediaoverride's, would hide it and the zips it lists. No pack is read then.
    if (const auto it = s.eo.find("zipmanifest.xml"); it != s.eo.end()) {
        pack.left_out = true;
        plog("car packs: " + pack.name + ": Extra Options' mod " + it->second + " has a zipmanifest.xml of its own, which the packs' "
             "merged one would hide, so no pack is read this session");
        note_car_pack_problem(pack.name, later("Its file {0} is also in {1}; with Extra Options that copy could be read instead, so the "
                                               "pack is left out.", {"zipmanifest.xml", "Extra Options' mod " + it->second}));
        return true;
    }
    auto mine = files_under(pack.media);
    for (const auto& es : pack.engine_sounds) {
        const fs::path bank = fs::path{L"audio"} / L"fmodanthem" / L"durango" / fs::path{"PG_" + es.sound + ".bank"};
        mine.emplace_back(lower(narrow(bank.wstring())), bank);
    }
    std::string file, where;
    for (const auto& [key, rel] : mine) {
        const bool zip = key.size() > 4 && key.compare(key.size() - 4, 4, ".zip") == 0;
        const fs::path unzipped = zip ? fs::path{rel}.replace_extension() : fs::path{};
        for (const auto& [shown, dir] : s.folders) {
            std::error_code ec;
            if (fs::exists(dir / rel, ec)) file = narrow(rel.wstring()), where = shown;
            else if (zip && fs::is_directory(dir / unzipped, ec)) file = narrow(unzipped.wstring()) + " (a folder, for " + narrow(rel.filename().wstring()) + ")", where = shown;
            if (!file.empty()) break;
        }
        if (const auto it = s.eo.find(key); file.empty() && it != s.eo.end())
            file = narrow(rel.wstring()), where = "Extra Options' mod " + it->second;
        if (file.empty() && zip) {   // a file under cars\X\ in one of its mods
            const std::string under = key.substr(0, key.size() - 4) + "\\";
            if (const auto it = s.eo.lower_bound(under); it != s.eo.end() && it->first.compare(0, under.size(), under) == 0)
                file = narrow(unzipped.wstring()) + " (a folder, for " + narrow(rel.filename().wstring()) + ")", where = "Extra Options' mod " + it->second;
        }
        if (!file.empty()) break;
    }
    if (file.empty()) return false;
    pack.left_out = true;
    plog("car packs: " + pack.name + ": its " + file + " is also in " + where + ", which can be read instead of it with Extra "
         "Options, so this pack is left out this session (its folder, its zips and its SQL)");
    note_car_pack_problem(pack.name, later("Its file {0} is also in {1}; with Extra Options that copy could be read instead, so the "
                                           "pack is left out.", {file, where}));
    return true;
}

std::mutex g_outcome_mu;

} // namespace

void note_eo_packs_outcome(const std::string& outcome, const std::string& why) {
    const fs::path f = azimuth_dir() / "fh2-mods" / "platform-eo-packs.txt";
    std::scoped_lock lk{g_outcome_mu};
    std::error_code ec;
    if (outcome.empty()) {
        fs::remove(f, ec);
        return;
    }
    if (g_lost.load() && outcome != "lost") return;   // the session's outcome stays lost
    // Written two or three times a session, sometimes a second apart: a virus scanner can still have the last one open,
    // and then the new one can't be moved over it (error 5) until it lets go. Never on a game thread.
    bool written = false;
    for (int tries = 0; tries < 20 && !(written = write_all(f, outcome + "\t" + why + "\r\n")); ++tries) Sleep(100);
    if (!written)
        plog("car packs: couldn't write fh2-mods\\platform-eo-packs.txt (error " + std::to_string(GetLastError()) + ")");
}

void car_packs_lost(const std::string& why) {
    if (g_lost.exchange(true)) return;   // the first reason is the one
    plog("car packs with Extra Options: the packs are NOT in the game this session: " + english(why));
    {
        // Over any other problem a pack had: this is why it isn't in the game.
        const std::string row = later("Not in the game this time: {0}", {why});
        std::scoped_lock lk{g_problem_mu};
        for (const CarPack& p : g_packs)
            if (!p.left_out) g_problems[p.name] = row;
    }
    note_eo_packs_outcome("lost", english(why));
    recheck_recorded_cars();   // against the database the game reads without them; the saves are copied if a car is gone
}

bool eo_packs_lost() { return g_lost.load(); }

fs::path override_root() {
    return instance_layer() ? instance_media() : game_mount() / "mediaoverride";
}

unsigned current_root() { return instance_layer() ? kRootLayer : kRootOverride; }

fs::path root_folder(unsigned root) { return root == kRootLayer ? instance_media() : game_mount() / "mediaoverride"; }

std::string root_names(unsigned roots) {
    std::string s = roots & kRootOverride ? "mediaoverride" : "";
    if (roots & kRootLayer) s += s.empty() ? "layer" : " layer";
    return s;
}

std::string root_shown(unsigned root) { return root == kRootLayer ? "this Azimuth folder's own layer" : "mediaoverride"; }

unsigned parse_roots(const std::string& text) {
    unsigned roots = 0;
    std::istringstream words{text};
    for (std::string w; words >> w;) roots |= w == "mediaoverride" ? kRootOverride : w == "layer" ? kRootLayer : 0u;
    return roots ? roots : kRootBoth;
}

const std::vector<CarPack>& car_packs() { return g_packs; }

bool car_packs_manifest() { return g_merged.load(); }

void car_packs_unavailable(const std::string& why) {
    // No pack is read, so an engine sound mod mustn't be told about any (the list from a session that read them would
    // have it load sounds for cars that aren't in the game).
    write_engine_synths();
    const int n = packs_in_fh2_cars();
    if (!n || why.empty()) return;
    g_unread_why = why;
    // (2.9.0 rc1 added "They need Azimuth 1.0 or later, without Extra Options.", which is no longer so.)
    g_unread = later("The {0} car pack(s) in fh2-cars aren't read: {1}", {std::to_string(n), why});
    plog("car packs: " + english(g_unread));
}

std::string car_packs_unread() { return g_unread; }

std::string car_packs_unread_why() { return g_unread_why; }

// A car's carbin names its upgradeable parts' models by their rows' Ids. Its body's are shared with
// the base car, but its rear wing's rows are its own, which its SQL numbers <car Id> * 1000 + n, so
// those Ids can only be set once the car has its Id. make_car_pack.py stores the carbin so its bytes
// are at fixed places and lists them in <car>.part-ids.txt ("zip <path under media>", "entry <local
// header offset>", "at <offset> <n>" lines). They're set in the zip on the temp drive, which is then
// a copy of the pack's rather than a link to it, with the entry's CRC made right; the copy keeps the
// pack zip's time, so mirror_pack keeps it until the pack's zip changes.
void place_car_ids(const CarPack& pack, const std::vector<std::pair<std::string, int64_t>>& cars) {
    for (const auto& [car, id] : cars) {
        std::error_code ec;
        const fs::path list = pack.root / fs::path{car + ".part-ids.txt"};
        if (!fs::is_regular_file(list, ec)) continue;
        std::string zip_rel;
        uint64_t entry = 0;
        std::vector<std::pair<uint64_t, uint64_t>> at;
        std::istringstream lines{read_all(list)};
        for (std::string l; std::getline(lines, l);) {
            std::istringstream w{l};
            std::string key;
            w >> key;
            uint64_t a = 0, b = 0;
            if (key == "zip") w >> zip_rel;
            else if (key == "entry") w >> entry;
            else if (key == "at" && w >> a >> b) at.emplace_back(a, b);
        }
        const auto fail = [&](const std::string& why) {
            plog("car packs: " + pack.name + ": " + car + "'s rear wing parts: " + why);
            note_car_pack_problem(pack.name, later("{0}'s rear wing may not show.", {car}));   // the log says why
        };
        if (zip_rel.empty() || at.empty()) {
            fail(car + ".part-ids.txt lists nothing");
            continue;
        }
        const fs::path from = pack.media / fs::path{zip_rel}, to = pack.mirror / fs::path{zip_rel};
        bool copied = false;
        if (fs::equivalent(from, to, ec)) { // a link: the Ids would go into the pack's own zip
            fs::remove(to, ec);
            if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
                fail("couldn't copy its zip to the temp drive (error " + std::to_string(GetLastError()) + ")");
                continue;
            }
            copied = true;
        }
        const auto want = [&](uint64_t n) { return static_cast<uint32_t>(id * 1000 + static_cast<int64_t>(n)); };
        // Most starts they're set already: look at those places only.
        std::size_t set = 0;
        {
            std::ifstream in{to, std::ios::binary};
            for (const auto& [o, n] : at) {
                uint32_t v = 0;
                in.seekg(static_cast<std::streamoff>(o));
                in.read(reinterpret_cast<char*>(&v), 4);
                set += !in || v != want(n);
            }
        }
        if (set) {
            std::string zip = read_all(to);
            if (std::any_of(at.begin(), at.end(), [&](const auto& a) { return a.first + 4 > zip.size(); })) {
                fail("its zip is shorter than " + car + ".part-ids.txt says");
                continue;
            }
            set = 0;
            for (const auto& [o, n] : at)
                if (const uint32_t v = want(n); u32_at(zip, o) != v) std::memcpy(zip.data() + o, &v, 4), ++set;
            uint16_t method = 0, nlen = 0, xlen = 0;
            if (u32_at(zip, entry) != 0x04034B50 || entry + 30 > zip.size()) {
                fail("no zip entry where " + car + ".part-ids.txt says");
                continue;
            }
            std::memcpy(&method, zip.data() + entry + 8, 2);
            std::memcpy(&nlen, zip.data() + entry + 26, 2);
            std::memcpy(&xlen, zip.data() + entry + 28, 2);
            const uint64_t data = entry + 30 + nlen + xlen, size = u32_at(zip, entry + 18);
            std::string content;
            if (data + size > zip.size() || !stored_data(zip.substr(data, size), method, content)) {
                fail("its carbin isn't stored in its zip as make_car_pack.py stores it");
                continue;
            }
            const uint32_t crc = crc32(content);
            std::memcpy(zip.data() + entry + 14, &crc, 4);
            ZipDir d;
            bool listed = false;
            if (zip_dir(to, d))
                for (uint64_t q = d.start; q + 46 <= zip.size() && u32_at(zip, q) == 0x02014B50;) {
                    if (u32_at(zip, q + 42) == entry) std::memcpy(zip.data() + q + 16, &crc, 4), listed = true;
                    uint16_t n2 = 0, x2 = 0, c2 = 0;
                    std::memcpy(&n2, zip.data() + q + 28, 2);
                    std::memcpy(&x2, zip.data() + q + 30, 2);
                    std::memcpy(&c2, zip.data() + q + 32, 2);
                    q += 46 + n2 + x2 + c2;
                }
            if (!listed) {
                fail("its zip's directory doesn't list the carbin");
                continue;
            }
            if (!write_all(to, zip)) {
                fail("couldn't write its zip on the temp drive (error " + std::to_string(GetLastError()) + ")");
                continue;
            }
        }
        if (set || copied) fs::last_write_time(to, fs::last_write_time(from, ec), ec);
        if (set)
            plog("car packs: " + pack.name + ": " + car + "'s rear wing part Ids set for car " + std::to_string(id) +
                 " (" + std::to_string(set) + " place(s) in its carbin, in its zip on the temp drive)");
    }
}

void note_car_pack_problem(const std::string& pack, const std::string& why) {
    std::scoped_lock lk{g_problem_mu};
    if (auto& p = g_problems[pack]; p.empty()) p = why;
}

std::string car_pack_problem(const std::string& pack) {
    std::scoped_lock lk{g_problem_mu};
    const auto it = g_problems.find(pack);
    return it == g_problems.end() ? std::string{} : it->second;
}

void prepare_car_packs() {
    std::error_code ec;
    const bool eo = eo_packs();   // else this folder's own layer
    if (eo)
        clean_eo_media();
    else
        fs::create_directories(instance_media(), ec);
    g_packs.clear();
    const fs::path root = azimuth_dir() / "fh2-cars";
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec) || !fs::is_directory(e.path() / "media", ec)) continue;
        g_packs.push_back({narrow(e.path().filename().wstring()), e.path(), e.path() / "media", {}, {}, {}, {}, {}});
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
    const Shadows shadowed = eo ? shadows() : Shadows{};
    for (auto& pack : g_packs) {
        read_pack_ini(pack);
        if (!pack.title.empty() || !pack.cars.empty()) {
            std::string cars;
            for (const auto& c : pack.cars) cars += (cars.empty() ? "" : ", ") + c;
            plog("car packs: " + pack.name + ": " + (pack.title.empty() ? pack.name : pack.title) +
                 (pack.version.empty() ? "" : " " + pack.version) +
                 (pack.cars.empty() ? ", no new car" : ", adds " + cars));
        }
        if (eo && left_out_shadowed(pack, shadowed)) continue;
        mirror_pack(pack);
        link_engine_sounds(pack);
        pack.sql = read_sql(pack);
    }
    write_manifest(eo ? eo_media() : instance_media());
    write_engine_synths();
}

// <sound name> TAB <base car's sound name> TAB <folder> TAB <.synth name without extension> TAB <pack> TAB <car's media
// name>, one car a line, with "PG_" on the sound names (as the game's events use them). The media name (2.9.0) lets
// the mod rename the car's engine; mods reading only the first five columns are unaffected. Rewritten at every
// start; gone when no pack has one.
void write_engine_synths() {
    std::string text;
    for (const auto& pack : g_packs)
        for (const auto& es : pack.engine_sounds) {
            if (es.synth.empty() || pack.left_out) continue;
            std::error_code ec;
            const fs::path synth = pack.root / fs::path{es.synth};
            if (!fs::is_regular_file(synth, ec)) {
                plog("car packs: " + pack.name + ": " + es.car + "'s engine_synth " + es.synth + " isn't there");
                note_car_pack_problem(pack.name, later("{0}'s engine sound file {1} is missing.", {es.car, es.synth}));
                continue;
            }
            text += "PG_" + es.sound + "\tPG_" + es.base + "\t" + narrow(synth.parent_path().wstring()) + "\t" +
                    narrow(synth.stem().wstring()) + "\t" + pack.name + "\t" + es.car + "\r\n";
        }
    const fs::path out = azimuth_dir() / "fh2-mods" / "platform-engine-synths.txt";
    std::error_code ec;
    if (text.empty()) {
        if (fs::remove(out, ec))
            plog(g_packs.empty() ? "car packs: no pack is read this session; removed platform-engine-synths.txt"
                                 : "car packs: no pack has an engine sound of its own; removed platform-engine-synths.txt");
        return;
    }
    if (read_all(out) != text && !write_all(out, text))
        plog("car packs: couldn't write platform-engine-synths.txt (error " + std::to_string(GetLastError()) + ")");
    else
        plog("car packs: platform-engine-synths.txt lists " + std::to_string(std::count(text.begin(), text.end(), '\n')) +
             " car(s) with an engine sound of their own");
}

} // namespace fh2platform

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
#include "platform.hpp"
#include "text.hpp"
#include "updates.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace fh2platform {

namespace fs = std::filesystem;

namespace {

std::vector<CarPack> g_packs;
std::string g_unread;                         // why fh2-cars' packs aren't read this session
std::mutex g_problem_mu;
std::map<std::string, std::string> g_problems; // pack -> why it isn't working

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
    // Where two packs have the same zip, the game reads the first one's (media_overrides.cpp mounts
    // them in name order), so its line is the one used.
    std::map<std::string, std::string> from; // game path -> the pack it's from
    for (const auto& pack : g_packs)
        for (const auto& e : fs::recursive_directory_iterator(pack.media, ec)) {
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

bool instance_layer() {
    // Worked out once, so every part of the platform goes by the same answer all session: start_media_overrides
    // asks first, before anything is written, and logs it. data\titles\1A96545B is where Azimuth keeps the game's
    // T:\ drive (its tmp folder); a fresh Azimuth folder only has it once the game has been started, so asking
    // again later in the session could move later writes away from what was mounted.
    static const bool layer = [] {
        std::error_code ec;
        // Extra Options counted as loaded under Wine (extra_options_expected) mounts mediaoverride, not this layer.
        return !game_mount().empty() && extra_options_dir().empty() && !extra_options_expected() &&
               fs::is_directory(temp_drive().parent_path(), ec);
    }();
    return layer;
}

fs::path override_root() {
    return instance_layer() ? instance_media() : game_mount() / "mediaoverride";
}

const std::vector<CarPack>& car_packs() { return g_packs; }

void car_packs_unavailable(const std::string& why) {
    std::error_code ec;
    int n = 0;
    for (const auto& e : fs::directory_iterator(azimuth_dir() / "fh2-cars", ec))
        n += e.is_directory(ec) && fs::is_directory(e.path() / "media", ec);
    if (!n) return;
    g_unread = later("The {0} car pack(s) in fh2-cars aren't read: {1} They need Azimuth 1.0 or later, without Extra Options.",
                     {std::to_string(n), why});
    plog("car packs: " + english(g_unread));
}

std::string car_packs_unread() { return g_unread; }

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
    for (auto& pack : g_packs) {
        read_pack_ini(pack);
        if (!pack.title.empty() || !pack.cars.empty()) {
            std::string cars;
            for (const auto& c : pack.cars) cars += (cars.empty() ? "" : ", ") + c;
            plog("car packs: " + pack.name + ": " + (pack.title.empty() ? pack.name : pack.title) +
                 (pack.version.empty() ? "" : " " + pack.version) +
                 (pack.cars.empty() ? ", no new car" : ", adds " + cars));
        }
        mirror_pack(pack);
        link_engine_sounds(pack);
        pack.sql = read_sql(pack);
    }
    write_manifest();
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
            if (es.synth.empty()) continue;
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
        if (fs::remove(out, ec)) plog("car packs: no pack has an engine sound of its own; removed platform-engine-synths.txt");
        return;
    }
    if (read_all(out) != text && !write_all(out, text))
        plog("car packs: couldn't write platform-engine-synths.txt (error " + std::to_string(GetLastError()) + ")");
    else
        plog("car packs: platform-engine-synths.txt lists " + std::to_string(std::count(text.begin(), text.end(), '\n')) +
             " car(s) with an engine sound of their own");
}

} // namespace fh2platform

// The database guards' rules, with no game in them (db_guards.hpp): where the game's database comes from, the
// game's powertrain lookup as the guard repeats it, the code the guard relies on, and its log line. The platform
// (db_guards.cpp) and tests/db_guards_test.cpp both use them.
#include "db_guards.hpp"

#include <cstdio>
#include <cstdlib>

namespace fh2platform {

// ---- where the game's database comes from ---------------------------------------------------------------------

DbSource db_source(const DbFacts& f) {
    if (f.layer) return DbSource::platform_layer;
    switch (f.override_db) {
    case OverrideDb::platform: return DbSource::platform_mediaoverride;
    case OverrideDb::fh2mp: return DbSource::fh2mp;
    case OverrideDb::fh2ml: return DbSource::fh2ml;
    case OverrideDb::other: return DbSource::other_mod;
    case OverrideDb::copy_of_media: return DbSource::games_own;   // it hides mediapc's, if there is one
    case OverrideDb::copy_of_mediapc:
        return f.mediapc && !f.mediapc_same_as_media ? DbSource::car_pack_mediapc : DbSource::games_own;
    case OverrideDb::none: break;
    }
    return f.mediapc && !f.mediapc_same_as_media ? DbSource::car_pack_mediapc : DbSource::games_own;
}

const char* db_source_text(DbSource s) {
    switch (s) {
    case DbSource::games_own: return "the game reads its own database";
    case DbSource::platform_layer: return "the platform built this Azimuth folder's database with car packs' or mods' changes";
    case DbSource::platform_mediaoverride: return "the platform built mediaoverride's database with mods' changes";
    case DbSource::fh2mp: return "mediaoverride has FH2MP's copy of the server's database";
    case DbSource::fh2ml: return "mediaoverride has FH2ML's database";
    case DbSource::other_mod: return "mediaoverride has another mod's database";
    case DbSource::car_pack_mediapc: return "mediapc has a car pack's database";
    }
    return "";
}

// ---- the game's powertrain lookup -------------------------------------------------------------------------------

Powertrain powertrain_lookup(GuardReadFn read, uintptr_t game_base, uint32_t id) {
    uintptr_t cache = 0, table = 0, begin = 0, end = 0, record = 0;
    if (!read(game_base + kRvaCarDbCache, &cache, sizeof cache) || !cache) return Powertrain::unreadable;
    if (!read(cache + kCacheToPowertrains, &table, sizeof table) || !table) return Powertrain::unreadable;
    if (!read(table + kTableBegin, &begin, sizeof begin) || !read(table + kTableEnd, &end, sizeof end) || end < begin ||
        (end - begin) % 8)
        return Powertrain::unreadable;
    if (id >= (end - begin) / 8) return Powertrain::missing;
    if (!read(begin + 8 * static_cast<uintptr_t>(id), &record, sizeof record)) return Powertrain::unreadable;
    return record ? Powertrain::known : Powertrain::missing;
}

// ---- the code the guard relies on -------------------------------------------------------------------------------

// From the exe on disk (forza_durango_release_final.exe of 24 Nov 2015). Each range is whole instructions.
// clang-format off
const GuardCodeCheck kGuardCodeChecks[] = {
    // DriveTrainConverter's vtable, slot 0: the converter
    {0x2DA6938, "E0 9B B9 40 01 00 00 00", "DriveTrainConverter's vtable"},
    // its start to the parameter's AddRef: rbx = the value (an int, or the low half of a 64-bit one), rdi = the
    // parameter (a string, or NULL), rsi = where the result goes, rbp = the type the binding wants
    {0xB99BE0, "48 8B C4 57 41 56 41 57 48 83 EC 60 48 C7 40 D8 FE FF FF FF 48 89 58 08 48 89 68 10 48 89 70 20 49 8B E9 "
               "49 8B D8 48 8B F2 4C 8B F1 45 33 FF 44 89 7C 24 20 4D 85 C0 0F 84 2D 02 00 00 33 C9 E8 FC EE A2 00 48 8B D0 "
               "48 8B CB E8 B1 94 8B 00 84 C0 74 05 8B 5B 28 EB 1E 33 C9 E8 71 F8 A2 00 48 8B D0 48 8B CB E8 96 94 8B 00 84 "
               "C0 0F 84 F8 01 00 00 48 8B 5B 28 48 8B BC 24 A0 00 00 00 48 85 FF 75 05 49 8B FF EB 18 33 C9 E8 C1 AE B0 00 "
               "48 8B D0 48 8B CF E8 66 94 8B 00 84 C0 49 0F 44 FF 48 89 7C 24 58 48 85 FF 74 04 F0 FF 47 10",
     "DriveTrainConverter: the value and the parameter"},
    // the string check (the guard's first site, 0xB99CA0: test al,al; je 0xB99D62), the "text" parameter's compare,
    // the cache's lookup and the read that faults (0xB99CD3)
    {0xB99C8E, "33 C9 E8 9B AE B0 00 48 8B D0 48 8B CD E8 C0 5C 9B 00 84 C0 0F 84 BA 00 00 00 48 8D 15 F1 38 36 01 48 8B "
               "4F 28 FF 15 47 6E 23 01 48 8B 0D A0 1D 91 02 85 C0 75 5E E8 F7 41 68 FF 48 8B C8 8B D3 E8 2D 5C 6D FF 44 0F "
               "BE 40 01 49 8D 4E D8 48 8D 54 24 38 E8 8A 18 00 00",
     "DriveTrainConverter: the string check and the drive type's text"},
    // the icon path's lookup (another parameter)
    {0xB99D22, "E8 99 41 68 FF 48 8B C8 8B D3 E8 CF 5B 6D FF 48 83 C0 30", "DriveTrainConverter: the icon's path"},
    // the other type's check (the guard's second site, 0xB99D74: test al,al; je 0xB99E37) and its lookup
    {0xB99D62, "33 C9 E8 77 F4 A2 00 48 8B D0 48 8B CD E8 EC 5B 9B 00 84 C0 0F 84 BB 00 00 00 48 8B 0D DD 1C 91 02 E8 38 "
               "41 68 FF 48 8B C8 8B D3 E8 6E 5B 6D FF 48 83 C0 30",
     "DriveTrainConverter: the other type's check"},
    // where the guard sends it: the parameter released, the game's empty value returned (AddRef'd), and the return
    {0xB99E37, "48 85 FF 74 0E F0 FF 4F 10 75 08 48 8B CF E8 E6 D8 6B FF E8 F1 24 A7 00 48 85 C0 48 89 06 74 04 F0 FF 40 "
               "10 48 8B C6 4C 8D 5C 24 60 49 8B 5B 20 49 8B 6B 28 49 8B 73 38 49 8B E3 41 5F 41 5E 5F C3",
     "DriveTrainConverter: the way out for a type it can't make"},
    // the empty value (a static object, game+0x36BA4F8)
    {0x160C340, "48 8D 05 B1 E1 0A 02 C3", "the game's empty value"},
    // the cache's Powertrains table and its lookup, which the guard repeats
    {0x21DEC0, "48 8B 81 A0 03 00 00 C3", "the car database cache's Powertrains table"},
    {0x26F900, "48 8B 41 70 44 8B C2 48 2B 41 68 48 C1 F8 03 4C 3B C0 76 03 33 C0 C3 48 8B 41 68 4A 8B 04 C0 C3",
     "the Powertrains lookup"},
};
// clang-format on
const std::size_t kGuardCodeCheckCount = sizeof kGuardCodeChecks / sizeof kGuardCodeChecks[0];

std::size_t guard_parse_hex(const char* s, uint8_t* out, std::size_t cap) {
    std::size_t n = 0;
    while (*s && n < cap) {
        while (*s == ' ') ++s;
        if (!s[0] || !s[1]) break;
        const char b[3] = {s[0], s[1], 0};
        out[n++]        = static_cast<uint8_t>(std::strtoul(b, nullptr, 16));
        s += 2;
    }
    return n;
}

// ---- what the log is told ---------------------------------------------------------------------------------------

std::string guard_fired_line(uint32_t powertrain, Powertrain why, DbSource source) {
    char b[640];
    std::snprintf(b, sizeof b,
                  "a car's powertrain, %lu, %s, so the game would have stopped at game+0x%X (the drivetrain the Autoshow "
                  "and garages show). The car is shown without its drivetrain instead. The likely cause is a car pack or a "
                  "mod that changes the game database (%s): its Data_Car rows need a PowertrainID that the Powertrains "
                  "table has.",
                  static_cast<unsigned long>(powertrain),
                  why == Powertrain::unreadable ? "couldn't be looked up in the game's Powertrains table"
                                                : "isn't in the game's Powertrains table",
                  static_cast<unsigned>(kRvaFaultSite), db_source_text(source));
    return b;
}

} // namespace fh2platform

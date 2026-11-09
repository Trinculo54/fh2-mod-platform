// Guards for game crashes that a game database missing rows a car needs causes (platform 2.9.0, issue #23).
//
// Car packs, and mods' SQL, can give the game a car whose rows don't all match the game's: a car whose
// Data_Car.PowertrainID isn't an Id in the Powertrains table, say. The game reads its database once at start and
// trusts it: its DriveTrainConverter (the menus' drivetrain text and icon, game+0xB99BE0) looks the powertrain up
// with no check for "not there", and the game stops (an access violation at game+0xB99CD3 reading 0x1) as soon as
// the Autoshow or a garage lists that car. db_guards.cpp turns that into the converter's own "nothing to show"
// answer, so the car is listed without its drivetrain.
//
// The guards go in only when the database the game reads isn't its own (db_source below), and only when every byte
// they rely on is as the exe on disk has it. Nothing changes until the game would have crashed.
//
// The rules have no game in them, so tests/db_guards_test.cpp runs them on their own: where the database comes
// from, the game's powertrain lookup, the code checks' table and the first log line.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct FH2ModApi;

namespace fh2platform {

// ---- where the game's database comes from ---------------------------------------------------------------------

// What's in mediaoverride\Stripped\gamedbRC.slt (database.cpp tells, as it does when it builds).
enum class OverrideDb { none, platform, fh2mp, fh2ml, copy_of_media, copy_of_mediapc, other };

struct DbFacts {
    bool layer = false;                    // this Azimuth folder's own database on the temp drive (only the platform writes it)
    OverrideDb override_db = OverrideDb::none;
    bool mediapc = false;                  // mediapc\Stripped\gamedbRC.slt is there (a car pack's)...
    bool mediapc_same_as_media = false;    // ...and is a plain copy of media's
};

// The one the game reads: the layer first, then mediaoverride, then mediapc, then media. mediaoverride counts
// whenever the file is there, though only Extra Options or the platform's own mount make the game read it: a guard
// in a session that didn't need it only costs two unused hooks.
enum class DbSource { games_own, platform_layer, platform_mediaoverride, fh2mp, fh2ml, other_mod, car_pack_mediapc };

DbSource db_source(const DbFacts& f);
// For platform.log: where it comes from, as the end of a sentence ("the platform built ..."), in English.
const char* db_source_text(DbSource s);

// ---- the game's powertrain lookup -------------------------------------------------------------------------------

// The car database cache (CCarDBCache) is at [game+0x34ABA60]+0x3A0; its Powertrains table, one record pointer per
// Id (std::vector), runs from [+0x68] to [+0x70]. game+0x26F900 returns NULL past the end, but reads one record past
// it for an Id equal to the count (it compares with jbe); the guard counts that Id as missing too.
inline constexpr uintptr_t kRvaCarDbCache    = 0x34ABA60;
inline constexpr uintptr_t kCacheToPowertrains = 0x3A0;
inline constexpr uintptr_t kTableBegin       = 0x68;
inline constexpr uintptr_t kTableEnd         = 0x70;

// Reads `n` bytes at `at` into `out`; false when they can't be read. The platform's reads memory under SEH.
using GuardReadFn = bool (*)(uintptr_t at, void* out, std::size_t n);

enum class Powertrain { known, missing, unreadable };
Powertrain powertrain_lookup(GuardReadFn read, uintptr_t game_base, uint32_t id);

// ---- the code the guard relies on -------------------------------------------------------------------------------

// The converter asks whether the binding wants a string (game+0x154F960, at 0xB99C9B) and, when not, whether it wants
// the other type it can make (at 0xB99D6F). Each answer is tested by "test al,al; je" at these sites, where the guard
// hooks: when the car's powertrain isn't known it makes the answer "no", so the converter goes the way it goes for a
// type it can't make (game+0xB99E37): it releases its parameter and returns the game's empty value.
inline constexpr uint32_t kRvaStringCheck = 0xB99CA0;
inline constexpr uint32_t kRvaOtherCheck  = 0xB99D74;
inline constexpr uint32_t kRvaConverter   = 0xB99BE0;   // DriveTrainConverter's slot 0 (vtable game+0x2DA6938)
inline constexpr uint32_t kRvaFaultSite   = 0xB99CD3;   // where the game stops without the guard

struct GuardCodeCheck {
    uint32_t rva;
    const char* hex;   // the bytes the exe has there, as "48 8B ..."
    const char* what;
};
extern const GuardCodeCheck kGuardCodeChecks[];
extern const std::size_t kGuardCodeCheckCount;

// The bytes each hook replaces: its site and length (whole instructions, 5 or more).
struct GuardPatch {
    uint32_t rva, len;
};
inline constexpr GuardPatch kGuardPatches[] = {{kRvaStringCheck, 8}, {kRvaOtherCheck, 8}};

// "48 8B C4" -> bytes; returns how many (at most `cap`).
std::size_t guard_parse_hex(const char* s, uint8_t* out, std::size_t cap);

// The cars whose Data_Car.PowertrainID the Powertrains table doesn't have (NULL too, which the game reads as a
// number no table has): their media name and the Id, in Id order, at most 20. For platform.log, when the guards go in.
inline constexpr const char* kCarsWithoutPowertrainSql =
    "SELECT MediaName, PowertrainID FROM Data_Car WHERE PowertrainID IS NULL OR PowertrainID NOT IN "
    "(SELECT Id FROM Powertrains) ORDER BY Id LIMIT 20";

// ---- what the player and the log are told -----------------------------------------------------------------------

// platform.log's line the first time a guard fires in a session (one line, no prefix).
std::string guard_fired_line(uint32_t powertrain, Powertrain why, DbSource source);

// ---- the platform's side (db_guards.cpp) --------------------------------------------------------------------------

// Starts the guards' thread: once the game's code is ready, it looks at where the database comes from, checks the
// code and puts the hooks in, or says why not. Call once, after the database is built.
void db_guards_start(const FH2ModApi* api);
// For YOUR MODS: once a guard has fired this session, the Id it fired for and the times it has; false before.
bool db_guard_fired(uint32_t* powertrain, uint32_t* times);

} // namespace fh2platform

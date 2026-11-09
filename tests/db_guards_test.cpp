// The database guards' rules (src/db_guard_rules.cpp), with no game: where the game's database comes from, the
// game's powertrain lookup as the guard repeats it, the query that names the cars, and the line platform.log gets. Given the game's exe
// (forza_durango_release_final.exe), it also checks every byte the guards rely on against it, and that the bytes
// the hooks replace are clear of every released mod's in-memory checks and the platform's own sites.
//   db_guards_test.exe [forza_durango_release_final.exe]
// Exits 0 when every check passes.
#include "db_guards.hpp"

#include <windows.h>

#include "sqlite3.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}
bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

// ---- a fake game's memory: a few regions, anything else unreadable

struct Region {
    uintptr_t at;
    std::vector<uint8_t> bytes;
};
std::vector<Region> g_mem;

bool fake_read(uintptr_t at, void* out, std::size_t n) {
    for (const Region& r : g_mem)
        if (at >= r.at && at + n <= r.at + r.bytes.size()) {
            std::memcpy(out, r.bytes.data() + (at - r.at), n);
            return true;
        }
    return false;
}
void put(uintptr_t at, std::size_t size) { g_mem.push_back({at, std::vector<uint8_t>(size)}); }
void poke(uintptr_t at, uint64_t v) {
    for (Region& r : g_mem)
        if (at >= r.at && at + 8 <= r.at + r.bytes.size()) std::memcpy(r.bytes.data() + (at - r.at), &v, 8);
}

constexpr uintptr_t kBase = 0x140000000ull, kCache = 0x50000000ull, kTable = 0x50010000ull, kRecords = 0x50020000ull;

// The cache, its table, and `n` record pointers: record i is kRecords + 0x100 + 0x40 * i, or NULL where `holes` says.
void make_table(std::size_t n, uint32_t holes = 0) {
    g_mem.clear();
    put(kBase + kRvaCarDbCache, 8);
    put(kCache, 0x400);
    put(kTable, 0x80);
    put(kRecords, 8 * (n + 1));   // one more than the table, as a vector's spare capacity may be
    poke(kBase + kRvaCarDbCache, kCache);
    poke(kCache + kCacheToPowertrains, kTable);
    poke(kTable + kTableBegin, kRecords);
    poke(kTable + kTableEnd, kRecords + 8 * n);
    for (std::size_t i = 0; i < n; ++i) poke(kRecords + 8 * i, holes >> i & 1 ? 0 : kRecords + 0x100 + 0x40 * i);
    poke(kRecords + 8 * n, 0x4141414141414141ull);   // past the end: what the game's jbe would read for Id n
}

// ---- the exe on disk

std::vector<char> g_exe;
const char* exe_at(uint32_t rva, std::size_t n) {
    if (g_exe.size() < 0x1000) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe.data());
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_exe.data() + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (rva >= sec[i].VirtualAddress && rva + n <= sec[i].VirtualAddress + sec[i].SizeOfRawData)
            return g_exe.data() + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
    return nullptr;
}

// Byte ranges released mods compare in memory (UI-TEMPLATES research S5, section 3, and FH2MP 1.2.0's guards near
// car code), and the platform's own hook and patch sites (16 bytes where the length isn't fixed).
struct Range {
    uint32_t at, len;
    const char* who;
};
const Range kChecked[] = {
    {0x323310, 16, "Achievements and FH2MP: MakeDisplayString"}, {0x755330, 15, "Achievements: Show"},
    {0x9224DD, 16, "FH2MP: the remote prediction guard"}, {0x105E530, 10, "FH2MP: the upgrade torque reader"},
    {0x289320, 13, "CCBR"}, {0x28C590, 11, "CCBR"}, {0x28D930, 8, "CCBR"}, {0xF5B420, 10, "CCBR"}, {0xF5B890, 9, "CCBR"},
    {0xF60510, 8, "CCBR"}, {0xF62280, 7, "CCBR"}, {0xF649F0, 16, "CCBR"}, {0xF73080, 9, "CCBR"}, {0x190BF90, 7, "CCBR"},
    {0x190BFD0, 9, "CCBR"}, {0x6A2AF7, 7, "CCBR"}, {0x6B84F2, 7, "CCBR"}, {0x6B8A87, 7, "CCBR"}, {0x6B8EA8, 7, "CCBR"},
    {0x6B965E, 7, "CCBR"}, {0x6B992A, 7, "CCBR"}, {0x2C5CCB0, 8, "CCBR"},
    {0x3229C0, 10, "Universal Radio"}, {0x1018A13, 5, "Universal Radio"}, {0x10221F2, 5, "Universal Radio"},
    {0x2D44F30, 8, "WheresMyBuddy"},
    {0x369690, 10, "WreckingBall"}, {0x36A330, 6, "WreckingBall"}, {0x36AC80, 12, "WreckingBall"}, {0x36AEAC, 9, "WreckingBall"},
    {0x384C00, 10, "WreckingBall"}, {0x12B2500, 6, "WreckingBall"}, {0x12CC010, 11, "WreckingBall"}, {0x12CC1A0, 8, "WreckingBall"},
    {0x12CC730, 12, "WreckingBall"}, {0x12CD300, 11, "WreckingBall"}, {0x13A41E0, 11, "WreckingBall"}, {0x1871B90, 6, "WreckingBall"},
    {0x1881EC0, 6, "WreckingBall"}, {0x2C6A208, 8, "WreckingBall"}, {0x2D4B2E0, 8, "WreckingBall"}, {0x2D4BA98, 8, "WreckingBall"},
    {0x2D4BC60, 8, "WreckingBall"}, {0x2EB88C8, 8, "WreckingBall"}, {0x2F5A3A8, 8, "WreckingBall"},
    {0x157FF60, 16, "platform"}, {0xD9FC10, 16, "platform"}, {0xDC8A60, 16, "platform"}, {0xDC6C40, 16, "platform"},
    {0xDC6E30, 16, "platform"}, {0xD9F9F0, 16, "platform"}, {0xB3A3E0, 16, "platform"}, {0xB0B990, 16, "platform"},
    {0xDBD720, 16, "platform"}, {0xDBDD00, 16, "platform"}, {0xDBDD60, 16, "platform"}, {0xDBFE40, 16, "platform"},
    {0xDBD635, 5, "platform"}, {0xDBF185, 5, "platform"}, {0x75542C, 5, "platform"}, {0x73E0B1, 7, "platform"},
    {0x73F046, 11, "platform"}, {0x3FCD84, 6, "platform"}, {0x10768D0, 16, "platform"}, {0x2DEFE50, 8, "platform"},
    {0x2DEFEC0, 8, "platform"}, {0x2DEF9C8, 8, "platform"}, {0x2DF06C0, 8, "platform"}, {0x2DF07B0, 8, "platform"},
    {0x2E53940, 8, "platform"}, {0x2E6EF28, 8, "platform"}, {0xDDACAD, 16, "platform"}, {0xDDAE1F, 16, "platform"},
    {0xDDB2F5, 16, "platform"}, {0xDDB910, 16, "platform"}, {0xDDBB3E, 16, "platform"},
};

} // namespace

int main(int argc, char** argv) {
    // ---- where the database comes from
    {
        DbFacts f;
        check(db_source(f) == DbSource::games_own, "nothing but media's: the game's own");
        f.mediapc = true;
        check(db_source(f) == DbSource::car_pack_mediapc, "a database in mediapc that isn't media's: a car pack's");
        f.mediapc_same_as_media = true;
        check(db_source(f) == DbSource::games_own, "a plain copy of media's in mediapc: the game's own");
        f = {};
        f.override_db = OverrideDb::copy_of_media;
        f.mediapc     = true;
        check(db_source(f) == DbSource::games_own, "a plain copy of media's in mediaoverride hides mediapc's: the game's own");
        f.override_db = OverrideDb::copy_of_mediapc;
        check(db_source(f) == DbSource::car_pack_mediapc, "a copy of a car pack's mediapc database in mediaoverride: the car pack's");
        f.override_db = OverrideDb::platform;
        check(db_source(f) == DbSource::platform_mediaoverride, "the platform's copy in mediaoverride");
        f.override_db = OverrideDb::fh2mp;
        check(db_source(f) == DbSource::fh2mp, "FH2MP's copy of the server's");
        f.override_db = OverrideDb::fh2ml;
        check(db_source(f) == DbSource::fh2ml, "FH2ML's");
        f.override_db = OverrideDb::other;
        check(db_source(f) == DbSource::other_mod, "another mod's in mediaoverride");
        f.layer = true;
        check(db_source(f) == DbSource::platform_layer, "this folder's own on the temp drive comes before them all");
        check(std::string(db_source_text(DbSource::games_own)) == "the game reads its own database", "the log says where it comes from");
    }

    // ---- the powertrain lookup
    {
        make_table(8, 1u << 3);
        check(powertrain_lookup(&fake_read, kBase, 0) == Powertrain::known, "Id 0 of 8: known");
        check(powertrain_lookup(&fake_read, kBase, 7) == Powertrain::known, "Id 7 of 8: known");
        check(powertrain_lookup(&fake_read, kBase, 3) == Powertrain::missing, "a hole (Id 3, a NULL record): missing");
        check(powertrain_lookup(&fake_read, kBase, 8) == Powertrain::missing,
              "Id 8 of 8 (the game would read past the end, jbe): missing");
        check(powertrain_lookup(&fake_read, kBase, 9) == Powertrain::missing, "Id 9 of 8: missing");
        check(powertrain_lookup(&fake_read, kBase, 0x7FFFFFFF) == Powertrain::missing, "INT_MAX (a value the game reads for NULL): missing");
        check(powertrain_lookup(&fake_read, kBase, 0xFFFFFFFF) == Powertrain::missing, "-1: missing");
        poke(kBase + kRvaCarDbCache, 0);
        check(powertrain_lookup(&fake_read, kBase, 1) == Powertrain::unreadable, "no cache yet: unreadable");
        make_table(8);
        poke(kCache + kCacheToPowertrains, 0x60000000ull);
        check(powertrain_lookup(&fake_read, kBase, 1) == Powertrain::unreadable, "a table that can't be read: unreadable");
        make_table(8);
        poke(kTable + kTableEnd, kRecords - 8);
        check(powertrain_lookup(&fake_read, kBase, 1) == Powertrain::unreadable, "an end before the start: unreadable");
        make_table(0);
        check(powertrain_lookup(&fake_read, kBase, 0) == Powertrain::missing, "an empty table: missing");
    }

    // ---- the line platform.log gets
    {
        const std::string l = guard_fired_line(12, Powertrain::missing, DbSource::platform_layer);
        std::printf("     %s\n", l.c_str());
        check(has(l, "a car's powertrain, 12, isn't in the game's Powertrains table"), "the line names the Id");
        check(has(l, "game+0xB99CD3"), "the line names where the game would have stopped");
        check(has(l, "The likely cause is a car pack or a mod that changes the game database"), "the line names the likely cause");
        check(has(l, "(the platform built this Azimuth folder's database with car packs' or mods' changes)"),
              "the line says where the database comes from");
        check(has(guard_fired_line(1, Powertrain::unreadable, DbSource::fh2mp), "couldn't be looked up"),
              "a table that can't be read is said so");
    }

    // ---- the cars the log names: the query, on a small database
    {
        sqlite3* db = nullptr;
        sqlite3_open(":memory:", &db);
        const char* setup = "CREATE TABLE Powertrains (Id INTEGER PRIMARY KEY, DrivetrainId INT);"
                            "INSERT INTO Powertrains VALUES (0,1),(1,2),(2,2),(3,2),(4,3),(5,3),(6,3),(7,2);"
                            "CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, MediaName TEXT, PowertrainID INT);"
                            "INSERT INTO Data_Car VALUES (2155,'FOR_MustangGT_15',1),(3000,'PACK_Car_A',12),(3001,'PACK_Car_B',NULL),"
                            "(3002,'PACK_Car_C',7),(2000,'ASM_V12VantageS_13',-1);";
        check(sqlite3_exec(db, setup, nullptr, nullptr, nullptr) == SQLITE_OK, "a small database with Powertrains 0 to 7");
        std::string found;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, kCarsWithoutPowertrainSql, -1, &st, nullptr) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW)
                found += std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, 0))) + "=" +
                         (sqlite3_column_type(st, 1) == SQLITE_NULL ? "null" : std::to_string(sqlite3_column_int(st, 1))) + " ";
        sqlite3_finalize(st);
        std::printf("     %s\n", found.c_str());
        check(found == "ASM_V12VantageS_13=-1 PACK_Car_A=12 PACK_Car_B=null ",
              "the cars whose powertrain isn't in the table, NULL too, in Id order; the others not");
        sqlite3_close(db);
    }

    // ---- the code checks' table
    {
        bool parsed = kGuardCodeCheckCount >= 9;
        for (std::size_t i = 0; i < kGuardCodeCheckCount; ++i) {
            uint8_t b[256];
            const std::size_t n = guard_parse_hex(kGuardCodeChecks[i].hex, b, sizeof b);
            parsed = parsed && n > 0 && n < 256 && std::strlen(kGuardCodeChecks[i].hex) == 3 * n - 1;
        }
        check(parsed, "every code check's bytes parse, each under 256 (disk_bytes_equal's limit)");
        bool covered = true;
        for (const GuardPatch& p : kGuardPatches) {
            bool in = false;
            for (std::size_t i = 0; i < kGuardCodeCheckCount; ++i) {
                uint8_t b[256];
                const std::size_t n = guard_parse_hex(kGuardCodeChecks[i].hex, b, sizeof b);
                in = in || (p.rva >= kGuardCodeChecks[i].rva && p.rva + p.len <= kGuardCodeChecks[i].rva + n);
            }
            covered = covered && in;
        }
        check(covered, "the bytes each hook replaces are inside a code check");
        bool clear = true;
        for (const GuardPatch& p : kGuardPatches)
            for (const Range& r : kChecked)
                if (p.rva < r.at + r.len && r.at < p.rva + p.len) {
                    std::printf("     game+0x%X overlaps %s's game+0x%X\n", p.rva, r.who, r.at);
                    clear = false;
                }
        check(clear, "the bytes the hooks replace are clear of every released mod's in-memory checks and the platform's own sites");
    }

    // ---- against the exe on disk
    if (argc > 1) {
        std::ifstream in{argv[1], std::ios::binary};
        g_exe.assign(std::istreambuf_iterator<char>(in), {});
        const auto* nt = g_exe.size() > 0x1000 ? reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                                                     g_exe.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe.data())->e_lfanew)
                                               : nullptr;
        check(nt && nt->FileHeader.TimeDateStamp == 0x565501ED && nt->OptionalHeader.SizeOfImage == 0x3A18000,
              "the exe is the 24 Nov 2015 build");
        int bad = 0;
        for (std::size_t i = 0; i < kGuardCodeCheckCount; ++i) {
            const GuardCodeCheck& c = kGuardCodeChecks[i];
            uint8_t b[256];
            const std::size_t n = guard_parse_hex(c.hex, b, sizeof b);
            const char* p       = exe_at(c.rva, n);
            if (!p || std::memcmp(p, b, n) != 0) {
                std::printf("FAIL game+0x%X (%s) isn't what the exe has\n", c.rva, c.what);
                ++bad;
            }
        }
        check(bad == 0, "every byte the guards rely on is what the exe has");
        // Each site: test al,al (84 C0); je rel32 (0F 84), 8 bytes of whole instructions, and where each goes.
        const char* s1 = exe_at(kRvaStringCheck, 8);
        const char* s2 = exe_at(kRvaOtherCheck, 8);
        int32_t r1 = 0, r2 = 0;
        if (s1) std::memcpy(&r1, s1 + 4, 4);
        if (s2) std::memcpy(&r2, s2 + 4, 4);
        check(s1 && std::memcmp(s1, "\x84\xC0\x0F\x84", 4) == 0 && kRvaStringCheck + 8 + r1 == 0xB99D62,
              "the first site is test al,al; je game+0xB99D62 (the other type's check)");
        check(s2 && std::memcmp(s2, "\x84\xC0\x0F\x84", 4) == 0 && kRvaOtherCheck + 8 + r2 == 0xB99E37,
              "the second site is test al,al; je game+0xB99E37 (the way out for a type it can't make)");
        const char* f = exe_at(kRvaFaultSite, 5);
        check(f && std::memcmp(f, "\x44\x0F\xBE\x40\x01", 5) == 0, "game+0xB99CD3 is the read that faults: movsx r8d, byte [rax+1]");
    } else {
        std::printf("     (no exe given: its code isn't checked)\n");
    }

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

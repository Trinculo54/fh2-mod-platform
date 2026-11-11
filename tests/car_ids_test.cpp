// The Ids car packs' cars are given (src/car_ids.cpp), with no game: a small database with the tables a pack
// car takes rows in, and packs and mods with SQL. Exits 0 when every check passes.
#include "car_ids.hpp"
#include "text.hpp"

#include "sqlite3.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace fh2platform::car_ids;
using fh2platform::english;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

void exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::printf("FAIL couldn't run test SQL: %s\n", err ? err : "?");
        ++g_failed;
    }
    sqlite3_free(err);
}

std::string q(sqlite3* db, const std::string& sql) {
    std::string out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        if (const auto* t = sqlite3_column_text(st, 0)) out = reinterpret_cast<const char*>(t);
    sqlite3_finalize(st);
    return out;
}

// The game's tables a pack car takes rows in (as the game declares them), a few it doesn't, and rows of the
// game's own kind: car 441 with engine 195, drivetrain 5000 with its rows 5000000 up (the game has those), and
// GraffitiPoints, whose Ids from 3000 up aren't cars'.
sqlite3* game(const std::string& more = "") {
    sqlite3* db = nullptr;
    sqlite3_open(":memory:", &db);
    exec(db, "CREATE TABLE Data_Car ([Id] INTEGER PRIMARY KEY, [MediaName] TEXT);"
             "CREATE TABLE Data_Engine ([EngineID] INTEGER PRIMARY KEY, [MediaName] TEXT);"
             "CREATE TABLE Data_Drivetrain ([DrivetrainID] INTEGER PRIMARY KEY);"
             "CREATE TABLE Data_Car_Buckets ([CarId] INTEGER PRIMARY KEY);"
             "CREATE TABLE CameraOverrides ([CarId] INTEGER PRIMARY KEY);"
             "CREATE TABLE CarExceptions ([CarID] INTEGER PRIMARY KEY);"
             "CREATE TABLE Data_CarBody ([Id] INTEGER PRIMARY KEY);"
             "CREATE TABLE List_UpgradeEngine ([Id] INTEGER PRIMARY KEY, [Ordinal] INT, [EngineID] INT);"
             "CREATE TABLE List_TorqueCurve ([TorqueCurveID] INTEGER PRIMARY KEY);"
             "CREATE TABLE CarPartPositions ([ID] INT, [Ordinal] INT);"
             "CREATE TABLE TrafficCars ([CarId] INTEGER PRIMARY KEY);"
             "CREATE TABLE GraffitiPoints ([ID] INTEGER PRIMARY KEY);"
             "INSERT INTO Data_Car VALUES (441, 'NIS_SilviaSpecR_00'), (2501, 'FOR_LastCar');"
             "INSERT INTO Data_Engine VALUES (195, 'SR20'), (2631, 'Last');"
             "INSERT INTO Data_Drivetrain VALUES (1), (5000);"
             "INSERT INTO Data_CarBody VALUES (441000), (2501000);"
             "INSERT INTO List_UpgradeEngine VALUES (441000, 441, 195), (5000000, 0, 0), (5000004, 0, 0);"
             "INSERT INTO List_TorqueCurve VALUES (195000), (195001);"
             "INSERT INTO GraffitiPoints VALUES (3000), (3001), (3002), (8195);" +
                 more);
    return db;
}

// A pack car's rows, as make_car_pack.py writes them.
std::string pack_sql(const std::string& car) {
    const std::string c = "{car:" + car + "}";
    return "-- " + car + ": the platform puts the car's Id where it says " + c + ".\n"
           "INSERT INTO Data_Car VALUES (" + c + ", '" + car + "');\n"
           "INSERT INTO Data_Engine VALUES (" + c + ", '" + car + "');\n"
           "INSERT INTO Data_Drivetrain VALUES (" + c + ");\n"
           "INSERT INTO Data_Car_Buckets VALUES (" + c + ");\n"
           "INSERT INTO CameraOverrides VALUES (" + c + ");\n"
           "INSERT INTO CarExceptions VALUES (" + c + ");\n"
           "INSERT INTO Data_CarBody VALUES (" + c + " * 1000);\n"
           "INSERT INTO List_UpgradeEngine VALUES (" + c + " * 1000, " + c + ", " + c + ");\n"
           "INSERT INTO List_TorqueCurve SELECT " + c + " * 1000 + 2;\n"
           "INSERT INTO CarPartPositions VALUES (1, " + c + ");\n";
}

struct Src {
    std::string name;
    std::vector<std::string> sql, cars, wanted; // wanted: pack.ini's id= for each car
};
Src pack(const std::string& folder, const std::string& car, std::string sql = "") {
    return {"car pack " + folder, {sql.empty() ? pack_sql(car) : sql}, {car}, {}};
}
Src mod(const std::string& name, const std::string& sql) { return {name, {sql}, {}, {}}; }
// A pack whose pack.ini gives its car `id =`.
Src asking(const std::string& folder, const std::string& car, const std::string& id) {
    Src p = pack(folder, car);
    p.wanted = {id};
    return p;
}

const std::string kShown = "media\\Stripped\\gamedbRC.slt";

std::vector<Outcome> run(sqlite3* db, const std::vector<Src>& s, const std::map<std::string, int64_t>& record = {}) {
    std::vector<Source> v;
    for (const auto& x : s) v.push_back({x.name, &x.sql, x.cars.empty() ? nullptr : &x.cars, x.wanted.empty() ? nullptr : &x.wanted});
    return plan(db, kShown, v, record);
}

std::string warnings(const Outcome& o) {
    std::string w;
    for (const auto& x : o.warnings) w += english(x) + " | ";   // messages for the MODS tab, in English
    return w;
}

// The build as database.cpp makes it: each source in a transaction, a pack's {car:...} replaced by its cars'
// Ids, a pack that's left out not run. -> whether each one's changes were made.
std::vector<bool> build(sqlite3* db, const std::vector<Src>& s, const std::vector<Outcome>& o) {
    std::vector<bool> made;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!s[i].cars.empty() && !o[i].problem.empty()) {
            made.push_back(false);
            continue;
        }
        bool ok = sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
        for (std::string t : s[i].sql) {
            for (const auto& [car, id] : o[i].ids) {
                const std::string tag = "{car:" + lower(car) + "}", value = std::to_string(id);
                for (std::size_t p = 0; (p = lower(t).find(tag, p)) != std::string::npos; p += value.size()) t.replace(p, tag.size(), value);
            }
            if (ok) ok = sqlite3_exec(db, t.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK;
        }
        if (ok) ok = sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
        if (!ok) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        made.push_back(ok);
    }
    return made;
}

bool all(const std::vector<bool>& v) {
    for (bool b : v)
        if (!b) return false;
    return true;
}

using Ids = std::vector<std::pair<std::string, int64_t>>;

void plain() {
    std::printf("\n---- a pack and a mod that don't hard-code Ids\n");
    sqlite3* db = game();
    const std::vector<Src> s = {pack("a", "PACK_A"), mod("Prices", "UPDATE Data_Car SET MediaName = MediaName;")};
    const auto o = run(db, s);
    check(o[0].ids == Ids{{"PACK_A", 3000}} && o[0].given == o[0].ids, "a new car gets 3000 (GraffitiPoints' 3000 isn't a car's)");
    check(o[0].problem.empty() && o[0].warnings.empty() && o[1].warnings.empty(), "no problem, no warning");
    check(q(db, "SELECT count(*) FROM Data_Car") == "2", "the database built on is only read");
    check(all(build(db, s, o)) && q(db, "SELECT MediaName FROM Data_Car WHERE Id = 3000") == "PACK_A", "the build makes both");
    sqlite3_close(db);
}

// Gap 1: a mod's SQL adds a car with an Id of its own.
void mods_ids() {
    std::printf("\n---- a mod adds a car with an Id from 3000 up\n");
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"), mod("HandMadeCar", "INSERT INTO Data_Car VALUES (3000, 'HAND_Car01');")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3001}}, "the pack's new car gets 3001, not the mod's 3000");
        check(has(warnings(o[1]), "Data_Car 3000") && has(warnings(o[1]), "from 9000 up"), "the mod is warned: " + warnings(o[1]));
        check(o[0].warnings.empty(), "the pack isn't");
        check(all(build(db, s, o)), "the build makes both (a plain INSERT of 3000 would have failed)");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"), mod("Replacer", "INSERT OR REPLACE INTO Data_Car VALUES (3000, 'HAND_Car01');")};
        const auto o = run(db, s);
        build(db, s, o);
        check(o[0].ids == Ids{{"PACK_A", 3001}} && q(db, "SELECT MediaName FROM Data_Car WHERE Id = 3001") == "PACK_A" &&
                  q(db, "SELECT MediaName FROM Data_Car WHERE Id = 3000") == "HAND_Car01",
              "INSERT OR REPLACE: the pack's car isn't written over");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"),
                                    mod("Copier", "CREATE TEMP TABLE c AS SELECT * FROM Data_Car WHERE Id = 441;"
                                                  "UPDATE c SET Id = 3000, MediaName = 'HAND_Copy';"
                                                  "INSERT INTO Data_Car SELECT * FROM c; DROP TABLE c;"
                                                  "INSERT INTO List_UpgradeEngine VALUES (3000000, 3000, 195), (3001001, 3000, 195);")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3002}}, "a car copied by way of a temp table, and rows 3001xxx: the pack's car gets 3002");
        check(has(warnings(o[1]), "Data_Car 3000, List_UpgradeEngine 3000000 and 1 more"), "the warning lists them: " + warnings(o[1]));
        check(all(build(db, s, o)), "the build makes both");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"),
                                    mod("Reserved", "INSERT INTO Data_Car VALUES (9000, 'HAND_Car01');"
                                                    "INSERT INTO List_UpgradeEngine VALUES (9000000, 9000, 195);")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[1].warnings.empty(), "a mod's car 9000: no clash, no warning");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"),
                                    mod("Broken", "INSERT INTO Data_Car VALUES (3000, 'HAND_Car01'); UPDATE NoSuchTable SET x = 1;")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[1].warnings.empty(),
              "a mod whose SQL fails makes no change (as when it runs for real), so 3000 is free");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"), mod("Next", "INSERT INTO Data_Car SELECT max(Id) + 1, 'HAND_Next' FROM Data_Car;")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[1].warnings.empty(), "a mod that adds max(Id) + 1 (2502 here) isn't warned");
        check(all(build(db, s, o)) && q(db, "SELECT MediaName FROM Data_Car WHERE Id = 3001") == "HAND_Next",
              "the build makes both (the mod's car comes after the pack's)");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game("INSERT INTO Data_Car VALUES (3500, 'OTHER_ModsCar');");
        const auto o = run(db, {pack("a", "PACK_A"), mod("Touch", "UPDATE Data_Car SET MediaName = MediaName;")});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[1].warnings.empty(),
              "a mod that changes every car, 3500 of the database built on too, isn't warned");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {mod("Early", "INSERT INTO Data_Car VALUES (3000, 'HAND_A');"),
                                    mod("Late", "INSERT INTO Data_Car VALUES (3001, 'HAND_B');"), pack("a", "PACK_A")};
        const auto o = run(db, s);
        check(o[2].ids == Ids{{"PACK_A", 3002}} && !o[0].warnings.empty() && !o[1].warnings.empty(), "two mods' cars: 3002, both warned");
        sqlite3_close(db);
    }
}

// Recorded Ids stay; the pack is left out when another source has their keys now.
void recorded() {
    std::printf("\n---- a recorded Id that's taken now\n");
    const std::map<std::string, int64_t> rec = {{"pack_a", 3000}};
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"), mod("HandMadeCar", "INSERT INTO Data_Car VALUES (3000, 'HAND_Car01');")};
        const auto o = run(db, s, rec);
        check(english(o[0].problem) == "PACK_A's Id, 3000, is taken now: HandMadeCar's SQL adds Data_Car 3000.", "left out: " + english(o[0].problem));
        check(o[0].ids.empty() && o[0].given.empty(), "no Ids for it");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game("INSERT INTO Data_Engine VALUES (3000, 'Another');");
        const auto o = run(db, {pack("a", "PACK_A")}, rec);
        check(english(o[0].problem) == "PACK_A's Id, 3000, is taken now: " + kShown + " has Data_Engine 3000.",
              "the database built on has its engine: " + english(o[0].problem));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {pack("a", "PACK_A"), mod("Torque", "INSERT INTO List_TorqueCurve VALUES (3000005);")}, rec);
        check(has(english(o[0].problem), "Torque's SQL adds List_TorqueCurve 3000005"), "a row 3000xxx: " + english(o[0].problem));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"),
                                    mod("Refers", "INSERT INTO CarPartPositions VALUES (9, 3000); INSERT INTO TrafficCars VALUES (3000);"
                                                  "UPDATE Data_Car SET MediaName = MediaName WHERE Id = 3000;")};
        const auto o = run(db, s, rec);
        check(o[0].problem.empty() && o[0].ids == Ids{{"PACK_A", 3000}} && o[0].given.empty(),
              "a mod that only names car 3000 (CarPartPositions, TrafficCars, an UPDATE) doesn't take it");
        check(o[1].warnings.empty(), "and isn't warned");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::vector<Src> s = {pack("a", "PACK_A"), {"car pack literal", {"INSERT INTO Data_Car VALUES (3000, 'LIT_Car');"}, {}}};
        const auto o = run(db, s, rec);
        check(english(o[0].problem) == "PACK_A's Id, 3000, is taken now: car pack literal's SQL adds Data_Car 3000.",
              "a pack without cars that writes 3000: " + english(o[0].problem));
        check(has(warnings(o[1]), "Data_Car 3000"), "and that pack is warned");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {pack("a", "PACK_A"), pack("b", "PACK_B")}, {{"pack_a", 3000}, {"pack_b", 3000}});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && english(o[1].problem) == "PACK_B's Id, 3000, is PACK_A's too (fh2-mods\\platform-car-ids.txt "
                                                                    "gives both the same one).",
              "two cars recorded with the same Id: " + english(o[1].problem));
        check(!o[1].base_unread, "that isn't a reason that passes on its own");
        sqlite3_close(db);
    }
    {
        // The database built on couldn't be read: every pack with cars is left out, for a reason that can pass on its
        // own (database.cpp keeps the last database it built, with their cars, rather than one without them).
        const auto o = run(nullptr, {pack("a", "PACK_A")}, {{"pack_a", 3000}});
        check(english(o[0].problem) == "The cars of " + kShown + " couldn't be read." && o[0].base_unread && o[0].ids.empty(),
              "an unreadable database: " + english(o[0].problem));
    }
}

// Gap 2: the database has a pack car's other keys, but no car with that Id.
void other_keys() {
    std::printf("\n---- keys a pack car takes besides Data_Car\n");
    {
        sqlite3* db = game("INSERT INTO Data_Engine VALUES (3000, 'e');"
                           "INSERT INTO List_UpgradeEngine VALUES (3001002, 0, 0);"
                           "INSERT INTO Data_CarBody VALUES (3002000);"
                           "INSERT INTO CameraOverrides VALUES (3003);"
                           "INSERT INTO Data_Drivetrain VALUES (3004);"
                           "INSERT INTO List_TorqueCurve VALUES (3005999);"
                           "INSERT INTO CarExceptions VALUES (3006);"
                           "INSERT INTO Data_Car_Buckets VALUES (3007);");
        const std::vector<Src> s = {pack("a", "PACK_A")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 3008}}, "engine 3000, rows 3001xxx, body 3002000, camera 3003, drivetrain 3004, "
                                                 "torque curve 3005999, exceptions 3006, bucket 3007: the car gets 3008");
        check(all(build(db, s, o)), "and its rows all go in");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        std::map<std::string, int64_t> rec;
        for (int64_t id = 3000; id < 5000; ++id) rec["gone_" + std::to_string(id)] = id;
        const std::vector<Src> s = {pack("a", "PACK_A")};
        const auto o = run(db, s, rec);
        check(o[0].ids == Ids{{"PACK_A", 5001}}, "drivetrain 5000 and its rows (the game's own) are skipped: 5001");
        check(all(build(db, s, o)), "and its rows all go in");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        std::map<std::string, int64_t> rec;
        for (int64_t id = 3000; id < 9000; ++id) rec["gone_" + std::to_string(id)] = id;
        const auto o = run(db, {pack("a", "PACK_A")}, rec);
        check(english(o[0].problem) == "No car Id from 3000 to 8999 is free for PACK_A.", "none free below 9000: " + english(o[0].problem));
        sqlite3_close(db);
    }
}

// Gap 3: a pack that lists cars but writes Ids of its own.
void literal_packs() {
    std::printf("\n---- a pack that lists cars but writes numbers\n");
    const std::string literal = "INSERT INTO Data_Car VALUES (3000, 'PACK_B');";
    {
        sqlite3* db = game();
        const auto o = run(db, {pack("b", "PACK_B", literal), pack("c", "PACK_C")});
        check(has(warnings(o[0]), "Its SQL never says {car:PACK_B} where PACK_B's Id goes"), "warned: no {car:PACK_B}");
        check(has(warnings(o[0]), "Data_Car 3000"), "warned: its own Ids from 3000 up");
        check(o[0].ids == Ids{{"PACK_B", 3001}} && o[1].ids == Ids{{"PACK_C", 3002}}, "its 3000 is taken: it gets 3001, the next pack 3002");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {pack("b", "PACK_B", literal)}, {{"pack_b", 3000}});
        check(o[0].problem.empty() && o[0].ids == Ids{{"PACK_B", 3000}}, "its own rows don't take its recorded Id from it");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const std::string mixed = pack_sql("PACK_A") + "UPDATE NoSuchTable SET x = 1;\nINSERT INTO List_TorqueCurve VALUES (3000001);\n";
        const auto o = run(db, {pack("a", "PACK_A", mixed)});
        check(o[0].ids == Ids{{"PACK_A", 3001}}, "a literal row among {car:...} statements is seen, after one that fails: 3001");
        check(o[0].warnings.size() == 1 && has(warnings(o[0]), "List_TorqueCurve 3000001"), "warned for it only: " + warnings(o[0]));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        std::string sql = pack_sql("PACK_A");
        for (std::size_t p = 0; (p = sql.find("{car:PACK_A}", p)) != std::string::npos; ++p) sql.replace(p, 12, "{CAR:pack_a}");
        const auto o = run(db, {pack("a", "PACK_A", sql)});
        check(o[0].warnings.empty() && o[0].ids == Ids{{"PACK_A", 3000}}, "{CAR:pack_a} counts (case doesn't matter)");
        sqlite3_close(db);
    }
}

// The Id a car's pack.ini asks for (id=): the same on every install that has the same packs.
void asked_ids() {
    std::printf("\n---- the Id pack.ini asks for\n");
    {
        sqlite3* db = game();
        const std::vector<Src> s = {asking("a", "PACK_A", "4000")};
        const auto o = run(db, s);
        check(o[0].ids == Ids{{"PACK_A", 4000}} && o[0].given == o[0].ids && o[0].warnings.empty(), "a new car gets the 4000 it asks for");
        check(all(build(db, s, o)) && q(db, "SELECT MediaName FROM Data_Car WHERE Id = 4000") == "PACK_A", "and its rows go in");
        sqlite3_close(db);
    }
    {
        // Install 1 has pack b first, then pack a too; install 2 gets both at once: the same Ids either way.
        sqlite3* db = game();
        const Src a = pack("a", "PACK_A"), b = asking("b", "PACK_B", "3000");
        const auto first = run(db, {b});
        const auto later = run(db, {a, b}, {{"pack_b", 3000}});
        const auto both  = run(db, {a, b});
        check(first[0].ids == Ids{{"PACK_B", 3000}} && later[0].ids == Ids{{"PACK_A", 3001}},
              "install 1: pack b's car gets the 3000 it asks for, then pack a's 3001");
        check(both[0].ids == Ids{{"PACK_A", 3001}} && both[1].ids == Ids{{"PACK_B", 3000}},
              "install 2, both at once: pack a's car (first by name) doesn't take the 3000 pack b's asks for");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {asking("a", "PACK_A", "4000"), mod("HandMadeCar", "INSERT INTO Data_Car VALUES (4000, 'HAND_Car01');")});
        check(o[0].ids == Ids{{"PACK_A", 3000}}, "taken by a mod's car: it gets 3000");
        check(has(warnings(o[0]), "PACK_A asks for Id 4000 in pack.ini, but HandMadeCar's SQL adds Data_Car 4000, so it's car 3000 here: "
                                  "a save from an install where it's 4000 doesn't show it here"),
              "and the pack is warned: " + warnings(o[0]));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game("INSERT INTO Data_Engine VALUES (4000, 'e');");
        const auto o = run(db, {asking("a", "PACK_A", "4000")});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && has(warnings(o[0]), "but " + kShown + " has Data_Engine 4000"),
              "engine 4000 in the database built on: " + warnings(o[0]));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {asking("a", "PACK_A", "4000")}, {{"gone_car", 4000}});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && has(warnings(o[0]), "but fh2-mods\\platform-car-ids.txt gives it to another car"),
              "recorded for a car from a pack that's gone: " + warnings(o[0]));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {asking("one", "PACK_A", "4000"), asking("two", "PACK_B", "4000")});
        check(o[0].ids == Ids{{"PACK_A", 4000}} && o[1].ids == Ids{{"PACK_B", 3000}} && has(warnings(o[1]), "but PACK_A has it"),
              "two packs ask for 4000: the first by name gets it, the other is warned");
        Src both = {"car pack x", {pack_sql("PACK_C") + pack_sql("PACK_D")}, {"PACK_C", "PACK_D"}, {"5100", "5100"}};
        const auto p = run(db, {both});
        check(p[0].ids == Ids{{"PACK_C", 5100}, {"PACK_D", 3000}} && has(warnings(p[0]), "PACK_D asks for Id 5100 in pack.ini, but PACK_C has it"),
              "two cars of one pack ask for the same one");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {asking("a", "PACK_A", "4000")}, {{"pack_a", 3000}});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[0].given.empty() && o[0].warnings.empty(), "a car given 3000 before keeps it");
        check(o[0].notes.size() == 1 && has(o[0].notes[0], "PACK_A keeps car 3000, which it was given before, rather than the 4000"),
              "and the log says so");
        sqlite3_close(db);
    }
    for (const char* bad : {"12", "9000", "abc", "-3500", "3500.5", "3500 "}) {
        sqlite3* db = game();
        const auto o = run(db, {asking("a", "PACK_A", bad)});
        check(o[0].ids == Ids{{"PACK_A", 3000}} &&
                  has(warnings(o[0]), std::string("pack.ini's id for PACK_A, ") + bad + ", isn't an Id from 3000 to 8999"),
              std::string("id = ") + bad + ": ignored, with a warning");
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        Src x = {"car pack x", {pack_sql("NEW_Car") + pack_sql("NIS_SilviaSpecR_00")}, {"NEW_Car", "NIS_SilviaSpecR_00"}, {"3000", ""}};
        const auto o = run(db, {x, mod("HandMadeCar", "INSERT INTO Data_Car VALUES (3000, 'HAND_Car01');")});
        check(!o[0].problem.empty() && o[0].warnings.empty(), "a pack that's left out isn't warned about the Ids it asks for");
        sqlite3_close(db);
    }
}

void as_before() {
    std::printf("\n---- left out as before\n");
    {
        sqlite3* db = game();
        const auto o = run(db, {pack("one", "PACK_A"), pack("two", "PACK_A")});
        check(o[0].ids == Ids{{"PACK_A", 3000}} && english(o[1].problem) == "PACK_A is car pack one's too.", "two packs add one car: " + english(o[1].problem));
        sqlite3_close(db);
    }
    {
        sqlite3* db = game();
        const auto o = run(db, {{"car pack x", {pack_sql("NEW_Car") + pack_sql("NIS_SilviaSpecR_00")}, {"NEW_Car", "NIS_SilviaSpecR_00"}},
                                pack("a", "PACK_A")});
        check(english(o[0].problem) == "The game already has NIS_SilviaSpecR_00." && o[0].given.empty(),
              "the game has one of its cars: " + english(o[0].problem));
        check(o[1].ids == Ids{{"PACK_A", 3000}}, "a pack left out keeps no new Id: the next one gets 3000");
        sqlite3_close(db);
    }
    {
        // Ids that Extra Options' own mods add later (reserved): no new car gets one; a recorded one stays, unless
        // that mod shares rows by Id.
        const std::vector<Src> s = {pack("a", "PACK_A"), asking("b", "PACK_B", "3001")};
        std::vector<Source> v;
        for (const auto& x : s) v.push_back({x.name, &x.sql, &x.cars, x.wanted.empty() ? nullptr : &x.wanted});
        const std::map<int64_t, Reserved> eo = {{3000, {"Data_Car 3000 in its mod M", false}}, {3001, {"Data_Car 3001 in its mod M", true}}};
        sqlite3* db = game();
        auto o = plan(db, kShown, v, {}, eo);
        check(o[0].ids == Ids{{"PACK_A", 3002}} && o[1].ids == Ids{{"PACK_B", 3003}}, "new cars skip Extra Options' mods' Ids");
        check(has(warnings(o[1]), "PACK_B asks for Id 3001 in pack.ini, but Extra Options has Data_Car 3001 in its mod M, so it's car 3003 here"),
              "and the one that asked for it is told why: " + warnings(o[1]));
        o = plan(db, kShown, v, {{"pack_a", 3000}, {"pack_b", 3001}}, eo);
        check(o[0].ids == Ids{{"PACK_A", 3000}} && o[0].problem.empty() && o[0].notes.size() == 1, "a recorded Id stays when the mod doesn't share rows");
        check(english(o[1].problem) == "PACK_B's Id, 3001, is taken now: Extra Options has Data_Car 3001 in its mod M.",
              "and its pack is left out when it does: " + english(o[1].problem));
        sqlite3_close(db);
    }
    {
        const std::vector<Src> s = {pack("a", "PACK_A")};
        std::vector<Source> v = {{s[0].name, &s[0].sql, &s[0].cars}};
        const auto o = plan(nullptr, kShown, v, {});
        check(english(o[0].problem) == "The cars of " + kShown + " couldn't be read.", "no database: " + english(o[0].problem));
    }
    {
        sqlite3* db = game();
        const std::filesystem::path f = "car_ids_attach.db";
        std::error_code ec;
        std::filesystem::remove(f, ec);
        const auto o = run(db, {pack("a", "PACK_A"), mod("Attacher", "ATTACH 'car_ids_attach.db' AS x; CREATE TABLE x.t (a);")});
        check(!std::filesystem::exists(f, ec), "a mod's ATTACH writes no file while Ids are picked");
        check(o[0].ids == Ids{{"PACK_A", 3000}}, "and the pack's car still gets 3000");
        sqlite3_close(db);
    }
}

void statements() {
    std::printf("\n---- split_statements\n");
    check(split_statements("SELECT 1; SELECT 2;").size() == 2, "two statements");
    const auto s = split_statements("INSERT INTO t VALUES ('a;b'); -- c;d\nSELECT 1;");
    check(s.size() == 2 && has(s[0], "'a;b'") && has(s[1], "SELECT 1;"), "a ';' in a string or a comment doesn't split");
    check(split_statements("CREATE TRIGGER x AFTER INSERT ON t BEGIN UPDATE t SET a = 1; END; SELECT 1;").size() == 2,
          "a trigger is one statement");
    const auto r = split_statements("SELECT 1; SELECT {car:X} * 1000");
    check(r.size() == 2 && r[1] == " SELECT {car:X} * 1000", "the rest without a ';' comes last");
    check(split_statements("SELECT 1;\r\n  ").size() == 1, "spaces after the last aren't one");
}

} // namespace

int main() {
    plain();
    mods_ids();
    recorded();
    other_keys();
    literal_packs();
    asked_ids();
    as_before();
    statements();
    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed;
}

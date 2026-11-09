// The Ids car packs' cars are given (car_ids.cpp), on their own: database.cpp reads and writes
// fh2-mods\platform-car-ids.txt and puts the Ids into the packs' SQL; this decides them.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

namespace fh2platform::car_ids {

// A pack's car is given an Id from kFirstPackCar up, and never one from kFirstHandMade up: those are for
// cars that a mod's SQL (or a pack's, without pack.ini) adds with an Id of its own.
constexpr int64_t kFirstPackCar = 3000, kFirstHandMade = 9000;

struct Source {
    std::string name;                               // as the log names it: "car pack <folder>", or the mod's name
    const std::vector<std::string>* statements = nullptr;
    const std::vector<std::string>* cars = nullptr; // a car pack's cars (pack.ini), or none: its SQL runs as it is
    const std::vector<std::string>* wanted = nullptr; // and the Id each asks for (pack.ini's id=), "" for none
};

struct Outcome {
    std::vector<std::pair<std::string, int64_t>> ids;   // a pack's cars with their Ids, when it isn't left out
    std::vector<std::pair<std::string, int64_t>> given; // those of them given their Id just now, to record
    std::string problem;                                // why the pack is left out, or "" (a message from later())
    bool base_unread = false;                           // left out only because `base` couldn't be read: that can pass
    std::vector<std::string> warnings;                  // what in its SQL or pack.ini could clash with packs' cars (later())
    std::vector<std::string> notes;                     // for the log only
};

// One Outcome a source, in order. `base` is the database being built on (read only; null when it
// couldn't be opened), shown as `shown`; `record` is every Id given so far (lower-case media name -> Id).
//
// A pack car takes more than its Data_Car row: make_car_pack.py gives it rows keyed by its Id N in
// Data_Car, Data_Engine, Data_Drivetrain, Data_Car_Buckets, CameraOverrides and CarExceptions, and
// rows N * 1000 + n in Data_CarBody and the List_* tables (upgrades, torque curves, physics). Every
// key there that the base has, or that the other sources' SQL adds, is taken: that SQL is run on a
// copy of `base` in memory first (the mods', the packs' without cars, and a pack's statements that
// don't say {car:...}), and SQLite reports each row it adds. A recorded Id stays, and its pack is
// left out if another source has its keys now. A new car gets the Id its pack.ini asks for (id=, from
// kFirstPackCar to below kFirstHandMade) when that one is free, so installs with the same packs give
// it the same Id and a save shows the same cars on each; otherwise the lowest Id from kFirstPackCar up
// that's free and that no other new car asks for. Free: its keys aren't taken, nothing is recorded
// with it, and no other car has it this time.
//
// Warnings: a source that adds such rows for a car from kFirstPackCar to below kFirstHandMade, a pack
// whose SQL never says {car:X} for one of its cars, and a new car that can't have the Id it asks for
// (or asks for one that can't be given).
std::vector<Outcome> plan(sqlite3* base, const std::string& shown, const std::vector<Source>& sources,
                          const std::map<std::string, int64_t>& record);

// SQL text split into its statements (sqlite3_complete), each with its ';'. Text after the last
// complete one comes last, if it has more than spaces.
std::vector<std::string> split_statements(const std::string& sql);

std::string lower(std::string s);

} // namespace fh2platform::car_ids

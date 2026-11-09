// The Ids car packs' cars are given: what's taken in the database being built on and by the SQL that
// runs with the packs', and the lowest free Id for a new car (car_ids.hpp says how).
#include "car_ids.hpp"
#include "text.hpp"

#include "sqlite3.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace fh2platform::car_ids {

namespace {

// Tables keyed by a pack car's own Id, as make_car_pack.py writes them.
constexpr const char* kByCar[] = {"data_car", "data_engine", "data_drivetrain", "data_car_buckets", "cameraoverrides",
                                  "carexceptions"};

bool by_car(const std::string& lower_table) {
    return std::any_of(std::begin(kByCar), std::end(kByCar), [&](const char* t) { return lower_table == t; });
}
// Tables with the car's rows <Id> * 1000 + n (the game numbers its own the same way, by car, engine or drivetrain).
bool by_thousands(const std::string& lower_table) { return lower_table == "data_carbody" || lower_table.rfind("list_", 0) == 0; }

bool in_pack_range(int64_t id) { return id >= kFirstPackCar && id < kFirstHandMade; }

std::string quoted(const std::string& name) {
    std::string q = "\"";
    for (char c : name) q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

// A table's INTEGER PRIMARY KEY (its rowid), or "" when it has none.
std::string key_column(sqlite3* db, const std::string& table) {
    sqlite3_stmt* q = nullptr;
    std::string key;
    int keys = 0;
    if (sqlite3_prepare_v2(db, ("PRAGMA table_info(" + quoted(table) + ")").c_str(), -1, &q, nullptr) == SQLITE_OK)
        while (sqlite3_step(q) == SQLITE_ROW)
            if (sqlite3_column_int(q, 5) > 0) {
                ++keys;
                const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(q, 1));
                const auto* type = reinterpret_cast<const char*>(sqlite3_column_text(q, 2));
                if (name && type && lower(type) == "integer") key = name;
            }
    sqlite3_finalize(q);
    return keys == 1 ? key : std::string{};
}

bool exec(sqlite3* db, const std::string& sql) { return sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK; }

// Who has a key: the database built on (-1), or a source.
struct Owner {
    int source = -1;
    std::string what; // "Data_Engine 3014"
};

struct Row {
    std::string table;
    int64_t key;
};

// Rows SQLite adds or changes in the copy's own tables (not temp ones), by rowid.
void hooked(void* rows, int op, const char* db, const char* table, sqlite3_int64 rowid) {
    if (op != SQLITE_DELETE && db && table && std::strcmp(db, "main") == 0)
        static_cast<std::vector<Row>*>(rows)->push_back({table, rowid});
}

// The copy is only looked at: a mod's ATTACH could write to another file, and does when it runs for real.
int no_attach(void*, int action, const char*, const char*, const char*, const char*) {
    return action == SQLITE_ATTACH ? SQLITE_DENY : SQLITE_OK;
}

} // namespace

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split_statements(const std::string& sql) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t semi = sql.find(';'); semi != std::string::npos; semi = sql.find(';', semi + 1)) {
        std::string part = sql.substr(start, semi + 1 - start);
        if (sqlite3_complete(part.c_str())) {
            out.push_back(std::move(part));
            start = semi + 1;
        }
    }
    if (sql.find_first_not_of(" \t\r\n", start) != std::string::npos) out.push_back(sql.substr(start));
    return out;
}

std::vector<Outcome> plan(sqlite3* base, const std::string& shown, const std::vector<Source>& sources,
                          const std::map<std::string, int64_t>& record) {
    std::vector<Outcome> out(sources.size());
    const auto has_cars = [&](std::size_t i) { return sources[i].cars && !sources[i].cars->empty(); };

    // A pack whose SQL never says where one of its cars' Id goes.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!has_cars(i) || !sources[i].statements) continue;
        std::vector<std::string> text;
        for (const auto& s : *sources[i].statements) text.push_back(lower(s));
        for (const auto& car : *sources[i].cars) {
            const std::string tag = "{car:" + lower(car) + "}";
            if (std::none_of(text.begin(), text.end(), [&](const std::string& t) { return t.find(tag) != std::string::npos; }))
                out[i].warnings.push_back(later("Its SQL never says {car:{0}} where {0}'s Id goes, so the car doesn't get the Id "
                                                "the platform gives it.", {car}));
        }
    }

    // The copy, and what it has: car names, and every key a pack car could take.
    sqlite3* db = nullptr;
    bool copied = false;
    if (base && sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK) {
        if (sqlite3_backup* b = sqlite3_backup_init(db, "main", base, "main")) {
            copied = sqlite3_backup_step(b, -1) == SQLITE_DONE;
            copied = sqlite3_backup_finish(b) == SQLITE_OK && copied;
        }
    }
    std::set<std::string> names; // lower-case media names
    std::map<int64_t, Owner> by_id, by_block; // a car's own Id, and <Id> * 1000 + n: who has it
    std::map<std::string, std::string> keys;  // lower-case table -> its key column ("" for none)
    if (copied) {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT MediaName FROM Data_Car", -1, &q, nullptr) == SQLITE_OK)
            while (sqlite3_step(q) == SQLITE_ROW)
                if (const auto* t = sqlite3_column_text(q, 0)) names.insert(lower(reinterpret_cast<const char*>(t)));
        sqlite3_finalize(q);
        std::vector<std::string> tables;
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table'", -1, &q, nullptr) == SQLITE_OK)
            while (sqlite3_step(q) == SQLITE_ROW)
                if (const auto* t = sqlite3_column_text(q, 0)) tables.emplace_back(reinterpret_cast<const char*>(t));
        sqlite3_finalize(q);
        for (const auto& t : tables) {
            const std::string lt = lower(t);
            if (!by_car(lt) && !by_thousands(lt)) continue;
            const std::string key = keys[lt] = key_column(db, t);
            if (key.empty()) continue;
            if (sqlite3_prepare_v2(db, ("SELECT " + quoted(key) + " FROM " + quoted(t)).c_str(), -1, &q, nullptr) == SQLITE_OK)
                while (sqlite3_step(q) == SQLITE_ROW) {
                    const int64_t k = sqlite3_column_int64(q, 0);
                    if (by_car(lt)) by_id.emplace(k, Owner{-1, t + " " + std::to_string(k)});
                    else if (k >= 0) by_block.emplace(k / 1000, Owner{-1, t + " " + std::to_string(k)});
                }
            sqlite3_finalize(q);
        }
    }
    const bool readable = copied && !names.empty();

    // The other SQL on the copy, as it runs for real: each source in a transaction. A pack with cars
    // has its statements that don't say {car:...} run, one at a time (the others are its cars' own).
    if (readable) {
        std::vector<Row> rows;
        sqlite3_update_hook(db, hooked, &rows);
        sqlite3_set_authorizer(db, no_attach, nullptr);
        for (std::size_t i = 0; i < sources.size(); ++i) {
            if (!sources[i].statements) continue;
            std::vector<Row> kept;
            exec(db, "BEGIN");
            if (has_cars(i)) {
                for (const auto& text : *sources[i].statements)
                    for (const auto& s : split_statements(text)) {
                        if (lower(s).find("{car:") != std::string::npos) continue;
                        rows.clear();
                        if (exec(db, s)) kept.insert(kept.end(), rows.begin(), rows.end());
                    }
                if (!sqlite3_get_autocommit(db)) exec(db, "COMMIT");
            } else {
                rows.clear();
                bool ok = true;
                for (const auto& text : *sources[i].statements)
                    if (ok) ok = exec(db, text);
                if (ok && exec(db, "COMMIT")) kept = rows;
                else if (!sqlite3_get_autocommit(db)) exec(db, "ROLLBACK");
            }
            rows.clear();
            // Keys nobody had before are this source's. Those in the packs' range are warned about.
            std::vector<std::string> fixed;
            for (const auto& r : kept) {
                const std::string lt = lower(r.table);
                if (!by_car(lt) && !by_thousands(lt)) continue;
                if (!keys.count(lt)) keys[lt] = key_column(db, r.table); // a table its SQL made
                if (keys[lt].empty()) continue;
                const std::string what = r.table + " " + std::to_string(r.key);
                const bool mine = by_car(lt) ? by_id.emplace(r.key, Owner{static_cast<int>(i), what}).second
                                             : r.key >= 0 && by_block.emplace(r.key / 1000, Owner{static_cast<int>(i), what}).second;
                if (mine && in_pack_range(by_car(lt) ? r.key : r.key / 1000)) fixed.push_back(what);
            }
            if (!fixed.empty()) {
                const std::string list = fixed.size() == 1 ? fixed[0]
                                         : fixed.size() == 2 ? fixed[0] + ", " + fixed[1]
                                                             : later("{0}, {1} and {2} more", {fixed[0], fixed[1], std::to_string(fixed.size() - 2)});
                out[i].warnings.push_back(later("Its SQL adds rows by Ids that car packs' cars are given ({0}); a car added by hand "
                                                "should have an Id from {1} up.", {list, std::to_string(kFirstHandMade)}));
            }
        }
        sqlite3_update_hook(db, nullptr, nullptr);
    }
    if (db) sqlite3_close(db);

    // Who has car `id`'s keys, other than source `self`.
    const auto taken = [&](int64_t id, int self) -> const Owner* {
        if (const auto c = by_id.find(id); c != by_id.end() && c->second.source != self) return &c->second;
        if (const auto b = by_block.find(id); b != by_block.end() && b->second.source != self) return &b->second;
        return nullptr;
    };
    // Whose source has an Id's rows: the database built on (shown), or a mod's or pack's SQL.
    const auto whose = [&](const Owner& o) { return o.source < 0 ? shown : sources[o.source].name; };
    std::set<int64_t> recorded;
    for (const auto& [car, id] : record) recorded.insert(id);
    // The Id each car's pack.ini asks for (0 for none). No new car is given one that another new car asks for.
    std::vector<std::vector<int64_t>> asks(sources.size());
    std::set<int64_t> asked;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!has_cars(i)) continue;
        const auto& cars = *sources[i].cars;
        for (std::size_t c = 0; c < cars.size(); ++c) {
            const std::string text = sources[i].wanted && c < sources[i].wanted->size() ? (*sources[i].wanted)[c] : "";
            int64_t id = 0;
            if (!text.empty()) {
                const bool digits = text.size() <= 9 && std::all_of(text.begin(), text.end(), [](char d) { return d >= '0' && d <= '9'; });
                if (digits) id = std::stoll(text);
                if (!digits || !in_pack_range(id)) {
                    out[i].warnings.push_back(later("pack.ini's id for {0}, {1}, isn't an Id from {2} to {3}, so the car is given one "
                                                    "as if it asked for none.",
                                                    {cars[c], text, std::to_string(kFirstPackCar), std::to_string(kFirstHandMade - 1)}));
                    id = 0;
                }
            }
            asks[i].push_back(id);
            if (id && !record.count(lower(cars[c]))) asked.insert(id);
        }
    }
    std::map<std::string, std::string> claimed; // lower-case media name -> the pack adding it
    std::map<int64_t, std::string> given;       // Id -> the car that has it this time
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!has_cars(i)) continue;
        Outcome& o = out[i];
        const int self = static_cast<int>(i);
        std::string problem;
        if (!readable) problem = later("The cars of {0} couldn't be read.", {shown});
        std::vector<std::pair<std::string, int64_t>> ids, fresh;
        std::map<int64_t, std::string> mine; // Id -> which of this pack's cars has it
        std::vector<std::string> wishes;     // warnings about Ids asked for, kept unless the pack is left out
        // The warning that `car` asks for Id `want` but is given `id`, saying why it can't have `want` (its
        // keys taken by someone other than `owner`): a whole sentence for each reason.
        const auto not_wanted = [&](const std::string& car, int64_t want, int64_t id, int owner) -> std::string {
            const std::string w = std::to_string(want), n = std::to_string(id);
            const auto g = given.find(want);
            const auto m = mine.find(want);
            if (g != given.end() || m != mine.end())
                return later("{0} asks for Id {1} in pack.ini, but {2} has it, so it's car {3} here: a save from an install where it's "
                             "{1} doesn't show it here.", {car, w, g != given.end() ? g->second : m->second, n});
            if (recorded.count(want))
                return later("{0} asks for Id {1} in pack.ini, but fh2-mods\\platform-car-ids.txt gives it to another car, so it's "
                             "car {2} here: a save from an install where it's {1} doesn't show it here.", {car, w, n});
            const Owner* t = taken(want, owner);
            if (t && t->source < 0)
                return later("{0} asks for Id {1} in pack.ini, but {2} has {3}, so it's car {4} here: a save from an install where "
                             "it's {1} doesn't show it here.", {car, w, whose(*t), t->what, n});
            return later("{0} asks for Id {1} in pack.ini, but {2}'s SQL adds {3}, so it's car {4} here: a save from an install "
                         "where it's {1} doesn't show it here.", {car, w, t ? whose(*t) : std::string("?"), t ? t->what : "?", n});
        };
        const auto free_for = [&](int64_t id, int owner) {
            return !given.count(id) && !mine.count(id) && !recorded.count(id) && !taken(id, owner);
        };
        const auto& cars = *sources[i].cars;
        for (std::size_t c = 0; c < cars.size() && problem.empty(); ++c) {
            const std::string& car = cars[c];
            const std::string key = lower(car);
            const int64_t want   = asks[i][c];
            if (names.count(key)) {
                problem = later("The game already has {0}.", {car});
            } else if (const auto cl = claimed.find(key); cl != claimed.end()) {
                problem = later("{0} is {1}'s too.", {car, cl->second});
            } else if (const auto r = record.find(key); r != record.end()) {
                const auto g = given.find(r->second);
                const auto m = mine.find(r->second);
                if (g != given.end() || m != mine.end()) {
                    problem = later("{0}'s Id, {1}, is {2}'s too (fh2-mods\\platform-car-ids.txt gives both the same one).",
                                    {car, std::to_string(r->second), g != given.end() ? g->second : m->second});
                } else if (const Owner* t = taken(r->second, self)) {
                    problem = t->source < 0 ? later("{0}'s Id, {1}, is taken now: {2} has {3}.", {car, std::to_string(r->second), whose(*t), t->what})
                                            : later("{0}'s Id, {1}, is taken now: {2}'s SQL adds {3}.",
                                                    {car, std::to_string(r->second), whose(*t), t->what});
                } else {
                    ids.emplace_back(car, r->second), mine[r->second] = car;
                    if (want && want != r->second)
                        o.notes.push_back(car + " keeps car " + std::to_string(r->second) + ", which it was given before, rather than the " +
                                          std::to_string(want) + " its pack.ini asks for (a bought car's Id is in the save), so a "
                                          "save from an install where it's " + std::to_string(want) + " doesn't show it here");
                }
            } else {
                int64_t id = want && free_for(want, self) ? want : 0;
                if (!id) {
                    id = kFirstPackCar;
                    while (id < kFirstHandMade && (asked.count(id) || !free_for(id, -2))) ++id;
                }
                if (id >= kFirstHandMade) {
                    problem = later("No car Id from {0} to {1} is free for {2}.",
                                    {std::to_string(kFirstPackCar), std::to_string(kFirstHandMade - 1), car});
                } else {
                    if (want && id != want) wishes.push_back(not_wanted(car, want, id, self));
                    ids.emplace_back(car, id), fresh.emplace_back(car, id), mine[id] = car;
                }
            }
        }
        if (!problem.empty()) {
            o.problem     = problem;
            o.base_unread = !readable;
            o.notes.clear();
            continue;
        }
        o.warnings.insert(o.warnings.end(), wishes.begin(), wishes.end());
        for (const auto& [car, id] : ids) {
            claimed[lower(car)] = sources[i].name;
            given[id] = car;
            recorded.insert(id);
        }
        o.ids   = std::move(ids);
        o.given = std::move(fresh);
    }
    return out;
}

} // namespace fh2platform::car_ids

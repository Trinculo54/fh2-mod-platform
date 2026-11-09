// Shared between the platform's source files.
#pragma once

#include "fh2mod.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct FH2Mod {
    std::string name, version, repo, platform_version, description;
    std::wstring wname, dll;
    bool check_updates = true;
    bool offline_only  = false;      // FH2ModInfo.offline_only (API 9)
    // Stopped for online play (platform 2.9.0): it had started offline, then FH2MP went online. Until
    // the game restarts its hooks are off, its text, SQL and files are out and new ones refused, and
    // its hotkeys, commands, pad layers, HUD layer calls and title-screen entries do nothing.
    // Read and written through fh2platform::stopped_online and stop_online (atomic).
    bool stopped_online = false;
    // table (lower case) -> edits
    std::map<std::string, std::map<uint16_t, std::wstring>> set;
    std::map<std::string, std::map<std::wstring, std::wstring>> repl;
    // table (lower case) -> key -> (msg key, English): set_string_tr's, each language's own text (API 10)
    std::map<std::string, std::map<uint16_t, std::pair<std::string, std::wstring>>> set_tr;
    std::vector<std::string> notice; // from set_notice
    std::string update_notice;       // the platform's own title notice that a newer version is out ("" for none)
    std::vector<std::string> status; // the platform's other title notices: platform too old
    std::set<std::wstring> files;    // mediaoverride paths this mod owns (lower case)
    std::vector<std::string> sql;    // database statements, in the order given
    std::string latest;              // latest release on GitHub, once checked
};

namespace fh2platform {

// One line to platform.log; `who` is a mod name, or "" for the platform.
void log_line(const std::string& who, const std::string& text);
inline void plog(const std::string& text) { log_line("", text); }

// The loader thread, before every fh2mod_init has returned (a mod's fh2mod_init is running).
bool in_mod_init();
// An interface the platform itself offers, there before any mod starts, so a mod finds it from
// fh2mod_init on and can't publish the name. No log line.
void publish_platform_interface(const char* name, uint32_t version, const void* iface, FH2Mod* owner);

// The pad layer preview (pad_layer.cpp): FH2Platform.PadLayer v1, and its start once every fh2mod_init
// has returned.
const void* pad_interface();
FH2Mod* pad_layer_owner();
void pad_layer_start(const FH2ModApi* api);

// Base of the mapped game exe, or nullptr (always, once Azimuth has started another game).
void* find_game();
constexpr uint32_t kGameSizeOfImage = 0x03A18000u;
// Copies n bytes, false if any of them can't be read or written; dst may then hold part of the copy,
// so don't use it. For memory that can change under the platform: the game's image while Azimuth
// maps it, or a region freed after VirtualQuery.
bool safe_copy(void* dst, const void* src, std::size_t n);

// titles.cpp: which game Azimuth started. Azimuth 1.0.1 runs several Forza games, all from a
// forza_durango_release_final.exe mapped at 0x140000000 (platform 2.8.0).
enum class Launched { not_yet, fh2, other };
Launched launched_game();
const char* launched_game_name(); // the other game, for a log line or crash report ("" until then)
// A game other than FH2 that Azimuth's command line starts straight away (--skip-launcher, or
// --game=DIR), with how it was told, for the log; "" for FH2, or when the launcher shows first.
// When it's another game, launched_game() says so from then on.
std::string other_game_on_command_line();
std::filesystem::path command_line_game_folder(); // --game=DIR, else empty
bool holds_fh2_exe(const std::filesystem::path& folder); // in its Mount folder, or itself
// The other game's name when `p` is in a game exe mapped at 0x140000000 that isn't FH2's
// 24 Nov 2015 one, else "". Nothing is hooked or patched there.
std::string other_game_at(const void* p);
// Watches for Azimuth mapping the game, says in platform.log which one it is, and then calls
// `fh2` or `other` on its own thread.
void watch_launched_game(std::function<void()> fh2, std::function<void()> other);

std::filesystem::path azimuth_dir();
std::filesystem::path game_mount(); // empty if not found

// files.cpp: file overrides in Extra Options' mediaoverride folder.
int set_file(FH2Mod* mod, const char* media_path, const void* data, std::size_t size);
// set_file as mods call it (the API): the platform's own UI files (xaml_safety.hpp) are refused, with a log line.
int mod_set_file(FH2Mod* mod, const char* media_path, const void* data, std::size_t size);
void clear_file(FH2Mod* mod, const char* media_path);
// Removes overrides from earlier launches that no mod owns now.
void clean_leftover_files(const std::vector<FH2Mod*>& mods);
// Removes one override from an earlier launch now, if no mod replaces it this time.
void remove_leftover_file(const char* media_path);

// media_overrides.cpp: mounts mediaoverride and mediapc itself when Extra Options isn't loaded.
void start_media_overrides();

// car_packs.cpp: this Azimuth folder's own media layer, and car packs.
struct CarPack {
    std::string name;                         // its folder's name in fh2-cars
    std::filesystem::path root, media, mirror; // fh2-cars\<name>, its media folder, its copy on the temp drive
    std::vector<std::string> sql;             // its *.sql files' text, by file name (database.cpp runs them)
    std::string title, version;               // pack.ini's [pack] name and version
    std::vector<std::string> cars;            // the cars it adds: pack.ini's [car <media name>] sections
    std::vector<std::string> car_names;       // and each one's name= there ("" for none), for the MODS tab
    std::vector<std::string> wanted_ids;      // and its id= ("" for none): the Id it asks for (car_ids.cpp)
    // A car's engine sound of its own (pack.ini's [car] engine_sound, engine_sound_base, engine_synth): the pack's SQL
    // names the stock engine_sound_base (the game finds events only under stock names); the platform links that bank
    // under engine_sound, and an engine sound mod (SynthWrapper) told about it in fh2-mods\platform-engine-synths.txt
    // renames the car's engine to engine_sound when it can play it.
    struct EngineSound {
        std::string car, sound, base, synth; // media name; PG_ bank names without "PG_"; .synth file under the pack
    };
    std::vector<EngineSound> engine_sounds;
};
// <Azimuth>\data\titles\1A96545B\tmp, the folder Azimuth gives the game as its T:\ drive.
std::filesystem::path temp_drive();
// How the game spells a folder under temp_drive(): "T:\...\".
std::string temp_drive_spelling(const std::filesystem::path& folder);
std::filesystem::path instance_media(); // temp_drive()\fh2-platform\media
// fh2-mods\platform-engine-synths.txt: the car packs' own engine sounds (car_packs.cpp).
void write_engine_synths();
// The platform mounts its own folders (no Extra Options, Azimuth 1.0 or later): what it writes goes
// to instance_media(), which only this Azimuth folder's game sees.
bool instance_layer();
// Where overrides are written: instance_media(), else the game's mediaoverride.
std::filesystem::path override_root();
// The two places the platform writes files for the game (its roots): mediaoverride, and this Azimuth folder's own
// layer. From 2.9.0 a line of fh2-mods\platform-files.txt or platform-tables.txt ends in the roots its file is in
// ("mediaoverride", "layer", or both); a line without them is from 2.8.0 or older (which wrote mediaoverride) or a
// 2.9.0 test build (which may have written the layer), so it counts as both.
enum : unsigned { kRootOverride = 1, kRootLayer = 2, kRootBoth = 3 };
unsigned current_root();                                // this session's (instance_layer)
std::filesystem::path root_folder(unsigned root);       // one root's folder
std::string root_names(unsigned roots);                 // as a manifest line ends
std::string root_shown(unsigned root);                  // one root, for the log
unsigned parse_roots(const std::string& text);          // the reverse; kRootBoth when it names none
// At start, before anything is written: the platform's own files (platform-files.txt) and string tables
// (platform-tables.txt) in the root this session doesn't write to (what 2.8.0 left in mediaoverride, or a
// session with the other root), moved into fh2-mods\platform-old-overrides\<root>\, keeping their paths.
// Nothing the manifests don't list is touched. files.cpp; the tables' part is move_old_tables (platform.cpp).
void move_old_overrides();
void move_old_tables();
// Moves the platform's copy of `rel` (relative to the root's folder) to fh2-mods\platform-old-overrides\<root>\rel,
// and lists it in moved.txt there: 1 moved, 0 nothing there, -1 couldn't be moved (`error` says why).
int move_old_override(unsigned root, const std::filesystem::path& rel, std::string& error);
const std::vector<CarPack>& car_packs();
// Finds the packs, puts their files on the temp drive, and writes the merged zipmanifest.xml to
// instance_media(). Their SQL goes into the database with the mods' (apply_database).
void prepare_car_packs();
// That merged zipmanifest.xml is in the layer this session (a pack has zips): set_file refuses a mod's.
bool car_packs_manifest();
// Once a pack's cars have their Ids (database.cpp): the part Ids of their own that their carbins
// name (<car>.part-ids.txt), set in their zips on the temp drive.
void place_car_ids(const CarPack& pack, const std::vector<std::pair<std::string, int64_t>>& cars);
// Without the layer the packs in fh2-cars aren't read: says so (with `why`) when there are any.
void car_packs_unavailable(const std::string& why);   // why: a sentence, as a message from later() (text.hpp)
// Why the packs in fh2-cars aren't read this session (for the MODS tab), or "".
std::string car_packs_unread();
// Why a pack (by folder name) isn't working this session, or "": the first reason given.
void note_car_pack_problem(const std::string& pack, const std::string& why);   // why: a message from later() (text.hpp)
std::string car_pack_problem(const std::string& pack);

// database.cpp: SQL patches applied to a copy of the game database.
void apply_database(const std::vector<FH2Mod*>& mods);
// Data_Car.str keys the database's car names have that a pack or mod gave as text, with their text
// (the string tables are written with them).
std::map<uint16_t, std::wstring> database_strings();
// What in a car pack's (by folder name) or a mod's (by name) SQL could clash with car packs' cars, from
// the last build, or "": rows by an Id car packs' cars are given (3000 to 8999), a pack's car its
// SQL never gives as {car:...}, or one that can't have the Id its pack.ini asks for (so saves from
// other installs don't show it). Only looked for while a pack adds cars. For the MODS tab.
std::string car_pack_sql_warning(const std::string& pack);
std::string mod_sql_warning(const std::string& mod);
// Where the database the game reads comes from, for the database guards (db_guards.hpp): the files as they are
// now, told apart as apply_database tells them.
struct DbFacts;
DbFacts database_facts();
// The cars in that database whose powertrain isn't in its Powertrains table, as "MEDIANAME (Id)" (db_guards.hpp's
// query), or none when there are none or it can't be read.
std::vector<std::string> cars_without_powertrain();

// hooks.cpp: the hook API, and what crash reports say about hooks.
// The folder of Extra Options' winmm.dll if it's loaded, else empty. Extra Options is recognised
// by its Fh2TweaksStatus export, looked for in every loaded DLL, not by the name winmm.dll: it
// loads the system's own winmm.dll as well, and Wine and Proton load their own winmm.dll instead
// of the one beside azimuth.exe unless told otherwise (WINEDLLOVERRIDES="winmm=n,b"), and still
// report that file's path for it.
std::filesystem::path extra_options_dir();
// Under Wine or Proton, when the list of loaded DLLs can't be read, so whether Extra Options is
// loaded can't be told: whether its winmm.dll is next to azimuth.exe and Wine is set to load it
// rather than its own (WINEDLLOVERRIDES, or winecfg's DLL overrides). It then counts as loaded:
// mounting mediaoverride under it would make it refuse all of its patches. Worked out once.
// False whenever the list can be read, and always on Windows.
bool extra_options_expected();
// Whether FH2_ExtraOptions.log next to azimuth.exe was written this launch: Extra Options rewrites
// it every time it starts.
bool extra_options_log_is_new();
// How long after the game is mapped Extra Options can still be waited for: it patches the game
// right away.
inline constexpr unsigned long long kExtraOptionsTimeoutMs = 20000;
void fill_hook_api(FH2ModApi& api);
bool hook_at(const void* p, char* out, std::size_t n); // p is in a hook's trampoline or stub
// Whether the exe on disk holds `expect` at game address p, n <= 256 bytes (the HUD layer's code checks).
bool disk_bytes_equal(const void* p, const void* expect, std::size_t n);
bool disk_image_readable();   // the exe on disk could be opened for disk_bytes_equal
int hook_count();
const char* hook_line(int i, bool* enabled);

// settings.cpp: rows on each mod's page under Pause > MODS > YOUR MODS (API 4).
int settings_add(FH2Mod* mod, const FH2Setting* setting);
void settings_mark_hotkey(FH2Mod* mod, const char* key);   // hotkeys.cpp: that row is a hotkey's
int settings_get(FH2Mod* mod, const char* key);
void settings_set(FH2Mod* mod, const char* key, int value);
float settings_get_slider(FH2Mod* mod, const char* key);
void settings_set_slider(FH2Mod* mod, const char* key, float value);
// "fh2mod Platform ui set" (Developer=1): the platform's Mod pop-ups or Tips row, or a mod's Pop-ups row (`which`:
// ModPopups, Tips or the mod's name), changed as the player would. Returns the command's reply.
std::string settings_player_set(const std::string& which, bool on);
// A row's text, in the game's language (API 10).
int settings_set_text(FH2Mod* mod, const char* key, const char* label, const char* description, const char* const* choices,
                      uint32_t choice_count);
// Pages of the mods' own (API 8).
int pages_add(FH2Mod* mod, const FH2Page* page);
int pages_set_rows(FH2Mod* mod, const char* page_key, const FH2PageRow* rows, uint32_t count);
int pages_set_xaml(FH2Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
int pages_get_slot(FH2Mod* mod, const char* page_key, int slot);
int pages_set_slot(FH2Mod* mod, const char* page_key, int slot, int value);
int pages_focus(FH2Mod* mod, const char* page_key, const char* name);
int pages_set_tile_text(FH2Mod* mod, const char* page_key, const char* text);
int pages_set_tile_colors(FH2Mod* mod, const char* page_key, uint32_t background, uint32_t text);
// The UI templates on the mods' pages (API 10): their help bar and hint line, and the game's message box from their
// page, setting and title-screen callbacks.
int pages_set_help(FH2Mod* mod, const char* page_key, const FH2Prompt* prompts, uint32_t count);
int pages_set_hints(FH2Mod* mod, const char* page_key, const char* const* texts, uint32_t count);
int pages_show_hint(FH2Mod* mod, const char* page_key, int index, uint32_t kind);
int settings_show_box(FH2Mod* mod, const FH2MessageBox* box);
// Title-screen entries (API 9).
int title_add(FH2Mod* mod, const FH2TitleEntry* entry);
const char* title_started_by(FH2Mod* mod);
// After every fh2mod_init: writes the pages and hooks the game if any mod added a row.
void settings_start(const FH2ModApi* api);

// The MODS tab (platform 2.5.0): every mod in fh2-mods, loaded or turned off there.
struct ModEntry {
    std::wstring dll;                 // file name in fh2-mods
    std::string name, version, description, repo;
    bool enabled = true;              // loaded this time (false: turned off on the MODS tab)
    std::string problem;              // why it didn't load or start ("" if it did)
    bool platform_mod = false;        // has fh2mod_init (an old-style mod doesn't register)
    FH2Mod* mod = nullptr;            // when loaded and registered
    bool offline_only = false;        // its author says it shouldn't run online (FH2ModInfo.offline_only)
    bool loads_online = true;         // loaded when playing online (the player's choice, else !offline_only)
    bool online_off = false;          // not running this time because the game is online (FH2MP)
    bool fresh = false;               // new in fh2-mods since the last launch (platform-mods.txt didn't list it);
                                      // the seen store's mod.new keeps it (a second start in one launch sees false)
};
// In load order, turned-off ones included. The first call also puts what's new in the seen store (first, mod.new).
std::vector<ModEntry> mod_entries();
// Whether fh2-mods\platform-mods.txt was there when this launch started: false on a first launch.
bool mods_list_existed();
// The platform's own lines on the title screen, in English (each language's Main.str gets them in its own
// words), kept in fh2-mods\platform-title.txt; none takes them away. Written from settings_start, after the first
// write of the tables.
void set_platform_notice(const std::vector<std::string>& english_lines);
// fh2-mods\platform-disabled.txt: the DLLs the MODS tab turned off (lower case).
std::set<std::wstring> disabled_mods();
void set_mod_enabled(const std::wstring& dll, bool on);
// Online play (platform 2.8.0): FH2MP is on and has a session to play online with. Decided before
// the mods load; from 2.9.0 it can also turn true later (FH2MP's session found once it has started,
// or FH2MP saying it's going online), never back.
bool playing_online();
bool fh2mp_installed();               // FH2MP is in fh2-mods (turned on or not), as FH2MP.dll or under another name
// That entry is FH2MP itself (it always loads online): FH2MP.dll, or the DLL FH2MP registered from. A
// mod that only registers under the name FH2MP isn't.
bool is_fh2mp(const ModEntry& e);
// FH2Mod::stopped_online, from any thread.
inline bool stopped_online(const FH2Mod* m) {
    return m && std::atomic_ref<bool>(const_cast<FH2Mod*>(m)->stopped_online).load();
}
inline void stop_online(FH2Mod* m) { std::atomic_ref<bool>(m->stopped_online).store(true); }
// fh2-mods\platform-online.txt: the player's choice per DLL, "Load when playing online".
void set_mod_online(const std::wstring& dll, bool on);
// The latest release of a mod on GitHub, once checked ("" before that, or if unknown).
std::string latest_known(const FH2Mod* m);
// A mod's description now: set_mod_description's, or FH2ModInfo's in the game's language (its
// catalogue's "mod.description", API 10).
std::string mod_description(FH2Mod* m);

// session.cpp: this run of the game (API 9). start_session makes the id and opens the copy of
// platform.log that fh2-mods\platform-logs keeps; session_line is platform.log's line about it.
FILE* start_session();
std::string session_line();
const char* session_id();
const wchar_t* session_log_path(FH2Mod* mod, const wchar_t* folder, uint32_t keep);
// fh2-mods\platform-session.json: rewritten as the mods load and the command pipe starts, and
// marked crashed (with the report's name, relative to fh2-mods) by the crash handler.
void write_session_status();
void set_session_pipe(const std::string& name);
void write_session_crashed(const wchar_t* fh2mods_dir, const char* report);
// Copies of the saves folder that mods ask for from fh2mod_init (API 9), made once they've all
// returned (start_save_snapshot).
int request_save_snapshot(FH2Mod* mod, const char* reason, uint32_t keep);
void start_save_snapshot();

// crash.cpp
struct LoadedDll {
    std::wstring name;
    const char* base;
    std::size_t size;
};
void install_crash_handler(const std::vector<LoadedDll>& mods);
void add_crash_dll(const LoadedDll& dll); // one loaded later (a mod from before the platform)
void recheck_crash_handler();
void add_dump_memory(FH2Mod* mod, const void* p, std::size_t n); // memory a mod wants in every dump (API 9)

// hotkeys.cpp: keys and controller chords that call mods back, with rows on the Mods page (API 9).
int hotkey_add(FH2Mod* mod, const FH2Hotkey* hotkey);
// Their rows' text, in the game's language now (before the pages are written).
void hotkeys_refresh_text();
// A mod's hotkey by its key: its binding now (the player may have changed it on the Mods page). False if the mod has
// no such hotkey. For tips that name one (FH2Prompt.hotkey, API 10).
bool hotkey_binding(const FH2Mod* mod, const std::string& key, uint32_t* vk, uint32_t* modifiers, uint32_t* chord);

// commands.cpp: mods' commands, run through a named pipe (API 9).
int command_add(FH2Mod* mod, const char* name, const char* help, FH2CommandFn fn, void* user);
// The platform's own command (ui_player.cpp's "Platform ui"): listed and run like a mod's, but it starts the pipe
// only with `start_pipe`; otherwise it can be run whenever a mod's command has started it.
int command_add_platform(FH2Mod* mod, const char* name, const char* help, FH2CommandFn fn, void* user, bool start_pipe);

} // namespace fh2platform

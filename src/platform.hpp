// Shared between the platform's source files.
#pragma once

#include "fh2mod.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

struct FH2Mod {
    std::string name, version, repo, platform_version, description;
    std::wstring wname, dll;
    bool check_updates = true;
    // table (lower case) -> edits
    std::map<std::string, std::map<uint16_t, std::wstring>> set;
    std::map<std::string, std::map<std::wstring, std::wstring>> repl;
    std::vector<std::string> notice; // from set_notice
    std::vector<std::string> status; // the platform's own: update available, platform too old
    std::set<std::wstring> files;    // mediaoverride paths this mod owns (lower case)
    std::vector<std::string> sql;    // database statements, in the order given
    std::string latest;              // latest release on GitHub, once checked
};

namespace fh2platform {

// One line to platform.log; `who` is a mod name, or "" for the platform.
void log_line(const std::string& who, const std::string& text);
inline void plog(const std::string& text) { log_line("", text); }

// Base of the mapped game exe, or nullptr.
void* find_game();
constexpr uint32_t kGameSizeOfImage = 0x03A18000u;

std::filesystem::path azimuth_dir();
std::filesystem::path game_mount(); // empty if not found

// files.cpp: file overrides in Extra Options' mediaoverride folder.
int set_file(FH2Mod* mod, const char* media_path, const void* data, std::size_t size);
void clear_file(FH2Mod* mod, const char* media_path);
// Removes overrides from earlier launches that no mod owns now.
void clean_leftover_files(const std::vector<FH2Mod*>& mods);
// Removes one override from an earlier launch now, if no mod replaces it this time.
void remove_leftover_file(const char* media_path);

// database.cpp: SQL patches applied to a copy of the game database.
void apply_database(const std::vector<FH2Mod*>& mods);

// hooks.cpp: the hook API, and what crash reports say about hooks.
void fill_hook_api(FH2ModApi& api);
bool hook_at(const void* p, char* out, std::size_t n); // p is in a hook's trampoline or stub
int hook_count();
const char* hook_line(int i, bool* enabled);

// settings.cpp: rows on the game's Mods page (API 4).
int settings_add(FH2Mod* mod, const FH2Setting* setting);
int settings_get(FH2Mod* mod, const char* key);
void settings_set(FH2Mod* mod, const char* key, int value);
float settings_get_slider(FH2Mod* mod, const char* key);
void settings_set_slider(FH2Mod* mod, const char* key, float value);
// Pages of the mods' own (API 8).
int pages_add(FH2Mod* mod, const FH2Page* page);
int pages_set_rows(FH2Mod* mod, const char* page_key, const FH2PageRow* rows, uint32_t count);
int pages_set_xaml(FH2Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
int pages_get_slot(FH2Mod* mod, const char* page_key, int slot);
int pages_set_slot(FH2Mod* mod, const char* page_key, int slot, int value);
int pages_focus(FH2Mod* mod, const char* page_key, const char* name);
int pages_set_tile_text(FH2Mod* mod, const char* page_key, const char* text);
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
};
std::vector<ModEntry> mod_entries();  // in load order, turned-off ones included
// fh2-mods\platform-disabled.txt: the DLLs the MODS tab turned off (lower case).
std::set<std::wstring> disabled_mods();
void set_mod_enabled(const std::wstring& dll, bool on);
// The latest release of a mod on GitHub, once checked ("" before that, or if unknown).
std::string latest_known(const FH2Mod* m);

// crash.cpp
struct LoadedDll {
    std::wstring name;
    const char* base;
    std::size_t size;
};
void install_crash_handler(const std::vector<LoadedDll>& mods);
void recheck_crash_handler();

} // namespace fh2platform

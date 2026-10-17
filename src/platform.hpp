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
    std::string name, version, repo, platform_version;
    std::wstring wname, dll;
    bool check_updates = true;
    // table (lower case) -> edits
    std::map<std::string, std::map<uint16_t, std::wstring>> set;
    std::map<std::string, std::map<std::wstring, std::wstring>> repl;
    std::vector<std::string> notice; // from set_notice
    std::vector<std::string> status; // the platform's own: update available, platform too old
    std::set<std::wstring> files;    // mediaoverride paths this mod owns (lower case)
    std::vector<std::string> sql;    // database statements, in the order given
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

// database.cpp: SQL patches applied to a copy of the game database.
void apply_database(const std::vector<FH2Mod*>& mods);

// hooks.cpp: the hook API, and what crash reports say about hooks.
void fill_hook_api(FH2ModApi& api);
bool hook_at(const void* p, char* out, std::size_t n); // p is in a hook's trampoline or stub
int hook_count();
const char* hook_line(int i, bool* enabled);

// crash.cpp
struct LoadedDll {
    std::wstring name;
    const char* base;
    std::size_t size;
};
void install_crash_handler(const std::vector<LoadedDll>& mods);
void recheck_crash_handler();

} // namespace fh2platform

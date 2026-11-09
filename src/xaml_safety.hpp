// File and XAML safety (platform 2.9.0): the platform's own UI files, which set_file keeps for the platform; the
// check a page of a mod's own design (set_page_xaml, API 8) passes before it's written; and the state of the page
// crash guard. Pure: no game calls and no platform globals, so the tests build it without the game. The game's
// files are read through callbacks (page_guard.cpp gives them UI.zip and the texture zips).
#pragma once

#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fh2platform {

// ---- the platform's own UI files -------------------------------------------------------------------------------

// What a mod can't replace with set_file, because the platform writes it (with the reason, for the log). The names
// are under media; the game reads UI.zip's files as UI\<path in the zip> and stateflow.zip's as stateflow\<name>.
struct PlatformUiFile {
    const char* path;   // as the docs list it: "UI\Scenes\Anthem\A_PauseMenu.xaml"; NN stands for two digits
    const char* what;   // what the platform uses it for: "the pause menu's MODS tab"
};
extern const PlatformUiFile kPlatformUiFiles[];
extern const std::size_t kPlatformUiFileCount;

// The entry of kPlatformUiFiles that `media_path` (a path as set_file takes it) names, or nullptr. Every spelling
// the game would read as the same file counts: / or \, any case, "media\" in front or not, the zip's name with
// ".zip" ("UI.zip\Scenes\...") or the path inside the zip alone ("Scenes\Anthem\A_Hud.xaml"), and the dots and
// spaces Windows drops from the end of a name.
const PlatformUiFile* platform_ui_file(std::string_view media_path);

// ---- the page check --------------------------------------------------------------------------------------------

// The keys every page can use: those of AnthemApp.xaml's own dictionary and of the dictionaries it merges.
struct AppKeys {
    std::set<std::string> keys;
    int dictionaries = 0;   // merged dictionaries read
};
// Reads them through `read` (a file of UI.zip by its path, "" if it can't be read). False, with why, if AnthemApp.xaml
// or one of its dictionaries can't be read or its tags don't balance.
bool load_app_keys(const std::function<std::string(const std::string&)>& read, AppKeys& out, std::string& why);

// The keys the platform's page frame adds to every page of a mod's own design (settings.cpp, write_custom_page).
extern const char* const kPageFrameKeys[];
extern const std::size_t kPageFrameKeyCount;

// Where the game looks for an image a XAML path names: a zip under media\ui\textures ("Anthem.zip",
// "data_bound\ControllerIcons.zip") and the entry in it ("Icons/Trophy_Icon_Medium.swatchbin"). XAML names a
// texture as .png, and the file is .swatchbin: "/ForzaUI;component/Textures/Anthem/Icons/Trophy_Icon_Medium.png"
// (the "/ForzaUI;component" may be left out, and \ is accepted for /).
struct TextureSpot {
    std::string zip, entry;
};
// The places, in the order the game tries them (Data_Bound has its own zips and one of its own); empty if `path`
// isn't one of the game's texture paths.
std::vector<TextureSpot> texture_spots(std::string_view path);
// Whether a zip holds an entry: 1 it does, 0 it doesn't, -1 the zip can't be read. Entries compare in any case,
// with / and \ the same.
using TextureLookup = std::function<int(const TextureSpot&)>;

struct FragmentCheck {
    std::size_t keys_used    = 0;   // distinct resource keys the fragment uses
    std::size_t own_keys     = 0;   // keys it defines itself
    std::size_t images       = 0;   // distinct image paths
    std::size_t images_unchecked = 0;   // of those, ones whose zip couldn't be read (let through)
};

// Checks a page of a mod's own design, its three parts as set_page_xaml takes them (any may be empty): every
// {StaticResource K} and {DynamicResource K} (and <StaticResource ResourceKey="K"/>) names a key of the
// application's, of the frame's or of the fragment's own (any x:Key in any part), and every image path is an entry
// of the game's texture zips. False, with `why` naming the first key or path that isn't, and which part it's in.
// The tags are checked before this (settings.cpp's balanced_xml).
bool check_page_fragment(const AppKeys& app, std::string_view resources, std::string_view content, std::string_view help,
                         const TextureLookup& textures, FragmentCheck& out, std::string& why);

// ---- the page crash guard's state ------------------------------------------------------------------------------

// fh2-mods\platform-pages.txt: a page of a mod's own design that was opening when the game stopped, or one that's
// left out because of that. One a line, tab-separated: state, the mod's DLL file name, the page's key, the DLL's
// stamp (its size and time, so an update or reinstall shows) and the date.
struct GuardEntry {
    std::string state;   // "opening" or "skip"
    std::string dll, page, stamp, date;
};
std::vector<GuardEntry> parse_guard(std::string_view text);
std::string guard_text(const std::vector<GuardEntry>& entries);

// Last session's entries as this session starts: a page still "opening" (the game stopped while loading it: a crash,
// most likely) is skipped from now on, and a skip lasts until its DLL changes. `stamp_of` gives a DLL's stamp now
// ("" if it's gone). `lines` gets a log line for each entry.
std::vector<GuardEntry> guard_at_start(const std::vector<GuardEntry>& last, const std::function<std::string(const std::string&)>& stamp_of,
                                       std::vector<std::string>& lines);

} // namespace fh2platform

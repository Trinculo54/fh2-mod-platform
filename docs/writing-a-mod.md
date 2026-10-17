# Writing a mod

This walks through making an FH2 mod on the platform, from an empty folder to a zip players can install. The example is [`examples/hello_mod`](../examples/hello_mod/hello_mod.cpp); copying it is a fine way to start.

## What you need

A mod is a 64-bit Windows DLL, written in anything that can export a C function. C++ is what the existing mods use, and the instructions here assume it.

To build, you need either [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) with CMake, which works on Linux and Windows and is what every FH2 mod uses so far, or Visual Studio with CMake. If you plan to use `__try`/`__except` around reads of game memory, llvm-mingw needs `-fms-extensions`, and GCC-based MinGW won't do at all.

To test, you need Forza Horizon 2 running through Azimuth. [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions) is needed for anything that changes the game's text, since the platform writes those changes to its `mediaoverride` folder. Most players have it already.

## Set up the project

Add the platform to your repo as a submodule, so your build always has a platform to ship and the header to compile against:

```bash
git submodule add https://github.com/Trinculo54/FH2-Mod-Platform platform
```

Then in your `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(mymod VERSION 1.0.0 LANGUAGES C CXX RC)

add_subdirectory(platform)   # gives you fh2mod_platform and fh2mod_api

add_library(mymod SHARED src/mymod.cpp)
target_link_libraries(mymod PRIVATE fh2mod_api)
set_target_properties(mymod PROPERTIES OUTPUT_NAME MyMod PREFIX "")
target_compile_definitions(mymod PRIVATE MYMOD_VERSION="${PROJECT_VERSION}")
if(NOT MSVC)
    target_link_options(mymod PRIVATE -static)   # no libc++/libunwind DLLs to ship
endif()
```

`fh2mod_api` adds the include path for `fh2mod.h` and defines `FH2MOD_PLATFORM_VERSION` as the platform version in the submodule. Building gives you `MyMod.dll` and the platform's `d3dcompiler_47.dll`.

## The entry point

Export `fh2mod_init`. The platform calls it once, right after loading your DLL, on its own thread and never under the Windows loader lock. That means you can load DLLs, create files and start threads from it, which you can't safely do from `DllMain`.

```cpp
#include "fh2mod.h"

static const FH2ModApi* g_api;
static FH2Mod* g_mod;

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "MyMod";
    info.version          = MYMOD_VERSION;
    info.github_repo      = "You/MyMod";            // or nullptr for no update checks
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates    = 1;                      // let players turn this off in your settings
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    // ... set things up ...
    return 0;
}
```

Keep `fh2mod_init` quick. Mods are initialised one after another, and the game's text is written out once all of them have returned, before the game reads it. Anything slow, like network requests, waiting for the game, or reading big files, goes on a thread you start yourself.

A mod doesn't strictly need `DllMain` at all. If you have one, keep it to `DisableThreadLibraryCalls` and the like.

## Changing the game's text

Every piece of text in the game lives in a string table, and every string has a 16-bit key. [`string-tables.md`](string-tables.md) explains how to find the key you want. `scripts/dump_strings.py` lists a table's keys, their `IDS_...` names and their English text.

There are two ways to change a string. `set_string(mod, "Main.str", key, L"text")` replaces one string by its key, in every language. `replace_string(mod, "AIPlayers.str", L"old", L"new")` replaces every string that is exactly `old`. That's useful for names, which are the same in every language but don't have keys you'd want to hard-code.

Edits made inside `fh2mod_init` are written for you once every mod has started. The game reads its string tables once, early on, so later edits only show from the next launch. Call `apply_strings(mod)` after making them so they're saved for then. `clear_strings` takes yours back out, for example when a player turns a feature off.

If two mods set the same key, the one loaded later wins, in alphabetical order of DLL name, and `platform.log` notes it. Replacements by text apply after key edits.

## Title-screen notices

`set_notice(mod, "line one\nline two")` puts lines under "PRESS (A) TO START" on the title screen, together with every other mod's. Players see it each time the game starts, so use it for things they need to know, not for status chatter. There's room for about 40 characters per line, and empty lines are dropped. `set_notice(mod, nullptr)` clears yours.

You don't need it for updates: register with `github_repo` and the platform checks your latest GitHub release (at most every six hours, remembered between launches) and adds "MyMod 1.1.0 is available" itself when it's newer than your `version`. It compares `x.y.z` numbers, with or without a leading `v`, so tag releases to match your version. It also adds "Old FH2 Mod Platform: reinstall MyMod" when the installed platform is older than your `platform_version`.

## Working with the game

Azimuth maps the game, `forza_durango_release_final.exe`, into its own process when you press Play, so it isn't there yet when your mod starts. `wait_for_game(timeout)` blocks until it is and returns its base address; `game_base()` returns it right away, or null. Call `wait_for_game` from your own thread, not from `fh2mod_init`.

Only one build of the game is in circulation: the Xbox One build from 24 November 2015. Addresses in it are fixed, so mods can use offsets from the base. Even so, check that the code or data you're about to change looks the way you expect before you patch it, and log and skip if it doesn't. A wrong guess is then a missing feature, not a crash. [`game-notes.md`](game-notes.md) lists what's known about the build, and both existing mods are worth reading. FH2 Universal Radio redirects calls inside the game's FMOD radio, and WheresMyBuddy swaps a virtual function on a save-game class.

Game code runs on many threads. If you hook something, your hook can be called from any of them, at any time, including while you're still setting up. Install hooks atomically (one aligned 8-byte write for a pointer, or a compare-exchange), and finish everything your hook needs before you install it.

## Your own files

Keep your settings, logs and other data in a folder of your own next to `azimuth.exe`, named after your mod, such as `MyMod\` or `mymod\`. The existing mods use `fh2-radio\` and `wheresmybuddy\`. `api->azimuth_dir` gives you that folder and `api->game_dir` the game's `Mount` folder. Don't put data in `fh2-mods`: anything in there ending in `.dll` gets loaded as a mod, and the platform keeps its own files there.

## Shipping it

A release zip, extracted into the Azimuth folder, should end up like this:

```
d3dcompiler_47.dll        the platform, from your build
fh2-mods\MyMod.dll
MyMod\...                 your data, if any
```

Always include the platform. A player who only installs your mod needs it, and the version checks take care of an older copy getting put back. A short `README.txt` that says where things go and where your log is will save you a lot of questions.

Before releasing, start the game with only your mod and check `fh2-mods\platform.log`. It should show your DLL loaded with its version, your `registered` line, and the tables you edited. Then try it next to the other mods.

Put a version in your DLL's file properties (a `VERSIONINFO` resource) as well. The platform logs it next to the file name, and players can see it under Properties → Details. Both existing mods generate theirs from the CMake project version; see `src/version.rc` and `cmake/version.h.in` in either repo.

## Etiquette

A few things keep mods from tripping over each other:

- **Don't proxy DLLs yourself.** `d3dcompiler_47.dll` is the platform's and `winmm.dll` is Extra Options'. If you need to load early, the platform is already as early as it gets.
- **Don't write into the game's own files.** Use the string-table functions, or Extra Options' `mediaoverride` folder for other assets, so that removing your mod puts everything back.
- **Make your features switchable.** Give each one a setting, and when it's off, undo what you did, for example with `clear_strings`.
- **Don't take ages to start.** Do your setup on your own threads.
- **Log what you change.** It's how people debug, you included.

<h1 align="center" id="title">FH2 Mod Platform</h1>

The shared base for **Forza Horizon 2** mods on PC. It loads every mod in one folder, gives them a small set of services so they don't each have to reinvent them, and keeps them from stepping on each other's files. If you play with FH2 mods, you already have it: every mod built on it ships a copy. If you make mods, this is where to start.

It's the `d3dcompiler_47.dll` that sits next to `azimuth.exe`. Azimuth loads that DLL when it starts, Windows looks for it in Azimuth's own folder first, and every function in it forwards straight to the real one in System32, so shaders compile exactly as before. The only extra thing it does is load the DLLs in the `fh2-mods` folder and look after them. It was the FH2 Mod Loader up to version 1.1, and became the platform with 2.0.

Mods currently built on it are [FH2 Universal Radio](https://github.com/Trinculo54/FH2-Universal-Radio), which plays Spotify and more on the in-game radio, [WheresMyBuddy](https://github.com/Trinculo54/WheresMyBuddy), which puts your Xbox friends in the game as Drivatars, and [WreckingBall](https://github.com/Trinculo54/WreckingBall), which makes trees, fences, railings, roadside clutter and telephone poles break when you drive into them. It works alongside [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions), which loads through `winmm.dll`.

## For players

There's nothing to install by hand. Each mod's zip includes `d3dcompiler_47.dll`, and extracting any of them into your Azimuth folder puts the platform in place. Each mod ships the platform it was built with, so keep the newest one in place: if you install a mod that ships an older platform, reinstall a mod that has the newest afterwards. The first line of `fh2-mods\platform.log` says which version is running. Mods that need a newer platform say so on the title screen, under "PRESS (A) TO START", and reinstalling one of them fixes it. Other mods can't tell, and an older platform can do worse: up to 2.7.1 it replaced a game database another mod had put in `mediaoverride` (a traffic car mod's, say), where 2.7.2 keeps it. From 2.7.3 the platform also makes the game read `mediaoverride` when Extra Options isn't loaded, for example on Azimuth 1.0.1, which Extra Options doesn't support yet ([game-notes.md](docs/game-notes.md#the-override-folder)). If an old mod from before the platform puts back the original FH2 Mod Loader, newer mods keep their main features but lose the extras, and they say so in their logs.

Mods can also put their main switches in the game itself. When one does, Pause > Settings gets a MODS tile next to the others, and the page behind it lists every mod's switches, with a description of each. A change takes effect straight away or, for switches that only work after a restart, the game offers to restart for you when you leave the page.

The same spot on the title screen tells you when one of your mods has a new release on GitHub. Each mod decides whether it checks, and most let you turn it off in their settings.

Everything the platform does is logged to `fh2-mods\platform.log`: which mods it loaded and their versions, which game text, files and data it changed and for whom, and any mod that failed to load or crashed while starting up. It's the first thing to look at when a mod doesn't seem to be doing anything. If the game itself crashes, the platform writes a report to `fh2-mods\crashes` naming the mod the crash came from, if any, which is the thing to attach when you report it to that mod's author.

**Racing wheels ([fh2wheel](https://github.com/iamAzoDx/fh2wheel)).** fh2wheel ships its own `D3DCOMPILER_47.dll` too, a small loader that only loads `fh2wheel.dll`, and only one of the two files can sit next to `azimuth.exe`. From platform 2.6.1 the platform loads `fh2wheel.dll` itself when it finds it there, so to use both: install fh2wheel as its instructions say, then install (or reinstall) a platform mod, so that the platform's `d3dcompiler_47.dll` is the one in place. Keep `fh2wheel.dll` and `fh2wheel-config.exe` next to `azimuth.exe`. fh2wheel then shows up on the MODS tab, `platform.log` says it loaded it, and its settings stay in `fh2wheel-config.exe`. If fh2wheel's `D3DCOMPILER_47.dll` ends up in place instead, the wheel works but none of the platform's mods load.

**On Linux (Wine or Proton).** The platform works as it is, but Extra Options needs a DLL override (`WINEDLLOVERRIDES="winmm=n,b"`), database changes need platform 2.7.1 or newer, and a few mods have limits there. Without the override, platform 2.7.3 or newer still gets mods' text, file and database changes to the game. All of it is in [docs/linux.md](docs/linux.md).

To remove the platform, delete `d3dcompiler_47.dll` and the `fh2-mods` folder. That removes every mod built on it.

## For mod makers

A mod is a 64-bit DLL that goes in `fh2-mods` and exports one function:

```cpp
#include "fh2mod.h"

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{sizeof info, "MyMod", "1.0.0", "You/MyMod", FH2MOD_PLATFORM_VERSION, 1};
    FH2Mod* mod = api->register_mod(&info);
    api->log(mod, "hello");
    api->set_string(mod, "LoadingMessages.str", 0x6875, L"Fast Traveling (with MyMod)");
    return 0;
}
```

The platform calls `fh2mod_init` on its own thread right after loading your DLL, and the API it passes in gives you:

- **Game text.** Change any string in the game's string tables, by key or by matching text, in every language. The platform merges the edits from every mod and writes them to the game's override folder (`mediaoverride`), so two mods editing the same table don't overwrite each other.
- **Title-screen notices.** Lines of your own under "PRESS (A) TO START", stacked with every other mod's.
- **Update checks.** Name your GitHub repo and the title screen tells players when you publish a newer release.
- **Version checks.** Say which platform version you ship with, and players get told when an older one is installed.
- **Logging.** Lines go to `fh2-mods\platform.log`, tagged with your mod's name.
- **Finding the game.** Azimuth maps the game into its own process when you press Play, and you get its base address.
- **Game files.** Replace any file the game loads. One mod per file, and it's cleaned up when the mod is removed.
- **The game database.** Change cars, events and the rest of the game's data with SQL. Every mod's changes go into one copy of the database.
- **Other mods.** Publish functions for other mods to call, look up theirs, and check which mods are installed.
- **In-game settings.** On/Off switches and sliders on a Mods page in the game's own Settings menu, shared by every mod. You're told when the player flips one, and switches that need a restart get the game's own restart prompt.
- **Pages of your own.** From platform 2.7.0, lists of your own in the game's menus, laid out like its options pages, with a row to open them under MODS. A page can also take over the pause menu's Achievements tile, which does nothing on PC. There it can be XAML of your own design, with bindings, A, B, X, Y and the bumpers of its own, and the tile itself can show a line of text and colours of your choosing.
- **Hooks.** Detour game functions, single calls or vtable slots through one shared copy of MinHook, and from platform 2.6.0 hook any single instruction (reading and changing the registers there) or patch bytes in place. Mods can hook the same place side by side, the platform refuses patches that would clash with another mod's or with Extra Options', and every hook is logged and listed in crash reports.
- **Crash reports.** When the game crashes, a report in `fh2-mods\crashes` says where, which mod it came from, and which hooks were in place.

The whole API is one C header, [`include/fh2mod.h`](include/fh2mod.h), and it only ever grows: a mod built against an older version keeps working with newer platforms.

[`docs/writing-a-mod.md`](docs/writing-a-mod.md) walks through making, building and shipping a mod from scratch. [`docs/api.md`](docs/api.md) covers every function. [`docs/string-tables.md`](docs/string-tables.md) explains the game's text files and how to find the key for a string, and [`scripts/dump_strings.py`](scripts/dump_strings.py) lists them for you. [`docs/game-notes.md`](docs/game-notes.md) collects what's known about the game build everyone plays on, from how Azimuth maps it to where the Drivatars, the radio and the game database live. [`examples/hello_mod`](examples/hello_mod/hello_mod.cpp) is a complete mod in about fifty lines.

Mods from before the platform, which start themselves from `DllMain`, still load. They just don't get the services.

## Build from source

The platform is built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and CMake. On Linux:

```bash
./scripts/build.sh
```

`dist/` then holds `d3dcompiler_47.dll`, the example mod, the API header, a `README.txt` and the license. GitHub Actions does the same on every push, and a tag like `v2.0.0` attaches the zip to a release. The version lives in `project(... VERSION x.y.z)` in `CMakeLists.txt`; bump it before tagging, since the tag build refuses to run if the two don't match.

A mod usually adds this repo as a git submodule and builds the platform alongside itself, so its release zip always carries a matching `d3dcompiler_47.dll`. [`docs/writing-a-mod.md`](docs/writing-a-mod.md) shows the CMake for that.

Configuring with `-DFH2MOD_TESTS=ON` builds an offline test, which runs under Wine as a stand-in for `azimuth.exe` with the example mod, two test mods and an old-style mod in `fh2-mods`. It needs `EN.zip` and `gamedbRC.slt` from your own install: `tests/run_platform_test.sh <EN.zip> <gamedbRC.slt> [forza_durango_release_final.exe]`. The same script runs the database test, a few dozen launches with other mods' databases in `mediaoverride` and `mediapc`, FH2MP's online copy, and files that can't be written. The hook test needs only the game exe, and runs five launches (no Extra Options, Extra Options finishing late, Wine's own winmm.dll, game code that isn't as expected, and a crash inside a hook); it covers instruction hooks and byte patches too, and `PreciseTest.dll` checks instruction hooks inside the running game: `tests/run_hook_test.sh <forza_durango_release_final.exe>`.

## License

Released under the [MIT License](LICENSE), so you can build anything on it, open source or not. The platform includes [MinHook](https://github.com/TsudaKageyu/minhook) (in `third_party/minhook`), which is under the BSD 2-Clause license; `dist/LICENSE-MinHook.txt` is its notice, to ship alongside `d3dcompiler_47.dll`. It also includes [SQLite](https://sqlite.org) (in `third_party/sqlite`), which is in the public domain.

## Disclaimer

Unofficial fan-made software. Not affiliated with, endorsed by, or connected to Playground Games, Turn 10 Studios, Xbox Game Studios, Microsoft, the Azimuth developers, or the Extra Options author. Use at your own risk.

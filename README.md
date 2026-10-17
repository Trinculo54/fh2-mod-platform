<h1 align="center" id="title">FH2 Mod Platform</h1>

The shared base for **Forza Horizon 2** mods on PC. It loads every mod in one folder, gives them a small set of services so they don't each have to reinvent them, and keeps them from stepping on each other's files. If you play with FH2 mods, you already have it: every mod built on it ships a copy. If you make mods, this is where to start.

It's the `d3dcompiler_47.dll` that sits next to `azimuth.exe`. Azimuth loads that DLL when it starts, Windows looks for it in Azimuth's own folder first, and every function in it forwards straight to the real one in System32, so shaders compile exactly as before. The only extra thing it does is load the DLLs in the `fh2-mods` folder and look after them. It was the FH2 Mod Loader up to version 1.1, and became the platform with 2.0.

Mods currently built on it are [FH2 Universal Radio](https://github.com/Trinculo54/FH2-Universal-Radio), which plays Spotify and more on the in-game radio, and [WheresMyBuddy](https://github.com/Trinculo54/WheresMyBuddy), which puts your Xbox friends in the game as Drivatars. It works alongside [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions), which loads through `winmm.dll`.

## For players

There's nothing to install by hand. Each mod's zip includes `d3dcompiler_47.dll`, and extracting any of them into your Azimuth folder puts the platform in place. Every copy is the same file, so it doesn't matter which mod you install last, as long as it's a mod from the platform era. If you install a mod that ships an older platform, mods that need the newer one say so on the title screen, under "PRESS (A) TO START", and reinstalling one of them fixes it. If an old mod from before the platform puts back the original FH2 Mod Loader, newer mods keep their main features but lose the extras, and they say so in their logs.

The same spot on the title screen tells you when one of your mods has a new release on GitHub. Each mod decides whether it checks, and most let you turn it off in their settings.

Everything the platform does is logged to `fh2-mods\platform.log`: which mods it loaded and their versions, which game text it changed and for whom, and any mod that failed to load or crashed while starting up. It's the first thing to look at when a mod doesn't seem to be doing anything.

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

- **Game text.** Change any string in the game's string tables, by key or by matching text, in every language. The platform merges the edits from every mod and writes them to Extra Options' override folder, so two mods editing the same table don't overwrite each other.
- **Title-screen notices.** Lines of your own under "PRESS (A) TO START", stacked with every other mod's.
- **Update checks.** Name your GitHub repo and the title screen tells players when you publish a newer release.
- **Version checks.** Say which platform version you ship with, and players get told when an older one is installed.
- **Logging.** Lines go to `fh2-mods\platform.log`, tagged with your mod's name.
- **Finding the game.** Azimuth maps the game into its own process when you press Play, and you get its base address so you can read or hook it.

The whole API is one C header, [`include/fh2mod.h`](include/fh2mod.h), and it only ever grows: a mod built against an older version keeps working with newer platforms.

[`docs/writing-a-mod.md`](docs/writing-a-mod.md) walks through making, building and shipping a mod from scratch. [`docs/api.md`](docs/api.md) covers every function. [`docs/string-tables.md`](docs/string-tables.md) explains the game's text files and how to find the key for a string, and [`scripts/dump_strings.py`](scripts/dump_strings.py) lists them for you. [`docs/game-notes.md`](docs/game-notes.md) collects what's known about the game build everyone plays on, from how Azimuth maps it to where the Drivatars and radio live. [`examples/hello_mod`](examples/hello_mod/hello_mod.cpp) is a complete mod in about fifty lines.

Mods from before the platform, which start themselves from `DllMain`, still load. They just don't get the services.

## Build from source

The platform is built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and CMake. On Linux:

```bash
./scripts/build.sh
```

`dist/` then holds `d3dcompiler_47.dll`, the example mod, the API header, a `README.txt` and the license. GitHub Actions does the same on every push, and a tag like `v2.0.0` attaches the zip to a release. The version lives in `project(... VERSION x.y.z)` in `CMakeLists.txt`; bump it before tagging, since the tag build refuses to run if the two don't match.

A mod usually adds this repo as a git submodule and builds the platform alongside itself, so its release zip always carries a matching `d3dcompiler_47.dll`. [`docs/writing-a-mod.md`](docs/writing-a-mod.md) shows the CMake for that.

Configuring with `-DFH2MOD_TESTS=ON` builds an offline test, which runs under Wine as a stand-in for `azimuth.exe` with the example mod, a test mod and an old-style mod in `fh2-mods`. It needs `EN.zip` from your own install: `tests/run_platform_test.sh <EN.zip> [forza_durango_release_final.exe]`.

## License

Released under the [MIT License](LICENSE), so you can build anything on it, open source or not.

## Disclaimer

Unofficial fan-made software. Not affiliated with, endorsed by, or connected to Playground Games, Turn 10 Studios, Xbox Game Studios, Microsoft, the Azimuth developers, or the Extra Options author. Use at your own risk.

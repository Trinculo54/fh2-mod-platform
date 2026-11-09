<h1 align="center" id="title">FH2 Mod Platform</h1>

The shared base for **Forza Horizon 2** mods on PC. It loads every mod in one folder, gives them a small set of services so they don't each have to reinvent them, and keeps them from stepping on each other's files. If you play with FH2 mods, you already have it: every mod built on it ships a copy. If you make mods, this is where to start.

It's the `d3dcompiler_47.dll` that sits next to `azimuth.exe`, and from 2.7.4 the same file a second time as `DWrite.dll`. Azimuth loads them when it starts: `d3dcompiler_47.dll` up to Azimuth 1.0.1, `DWrite.dll` from 1.0.1 on, and only `DWrite.dll` from 1.0.3. Windows looks for them in Azimuth's own folder first, and every function in them forwards straight to the real one in System32, so shaders compile and text shows exactly as before. When Azimuth loads both, only one of them runs. The only extra thing it does is load the DLLs in the `fh2-mods` folder and look after them. It was the FH2 Mod Loader up to version 1.1, and became the platform with 2.0.

Mods currently built on it are [Achievements](https://github.com/Trinculo54/fh2-achievements), which brings back the game's Xbox achievements offline, [FH2 Universal Radio](https://github.com/Trinculo54/FH2-Universal-Radio), which plays Spotify and more on the in-game radio, [WheresMyBuddy](https://github.com/Trinculo54/WheresMyBuddy), which puts your Xbox friends in the game as Drivatars, and [WreckingBall](https://github.com/Trinculo54/WreckingBall), which makes trees, fences, railings, roadside clutter and telephone poles break when you drive into them. It works alongside [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions), which loads through `winmm.dll`.

## What's new in 2.9.0

For players:

- **One home for mod settings.** Every mod has its own page under Pause > MODS > YOUR MODS, and Pause > Settings > MODS opens the same list ([below](#where-to-find-mod-settings)).
- **Your language.** The MODS tab, every mod's page and the restart prompt are in the language the game shows, and mods can now be translated as well.
- **Pop-ups and tips from mods**, on the game's own notification and paced around the game's own. You decide whether they show, on the platform's own page at the end of YOUR MODS, where Recent pop-ups also says which mod sent which.
- **Car packs:** new cars for the Autoshow from the `fh2-cars` folder, on Azimuth 1.0 or later without Extra Options ([docs/car-packs.md](docs/car-packs.md)).
- **Fewer crashes:** a car whose drivetrain isn't in the game's database is listed without it instead of stopping the game ([issue #23](https://github.com/Trinculo54/fh2-mod-platform/issues/23)), and a mod's page that would crash the game shows "This page couldn't be shown" instead.
- **Online play:** mods that shouldn't run online are stopped even when FH2MP only turns out to be online after they started ([issue #22](https://github.com/Trinculo54/fh2-mod-platform/issues/22)). FH2MP's launcher puts back the platform it ships every time you press Play, so online players get 2.9.0 once an FH2MP release ships it.
- **Proton:** mods' text, files and database changes reach the game without Extra Options there too ([PR #17](https://github.com/Trinculo54/fh2-mod-platform/pull/17)).

For mod makers, API 10 adds translations, the UI templates (pop-ups, tips, message boxes, UI sounds and your pages' help bar and hint line, with no XAML of your own) and the HUD and pad layers as previews; `set_file` now refuses the platform's own UI files. [docs/release-notes-2.9.0.md](docs/release-notes-2.9.0.md) has the whole list.

## For players

There's nothing to install by hand. Each mod's zip includes `d3dcompiler_47.dll` (and `DWrite.dll` from platform 2.7.4), and extracting any of them into your Azimuth folder puts the platform in place. Azimuth 1.0.3 only loads `DWrite.dll`, so there you need a mod that ships platform 2.7.4 or newer, or `DWrite.dll` from the platform's own release zip. On Azimuth 1.0.1, which loads both, an older `d3dcompiler_47.dll` put back by another mod's zip can't stand down, so it runs instead of the newer `DWrite.dll`. `fh2-mods\platform-standby.log` then says so, and reinstalling a mod with the newest platform fixes it. Each mod ships the platform it was built with, so keep the newest one in place: if you install a mod that ships an older platform, reinstall a mod that has the newest afterwards. The first line of `fh2-mods\platform.log` says which version is running. Mods that need a newer platform say so on the title screen, under "PRESS (A) TO START", and reinstalling one of them fixes it. Other mods can't tell, and an older platform can do worse: up to 2.7.1 it replaced a game database another mod had put in `mediaoverride` (a traffic car mod's, say), where 2.7.2 keeps it. From 2.7.3 the platform also makes the game read `mediaoverride` when Extra Options isn't loaded, for example on Azimuth 1.0.1, which Extra Options doesn't support yet ([game-notes.md](docs/game-notes.md#the-override-folder)). If an old mod from before the platform puts back the original FH2 Mod Loader, newer mods keep their main features but lose the extras, and they say so in their logs.

Azimuth 1.0.1 can run other Forza games too, such as Forza Motorsport 5 and 6, from the same launcher. From 2.8.0, when you start one of those, the platform and its mods leave it alone: nothing is hooked or patched in it, and `platform.log` says which game was started. Your FH2 mods are back as usual the next time you start Forza Horizon 2.

**Car packs.** From 2.9.0 the platform loads new cars for the Autoshow from car packs, each pack a folder in `fh2-cars` next to `azimuth.exe`, with no installer and nothing written to the game folder. They need Azimuth 1.0 or later without Extra Options. On such an install the platform also keeps what it writes for the game (mods' text, files and database changes) in the Azimuth folder, so Azimuth folders sharing one game folder no longer see each other's mods. All of it is in [docs/car-packs.md](docs/car-packs.md).

### Where to find mod settings

The platform turns the pause menu's old MARKETPLACE tab, the last one, into **MODS**. Its tiles are YOUR MODS, UPDATES, GET MODS (opens the mod site in your browser), ABOUT and HELP. From platform 2.9.0:

- **Every mod has its own page under Pause > MODS > YOUR MODS.** The list shows each mod's version and state ("Off", "Off online", "New", a yellow "Update x.y.z" when a new version is out, or a red "! Not working"), and its description ends with what A shows: "Press A for its 13 settings.", say. **Pause > Settings > MODS opens the same list**, and B goes back to whichever you came from.
- **A mod's page, "MODS / <MOD>", has everything the mod added:** its switches, sliders and choices in its own order, its hotkeys under "Keys and buttons", its links (they open in your browser) and its pages, then "About <Mod>" with what it does, **Load this mod** (its on/off switch) and, where FH2MP is installed, Load when playing online. The page opens on the mod's first setting. Left and right change a row, and changes are saved as you make them.
- **Settings marked \*** take effect the next time you start the game. The panel under the list names the ones waiting, and when you leave, the game offers to restart for you; Go back returns you to the page and row you left.
- **The marks on the MODS tab:** a red ! means a mod isn't working (its row in YOUR MODS says why), a yellow ! means a new version is out (see UPDATES), and NEW means a mod you haven't looked at yet. The tab shows one at a time, and the tile that sorts it out shows it too.
- **To turn a mod off**, use Load this mod at the bottom of its page. Outside the game, move its DLL out of `fh2-mods`.
- Until you've opened YOUR MODS once, the title screen says where mod settings are, the MODS tab has a NEW mark, and the first few times you drive the game says which mods are loaded. A mod you add later gets a "NEW MOD" notice once. All of this is kept in `fh2-mods\platform-seen.txt`; deleting it brings the hints back.

**If MODS is missing**, the game isn't reading the platform's menu files, and from 2.9.0, when a mod has settings or pages you'd miss, a message box says so once for each platform version. With Extra Options, update it, keep `EnableMediaOverrides=1` in `FH2_ExtraOptions.ini`, and check the first line of `FH2_ExtraOptions.log`. `fh2-mods\platform.log` says why. Your mods keep running meanwhile; to turn one off, move its DLL out of `fh2-mods`.

Before platform 2.9.0, Pause > Settings > MODS opened a page of its own with every mod's switches, and each mod's full page was under MODS > INSTALLED MODS. A mod zip that ships an older platform brings that layout back until a mod with platform 2.9.0 or newer is installed again.

The same spot on the title screen tells you when one of your mods has a new release on GitHub. Each mod decides whether it checks, and most let you turn it off in their settings.

From 2.9.0 the MODS tab, every mod's page and the restart prompt are in the game's language (Brazilian Portuguese, Dutch, French, German, Italian, Japanese, Korean, Polish, Russian, Spanish and Traditional Chinese; the game's Czech and Hungarian text is English, and so is the platform's there). So are the MODS tab's tiles, with their counts. The title-screen notices, "MODS" and product names stay in English.

Everything the platform does is logged to `fh2-mods\platform.log`: which mods it loaded and their versions, which game text, files and data it changed and for whom, and any mod that failed to load or crashed while starting up. It's the first thing to look at when a mod doesn't seem to be doing anything. If the game itself crashes, the platform writes a report to `fh2-mods\crashes` naming the mod the crash came from, if any, which is the thing to attach when you report it to that mod's author.

**Racing wheels ([fh2wheel](https://github.com/iamAzoDx/fh2wheel)).** fh2wheel ships its own `D3DCOMPILER_47.dll` too, a small loader that only loads `fh2wheel.dll`, and only one of the two files can sit next to `azimuth.exe`. From platform 2.6.1 the platform loads `fh2wheel.dll` itself when it finds it there, so to use both: install fh2wheel as its instructions say, then install (or reinstall) a platform mod, so that the platform's `d3dcompiler_47.dll` is the one in place. Keep `fh2wheel.dll` and `fh2wheel-config.exe` next to `azimuth.exe`. fh2wheel then shows up on the MODS tab, `platform.log` says it loaded it, and its settings stay in `fh2wheel-config.exe`. If fh2wheel's `D3DCOMPILER_47.dll` ends up in place instead, the wheel works but none of the platform's mods load. Azimuth 1.0.3 doesn't load any `D3DCOMPILER_47.dll`, so fh2wheel's own loader doesn't run there, but the platform's `DWrite.dll` still loads `fh2wheel.dll`.

**On Linux (Wine or Proton).** The platform works as it is, but Extra Options needs a DLL override (`WINEDLLOVERRIDES="winmm=n,b"`), database changes need platform 2.7.1 or newer, and a few mods have limits there. Without the override, platform 2.7.3 or newer still gets mods' text, file and database changes to the game (under Proton, 2.9.0 or newer). On Azimuth 1.0.3 the platform is `DWrite.dll`, and Wine has its own copy of that, so if `fh2-mods\platform.log` never appears, add `dwrite` to the override. All of it is in [docs/linux.md](docs/linux.md).

To remove the platform, delete `d3dcompiler_47.dll`, `DWrite.dll` and the `fh2-mods` folder (and `fh2-cars` and `data\titles\1A96545B\tmp\fh2-platform`, if you have them). That removes every mod and car pack built on it.

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

- **Game text.** Change any string in the game's string tables, by key or by matching text, in every language. The platform merges the edits from every mod and writes them to the game's override folder (`mediaoverride`, or from 2.9.0 the Azimuth folder's own), so two mods editing the same table don't overwrite each other.
- **Title-screen notices.** Lines of your own under "PRESS (A) TO START", stacked with every other mod's.
- **Title-screen entries.** From platform 2.8.0, a line with a button of its own there, like "(Y) START ON STORM ISLAND", that runs your code and can start the game for the player, and tells you afterwards that it did.
- **Update checks.** Name your GitHub repo and the title screen tells players when you publish a newer release.
- **Version checks.** Say which platform version you ship with, and players get told when an older one is installed.
- **Logging.** Lines go to `fh2-mods\platform.log`, tagged with your mod's name.
- **Finding the game.** Azimuth maps the game into its own process when you press Play, and you get its base address.
- **Game files.** Replace any file the game loads. One mod per file, and it's cleaned up when the mod is removed.
- **The game database.** Change cars, events and the rest of the game's data with SQL. Every mod's changes go into one copy of the database.
- **Other mods.** Publish functions for other mods to call, look up theirs, and check which mods are installed.
- **In-game settings.** On/Off switches, sliders and, from platform 2.8.0, rows of named choices ("LB / RB / A...") on your mod's own page under Pause > MODS > YOUR MODS (Pause > Settings > MODS opens the same list). You're told when the player changes one, and switches that need a restart get the game's own restart prompt. From platform 2.9.0, the rows' text can follow the game's language: you're told which language the game shows, and can change the text once it's known.
- **Online play.** From platform 2.8.0, a mod that shouldn't run online (a nitro boost, say) can say so, and isn't loaded when the game is started from the [FH2MP](https://github.com/Charelene/FH2MP) launcher. Players can choose for any mod on its page under MODS, and mods can ask whether the game is online. From 2.9.0, if FH2MP turns out to be online only after the mods have started (its session written late, or FH2MP under another file name), the platform stops those mods for the rest of the session, and tells FH2MP about them before it connects.
- **Pages of your own.** From platform 2.7.0, lists of your own in the game's menus, laid out like its options pages, with a row to open them under MODS. A page can also take over the pause menu's Achievements tile, which does nothing on PC. There it can be XAML of your own design, with bindings, A, B, X, Y and the bumpers of its own, and the tile itself can show a line of text and colours of your choosing.
- **Hooks.** Detour game functions, single calls or vtable slots through one shared copy of MinHook, and from platform 2.6.0 hook any single instruction (reading and changing the registers there) or patch bytes in place. Mods can hook the same place side by side, the platform refuses patches that would clash with another mod's or with Extra Options', and every hook is logged and listed in crash reports.
- **Crash reports.** When the game crashes, a report in `fh2-mods\crashes` says where, which mod it came from, and which hooks were in place. From platform 2.8.0 its minidump has the mods' own data too, plus any memory a mod adds, and a full-memory dump can be switched on while chasing a crash.
- **Sessions and logs.** From platform 2.8.0, each run of the game has an id, `platform.log` is kept per session in `fh2-mods\platform-logs`, mods get log files named after the session, and `fh2-mods\platform-session.json` tells tools what's running. A mod being tested can ask for a copy of the saves before the game reads them.
- **Hotkeys.** From platform 2.8.0, a key or a controller chord that calls your mod back while the game is in front, rebindable on your mod's page.
- **Commands.** From platform 2.8.0, commands other programs can run while the game does, through a pipe only the same user can reach, and the `fh2mod` tool in the zip: `fh2mod MyMod shadows off`.
- **Your text in the player's language.** From platform 2.9.0, translations built into your DLL as PO files and looked up in the language the game shows, with each language's plural forms, numbers and dates, and string-table text in each language's own words ([`docs/translating.md`](docs/translating.md)).
- **Pop-ups, tips and message boxes.** From platform 2.9.0, the game's own notification, message box and UI sounds, and your pages' help bar and hint line, filled with your text: the platform draws them, decides when they show and takes no XAML from you, and the player decides whether pop-ups and tips show.
- **A HUD and controller buttons of your own (previews).** From platform 2.9.0, a bar, labels and a feed in the game's free-roam HUD, and buttons of your own while the player holds a modifier. They aren't part of the API yet and will change ([`docs/hud-layer-preview.md`](docs/hud-layer-preview.md), [`docs/pad-layer-preview.md`](docs/pad-layer-preview.md)).

The whole API is one C header, [`include/fh2mod.h`](include/fh2mod.h), and it only ever grows: a mod built against an older version keeps working with newer platforms. Next to it, [`include/fh2game.h`](include/fh2game.h) names the places in the game's code and data that mods already use, with the bytes to check before hooking them. [`include/fh2mod_ui.h`](include/fh2mod_ui.h) and [`include/fh2mod_l10n.h`](include/fh2mod_l10n.h) are header-only helpers for the 2.9.0 calls, which do nothing (or give the English) on an older platform, and [`include/fh2mod_hud.h`](include/fh2mod_hud.h) and [`include/fh2mod_pad.h`](include/fh2mod_pad.h) are the two previews' interfaces.

[`docs/writing-a-mod.md`](docs/writing-a-mod.md) walks through making, building and shipping a mod from scratch. [`docs/api.md`](docs/api.md) covers every function. [`docs/string-tables.md`](docs/string-tables.md) explains the game's text files and how to find the key for a string, and [`scripts/dump_strings.py`](scripts/dump_strings.py) lists them for you. [`docs/translating.md`](docs/translating.md) is the translation workflow, for mod makers and translators. [`docs/game-notes.md`](docs/game-notes.md) collects what's known about the game build everyone plays on, from how Azimuth maps it to where the Drivatars, the radio and the game database live, and lists the game sites the platform hooks. [`examples/hello_mod`](examples/hello_mod/hello_mod.cpp) is a complete mod in about fifty lines.

Mods from before the platform, which start themselves from `DllMain`, still load. They just don't get the services. From 2.8.0 they load once the game Azimuth starts is known to be FH2, as they change the game by themselves.

## Build from source

The platform is built with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and CMake. On Linux:

```bash
./scripts/build.sh
```

`dist/` then holds `d3dcompiler_47.dll` and `DWrite.dll`, the example mod, every header in `include/` (`fh2mod.h`, `fh2game.h`, the helpers and the previews' interfaces), the `fh2mod` command tool, the translation scripts in `tools/l10n`, a `README.txt` and the licence files. GitHub Actions does the same on every push, and a tag like `v2.0.0` attaches the zip to a release. The version lives in `project(... VERSION x.y.z)` in `CMakeLists.txt`; bump it before tagging, since the tag build refuses to run if the two don't match.

A mod usually adds this repo as a git submodule and builds the platform alongside itself, so its release zip always carries a matching `d3dcompiler_47.dll`. [`docs/writing-a-mod.md`](docs/writing-a-mod.md) shows the CMake for that.

Configuring with `-DFH2MOD_TESTS=ON` builds an offline test, which runs under Wine as a stand-in for `azimuth.exe` with the example mod, two test mods and an old-style mod in `fh2-mods`. It needs `EN.zip` and `gamedbRC.slt` from your own install: `tests/run_platform_test.sh <EN.zip> <gamedbRC.slt> [forza_durango_release_final.exe]`. The same script runs the database test, a few dozen launches with other mods' databases in `mediaoverride` and `mediapc`, FH2MP's online copy, and files that can't be written. The hook test needs only the game exe, and runs five launches (no Extra Options, Extra Options finishing late, Wine's own winmm.dll, game code that isn't as expected, and a crash inside a hook); it covers instruction hooks and byte patches too, and `PreciseTest.dll` checks instruction hooks inside the running game: `tests/run_hook_test.sh <forza_durango_release_final.exe> [another game's exe]`. Given another Forza game's exe as well (Forza Motorsport 5's, say), it runs two more launches, with that game picked in the launcher and with `--skip-launcher`, to check that the platform leaves it alone. From 2.9.0 the platform test runs twice, as an Azimuth before 1.0 (files in `mediaoverride`) and as Azimuth 1.0 or later (the folder's own layer), and the two previews have tests of their own: `tests/run_hud_layer_test.sh` and `tests/run_pad_test.sh <forza_durango_release_final.exe>`.

## License

Released under the [MIT License](LICENSE), so you can build anything on it, open source or not. The platform includes [MinHook](https://github.com/TsudaKageyu/minhook) (in `third_party/minhook`), which is under the BSD 2-Clause license; `dist/LICENSE-MinHook.txt` is its notice, to ship alongside `d3dcompiler_47.dll`. It also includes [SQLite](https://sqlite.org) (in `third_party/sqlite`), which is in the public domain.

## Disclaimer

Unofficial fan-made software. Not affiliated with, endorsed by, or connected to Playground Games, Turn 10 Studios, Xbox Game Studios, Microsoft, the Azimuth developers, or the Extra Options author. Use at your own risk.

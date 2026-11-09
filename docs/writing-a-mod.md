# Writing a mod

This walks through making an FH2 mod on the platform, from an empty folder to a zip players can install. The example is [`examples/hello_mod`](../examples/hello_mod/hello_mod.cpp); copying it is a fine way to start.

## What you need

A mod is a 64-bit Windows DLL, written in anything that can export a C function. C++ is what the existing mods use, and the instructions here assume it.

To build, you need either [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) with CMake, which works on Linux and Windows and is what every FH2 mod uses so far, or Visual Studio with CMake. If you plan to use `__try`/`__except` around reads of game memory, llvm-mingw needs `-fms-extensions`, and GCC-based MinGW won't do at all.

To test, you need Forza Horizon 2 running through Azimuth. The platform writes text, file and database changes to the game's `mediaoverride` folder. [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions) makes the game read it, and most players have it already. When Extra Options isn't loaded, platform 2.7.3 and newer makes the game read the folder itself. From 2.9.0, on Azimuth 1.0 or later without Extra Options, the changes go to the Azimuth folder's own layer instead, `data\titles\1A96545B\tmp\fh2-platform\media`, which the game reads ahead of `mediaoverride` ([car-packs.md](car-packs.md#this-azimuth-folders-own-files)).

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

`fh2mod_api` adds the include path for `fh2mod.h` and defines `FH2MOD_PLATFORM_VERSION` as the platform version in the submodule. Building gives you `MyMod.dll` and the platform's `d3dcompiler_47.dll`, plus, from platform 2.7.4, a copy of it named `DWrite.dll`.

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

You don't need it for updates: register with `github_repo` and the platform checks your latest GitHub release (at most every six hours, remembered between launches, and again straight away when a different version of your mod is installed) and adds "MyMod 1.1.0 is out: Pause > MODS > UPDATES" itself when it's newer than your `version` ("MyMod 1.1.0 is available" up to platform 2.8.0). It compares `x.y.z` numbers, with or without a leading `v`, so tag releases to match your version. It also adds "Old FH2 Mod Platform: reinstall MyMod" when the installed platform is older than your `platform_version`.

From platform 2.8.0 (API 9), `add_title_entry` adds a line there with a button of its own, like "(Y) START ON STORM ISLAND", and calls your code when the player presses it. Your code can have the platform start the game straight after, and `title_started_by` then tells your mod that this start was its entry's. There are four buttons for every mod together, so add one entry at most. [`api.md`](api.md#title-screen-entries-api-9) has the details.

## Replacing game files

To swap out a whole file the game loads, such as a texture, an XML file or the Drivatar cache, use `set_file(mod, "drivatarglobalcache/DefaultCache.bin", data, size)` instead of writing into `mediaoverride` yourself. The path is relative to `media`, and a zip counts as a folder: `media\UI.zip\x.xml` is `"UI/x.xml"`. The platform makes sure only one mod replaces a given file and logs it when a second one tries. It also deletes the file once your mod stops claiming it, whether you call `clear_file` or the player removes your mod. Check the return value: 0 means you didn't get the file. These calls need API version 2, so check `api->api_version >= 2` first. From platform 2.9.0 the platform's own menu files (the pause menu, the Mods page, the MODS tab's pages, the HUD and the title screen, listed in [`api.md`](api.md#set_file-and-clear_file-api-2)) can't be replaced: for a page of your own, use pages (API 8). A page of your own design (`set_page_xaml`) is checked before it's shown: every resource key it uses has to be one every page has or one of its own, and every texture one of the game's, or the page shows "This page couldn't be shown" instead of crashing the game. A page that crashed the game as it opened is left out until your DLL changes, or until the player gives it another try with A. [`api.md`](api.md#pages-of-your-own-design) has the rules.

## Changing the game database

Most of the game's data, from car stats and prices to events and AI drivers, is in a SQLite database, `media\Stripped\gamedbRC.slt`. Open a copy in any SQLite browser to look around (`Data_Car` is a good place to start). Then change it with plain SQL:

```cpp
api->add_sql(g_mod, "UPDATE Data_Car SET BaseCost = BaseCost / 2");   // half-price Autoshow
```

Every mod's statements run against one copy of the database, in load order, so two mods can change different things in it, or even the same table, without either replacing the other's copy. If your SQL has an error, none of your changes are made and `platform.log` gives SQLite's error message. Use `UPDATE` and `INSERT` rather than dropping and recreating tables, so other mods' changes survive.

## Working with other mods

If your mod has something other mods could use, publish it. Write a small header with a struct of function pointers, starting with a `size` member, and call `publish_interface(mod, "MyMod.Thing", 1, &my_struct)` in `fh2mod_init`. Another mod includes your header, and then, on its own thread, waits with `wait_for_mods` and calls `get_interface("MyMod.Thing", 1)`. If it gets null, your mod isn't installed or is too old, and it should carry on without it. `mod_version("MyMod")` tells a mod whether another is installed at all.

Once an interface is published, keep it working: add members at the end and bump the version, never change existing ones. WheresMyBuddy and FH2 Universal Radio both publish one: `WheresMyBuddy.Friends` and `FH2UniversalRadio.NowPlaying`.

## When it crashes

If the game crashes, look in `fh2-mods\crashes`. Each crash gets a text report, with the call stack and the mod it came from, plus a minidump to open in a debugger. When players report a crash, ask them for the text file.

## Working with the game

Azimuth maps the game, `forza_durango_release_final.exe`, into its own process when you press Play, so it isn't there yet when your mod starts. `wait_for_game(timeout)` blocks until it is and returns its base address; `game_base()` returns it right away, or null. Call `wait_for_game` from your own thread, not from `fh2mod_init`.

Only one build of the game is in circulation: the Xbox One build from 24 November 2015. Addresses in it are fixed, so mods can use offsets from the base. Even so, check that the code or data you're about to change looks the way you expect before you patch it, and log and skip if it doesn't. A wrong guess is then a missing feature, not a crash. [`game-notes.md`](game-notes.md) lists what's known about the build, and the existing mods are worth reading. FH2 Universal Radio redirects calls inside the game's FMOD radio, WheresMyBuddy swaps a virtual function on a save-game class, and [WreckingBall](https://github.com/Trinculo54/WreckingBall) hooks seventeen functions in the game's collision, physics and rendering code, checking that each looks right before it does.

Before hunting for an address yourself, look in [`include/fh2game.h`](../include/fh2game.h) (platform 2.8.0). It names the places in the game's code and data that mods already use, `FH2_GAME_COLLISIONWORLD_GETPOLYS` and the like, with what each one is, which mods use it, and, as `FH2_GAME_..._BYTES`, the bytes the game has there for your check. It's a plain C header with nothing to link; include it next to `fh2mod.h`. When you find a new one, add it by pull request, with the bytes read from the exe on disk.

## Changing game code

Hook through the platform (API 3) instead of patching memory yourself. `hook_function` detours a whole function, `hook_call` redirects one call instruction, and `hook_pointer` swaps a function pointer such as a vtable slot. The platform makes the patch, gives you the `original` to call on, and lets other mods hook the same code alongside you. It also refuses code that Extra Options has already changed, and lists your hooks in crash reports. [`api.md`](api.md#hooks-api-3) has the details and an example.

Two rules matter. First, get the game's base from `wait_for_code`, not `wait_for_game`, before hooking game code, so Extra Options gets to patch first. It returns null when there's no FH2 to hook: Azimuth 1.0.1 can start other Forza games too, whose exes have the same name and address, and the platform refuses any hook into one of those ([`api.md`](api.md#game_base-and-wait_for_game)). Stop when it's null. Second, game code runs on many threads, so your detour can be called from any of them as soon as the hook is on. Set up everything it needs first, then call `enable_hooks`, and make the detour safe to run on several threads at once.

If you can't use the platform's hooks for something (a patch in the middle of a function, say), still check the bytes are what you expect before you change them, and write them atomically.

## Switches in the game

If players will want to turn parts of your mod on and off, or tune a number, give them rows (API 4; sliders API 5) on your mod's own page under Pause > MODS > YOUR MODS (Pause > Settings > MODS opens the same list). You pass the current value and get told when the player changes it; you keep saving it in your own settings file, as before. Rows that only work after a restart are marked so, and the game then offers to restart for the player. From platform 2.8.0 (API 9) a row can also offer named choices, like a button or a mode, instead of a number standing for one. [`api.md`](api.md#your-mods-page-api-4-sliders-api-5) has the details and an example. Keep it to the main switches; fine-tuning still belongs in your ini. A few rules make your page easy to use:

- **Put your main switch first.** The page opens with the focus on your first row.
- **Keep labels to about 24 characters, and leave your mod's name out of them.** They have half the list's width, and the page's title is already your mod's name.
- **Write `FH2ModInfo.description` for players.** It's the first thing they read about your mod, on the YOUR MODS list.
- **Link rows and pages show on the same page**, after your settings, so one place has everything.

For your README, a sentence players can follow: "In game: Pause > MODS > YOUR MODS > MyMod (Pause > Settings > MODS opens the same list)."

## Text in the player's language

From platform 2.9.0 (API 10), your rows, pages, title-screen lines and pop-ups can be in the language the game shows. Write the English with a key for each text (`FH2_TR(api, g_mod, "row.Trees.label", "Breakable trees")`, from [`include/fh2mod_l10n.h`](../include/fh2mod_l10n.h), which gives the English on an older platform), list them for translators with `tools/l10n/extract.py`, and build their PO files into your DLL with `tools/l10n/po2inc.py`. Text you pass on as `FH2_TR` gave it follows the game's language by itself, even though the game picks it after `fh2mod_init`. [`translating.md`](translating.md) has the whole workflow, for you and for your translators. To find text that's cut off or never translated, put `test=1` in `fh2-mods\platform-language.ini`: every translatable text then shows padded and in brackets.

## Showing things to the player

From platform 2.9.0 (API 10) a mod never needs XAML to tell the player something: the UI templates are pieces of the game's own UI that you fill with text, and the platform draws them in the game's look, decides when they show and logs what they showed. [`api.md`](api.md#ui-templates-api-10) has every call in full. The rows marked 2.9.1 and 2.9.x below are planned, not in 2.9.0.

| You want to | Use | Since |
|---|---|---|
| Tell the player something happened (an unlock, a file written) | `show_toast` (or `fh2ui_notify`) | 2.9.0 |
| Answer the player's own key press | `show_toast` with `FH2_TOAST_FLAG_REPLY`; `play_ui_sound` if it returns 0 | 2.9.0 |
| Report a fault the player must act on | `show_toast` with `FH2_TOAST_FLAG_URGENT`, saying what to do | 2.9.0 |
| Teach a control once | `show_tip` | 2.9.0 |
| Ask for a confirmation or a choice | `show_message_box`, from your page | 2.9.0 |
| Say what the buttons do on your page | `set_page_help` | 2.9.0 |
| Show a short status on your page ("Saved.") | `set_page_hints` and `show_page_hint` | 2.9.0 |
| Say something before the game starts | `set_notice`, `add_title_entry` | 2.8.0 |
| Show a state that lasts while driving (a ghost, a score) | a card or status line | 2.9.1 |
| Acknowledge a press within a tenth of a second | a chip or status line | 2.9.1 |
| Count down to something | `show_countdown` | 2.9.x |
| Run a mode with its own controls | the HUD layer and pad layer previews (below); an API for them later | 2.9.0 (preview) |
| Watch numbers while testing | `set_overlay_text` | 2.9.x |

Use [`include/fh2mod_ui.h`](../include/fh2mod_ui.h): each call there is the API member without `api` and `mod`, and does nothing (returning 0) on a platform older than 2.9.0, so your mod needs no version checks. [`include/fh2mod_l10n.h`](../include/fh2mod_l10n.h)'s `FH2_TR` gives the text in the player's language. Set both up once in `fh2mod_init`, and say what you'll use there too: the platform only hooks the game's notification in a session where a mod has declared pop-ups or tips.

```cpp
#include "fh2mod_ui.h"
#include "fh2mod_l10n.h"

g_mod = api->register_mod(&info);
fh2_helpers_init(api, g_mod);
fh2ui_ui_uses(FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS);   // ui_uses: from fh2mod_init only
```

What works this session, whenever it matters (`ui_status`):

```cpp
const uint32_t ui = fh2ui_ui_status();
if (ui & FH2_UI_TOASTS_OFF) { /* the player turned pop-ups off: don't nag in other ways either */ }
if (ui & FH2_UI_KEYBOARD)   { /* buttons are written as keys (the keyboard was used last, or the player chose it): name keys */ }
```

A pop-up, in one call or in full (`show_toast`), and taking a waiting one down (`clear_toast`):

```cpp
fh2ui_notify(FH2_TR(FH2_API, FH2_MOD, "toast.ready", "Breakable trees are on"),
             FH2_TR(FH2_API, FH2_MOD, "toast.ready.line", "Change it under Pause > MODS"));

FH2Toast t{};
t.size     = sizeof t;
t.key      = "Snapshot";                          // a second one with this key replaces it while it waits
t.title    = FH2_TR(FH2_API, FH2_MOD, "toast.snapshot", "Snapshot written");
t.subtitle = FH2_TR(FH2_API, FH2_MOD, "toast.where", "WreckingBall-defs.txt, next to the game");
t.icon     = FH2_ICON_PHOTO;
t.flags    = FH2_TOAST_FLAG_REPLY;                // from a hotkey's on_press: it answers the press
if (!fh2ui_show_toast(&t)) fh2ui_play_ui_sound(FH2_SOUND_SELECT);   // off, or an older platform

fh2ui_clear_toast("Snapshot");                    // it isn't true any more: take it down if it's still waiting
```

A fault the player has to fix: `FH2_TOAST_FLAG_URGENT`, and say what to do.

```cpp
FH2Toast f{};
f.size     = sizeof f;
f.key      = "NoData";
f.title    = FH2_TR(FH2_API, FH2_MOD, "toast.nodata", "Track data missing");
f.subtitle = FH2_TR(FH2_API, FH2_MOD, "toast.nodata.fix", "Reinstall MyMod from its zip");
f.flags    = FH2_TOAST_FLAG_URGENT;               // first in the queue; twice a session
fh2ui_show_toast(&f);
```

A tip that teaches a control once (`show_tip`), until the player has used it (`set_tip_seen`), and asking whether it was seen (`tip_seen`):

```cpp
FH2Tip tip{};
tip.size          = sizeof tip;
tip.key           = "FirstSnapshot";
tip.text          = FH2_TR(FH2_API, FH2_MOD, "tip.snapshot", "Press {button} to write a snapshot");
tip.button.hotkey = "Snapshot";                   // the player's own binding for your hotkey
tip.flags         = FH2_TIP_FLAG_UNTIL_DONE;
tip.max_sessions  = 3;
if (!fh2ui_tip_seen("FirstSnapshot")) fh2ui_show_tip(&tip);
/* in the hotkey's on_press */ fh2ui_set_tip_seen("FirstSnapshot", 1);
```

A sound on its own, to answer a press when there's nothing to say (`play_ui_sound`); never the only cue:

```cpp
fh2ui_play_ui_sound(FH2_SOUND_SELECT);
```

A confirmation from your page (`show_message_box`, from the page's own callbacks only):

```cpp
static void on_reset_choice(void*, const char*, uint32_t choice) {
    if (choice == 1) reset_progress();            // 0 is "Keep my progress"; FH2_BOX_BACK is Back
}
static void on_select(void*, const char*, const char* row) {
    if (strcmp(row, "Reset") != 0) return;
    const char* choices[] = {FH2_TR(FH2_API, FH2_MOD, "box.keep", "Keep my progress"), FH2_TR(FH2_API, FH2_MOD, "box.reset", "Reset")};
    FH2MessageBox b{};
    b.size         = sizeof b;
    b.key          = "Reset";
    b.title        = FH2_TR(FH2_API, FH2_MOD, "box.title", "Reset your progress?");
    b.message      = FH2_TR(FH2_API, FH2_MOD, "box.msg", "Every achievement goes back to locked. Your save isn't touched.");
    b.choices      = choices;
    b.choice_count = 2;
    b.on_choose    = on_reset_choice;
    fh2ui_show_message_box(&b);
}
```

Your page's help bar (`set_page_help`) and a hint line for short statuses (`set_page_hints`, `show_page_hint`):

```cpp
FH2Prompt open{};
open.glyph = FH2_GLYPH_A;
open.label = FH2_TR(FH2_API, FH2_MOD, "help.open", "Open");
fh2ui_set_page_help("Main", &open, 1);            // relabels A; (B) Back stays

const char* hints[] = {FH2_TR(FH2_API, FH2_MOD, "hint.saved", "Saved."), FH2_TR(FH2_API, FH2_MOD, "hint.reset", "Progress reset.")};
fh2ui_set_page_hints("Main", hints, 2);           // in fh2mod_init or the page's on_open
/* in on_reset_choice, after resetting */ fh2ui_show_page_hint("Main", 1, FH2_HINT_INFO);
```

**The player stays in charge.** The platform's own page under YOUR MODS has "Mod pop-ups" and "Tips" for every mod, "Show tips again" and "Recent pop-ups" (which mod sent what), and your page gets a "Pop-ups" row when you declare pop-ups, unless you have a row of your own (`FH2_UI_USES_OWN_SWITCH`). So write each pop-up as if it might not show: keep what matters in your log and on your page too.

**Trying them out.** With `Developer=1` under `[popups]` in `fh2-mods\platform-popups.ini`, the platform's page gets a "UI templates" page that fires each template once, and `fh2mod Platform ui demo`, `ui toast`, `ui tip`, `ui keyboard` and `ui pad` do the same from a command line; `fh2mod Platform ui` alone says what's on screen and waiting, and why. [`api.md`](api.md#trying-templates-fh2mod-platform-ui-and-the-test-page) has them all.

## A HUD of your own (preview)

From platform 2.9.0, the `FH2Platform.HudLayer` interface (version 2) lets a mod show a bar, labels, button prompts and a feed of its own in the game's free-roam HUD, drawn by the game's UI in its own look, and `FH2Platform.PadLayer` (version 1) lets it have some controller buttons to itself while the player holds a modifier. Both are preview interfaces: they aren't part of the API yet, they'll change before they join it, and platform 2.8.0 and older don't have them, so your mod has to carry on without them. [`hud-layer-preview.md`](hud-layer-preview.md) and [`pad-layer-preview.md`](pad-layer-preview.md) have how to use them. One layer is on screen at a time, so if your mod could use the HUD layer, ask first.

## Your own files

Keep your settings, logs and other data in a folder of your own next to `azimuth.exe`, named after your mod, such as `MyMod\` or `mymod\`. The existing mods use `fh2-radio\` and `wheresmybuddy\`. `api->azimuth_dir` gives you that folder and `api->game_dir` the game's `Mount` folder. Don't put data in `fh2-mods`: anything in there ending in `.dll` gets loaded as a mod, and the platform keeps its own files there.

## Shipping it

A release zip, extracted into the Azimuth folder, should end up like this:

```
d3dcompiler_47.dll        the platform, from your build
DWrite.dll                the same file, from your build (platform 2.7.4 and later)
fh2-mods\MyMod.dll
MyMod\...                 your data, if any
```

Always include the platform, under both names: Azimuth 1.0.3 only loads `DWrite.dll`, and Azimuth before 1.0.1 only `d3dcompiler_47.dll`. Since 2.2 the platform has MinHook built in, which is under the BSD 2-Clause license, so include `LICENSE-MinHook.txt` from the platform's `dist` folder in your zip too. A player who only installs your mod needs it, and the version checks take care of an older copy getting put back. A short `README.txt` that says where things go and where your log is will save you a lot of questions.

Before releasing, start the game with only your mod and check `fh2-mods\platform.log`. It should show your DLL loaded with its version, your `registered` line, and the tables, files and database changes you made. Then try it next to the other mods.

Put a version in your DLL's file properties (a `VERSIONINFO` resource) as well. The platform logs it next to the file name, and players can see it under Properties → Details. Both existing mods generate theirs from the CMake project version; see `src/version.rc` and `cmake/version.h.in` in either repo.

## Etiquette

A few things keep mods from tripping over each other:

- **Don't proxy DLLs yourself.** `d3dcompiler_47.dll` and `DWrite.dll` are the platform's and `winmm.dll` is Extra Options'. If you need to load early, the platform is already as early as it gets.
- **Hook through the platform.** Your own copy of MinHook, or your own byte patches, can't see other mods' hooks, and the platform can't warn about clashes with them.
- **Don't write into the game's own files.** Use the string-table, file and database functions, so that removing your mod puts everything back.
- **Make your features switchable.** Give each one a setting, and when it's off, undo what you did, for example with `clear_strings`.
- **Mind online play.** If your mod changes driving or what other players would see, set `offline_only` in your `FH2ModInfo` (platform 2.8.0), so it isn't loaded when the game is started from the FH2MP launcher, or check `playing_online()` and turn just that part off. [`api.md`](api.md#online-play-api-9) explains both.
- **Don't take ages to start.** Do your setup on your own threads.
- **Log what you change.** It's how people debug, you included.

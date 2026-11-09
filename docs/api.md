# API reference

Everything is in [`include/fh2mod.h`](../include/fh2mod.h). This page goes through it member by member. Each API version came with a platform release, and the sections for the later ones say which:

| API | Platform | Added |
|---|---|---|
| 1 | 2.0 | `register_mod`, `log`, string tables, title-screen notices, `game_base` and `wait_for_game` |
| 2 | 2.1 | file overrides, database changes, interfaces between mods |
| 3 | 2.2 | hooks and `wait_for_code` |
| 4 | 2.3 | settings rows (on each mod's page under MODS > YOUR MODS from 2.9.0) |
| 5 | 2.4 | sliders |
| 6 | 2.5 | mod descriptions and link rows |
| 7 | 2.6 | byte patches and instruction hooks |
| 8 | 2.7 | pages of your own |
| 9 | 2.8 | title-screen entries, choice rows, online play (FH2MP), sessions, save snapshots, hotkeys, commands, crash-dump memory |
| 10 | 2.9 | text in the game's language, translations, and the UI templates (pop-ups, tips, message boxes, UI sounds and page help) |

API 10 only adds members at the end, so a mod built for API 9 or older loads on platform 2.9.0 without a rebuild. A few rules did change in 2.9.0 for every mod: `set_file` refuses the platform's own UI files ([set_file](#set_file-and-clear_file-api-2)), a mod that shouldn't run online is stopped when FH2MP turns out to be online after the mods started ([Online play](#online-play-api-9)), and pop-ups and tips need declaring ([ui_uses](#ui_status-and-ui_uses)).

## Rules that apply everywhere

- **Threads:** every function is thread-safe and can be called from any thread.
- **Strings:** strings passed in are copied, so you can free yours straight after. Names, versions, log lines and notices are UTF-8 (`const char*`). Game text is UTF-16 (`const wchar_t*`), because that's what the game uses. Text the platform puts into its pages (your rows and choices, your pages' titles and rows, the MODS tab) is UTF-8 too. From 2.9.0, letters beyond ASCII reach the game as they are, as long as its font has them: before, the game read those pages as Windows-1252, so a `·` showed as `Â·`. Bytes that aren't valid UTF-8 are still read as Windows-1252, so text written that way looks the same as before.
- **Memory:** nothing the platform returns needs freeing, and the `FH2ModApi` pointer stays valid until the process ends.
- **Compatibility:** members are only ever added at the end of `FH2ModApi`. If you use one added after version 1, check `api->api_version` (or `api->size`) first, so your mod still runs on an older platform.

## Entry point

```c
FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api);
```

Export this from your DLL. The platform loads every DLL in `fh2-mods` in alphabetical order, then calls each one's `fh2mod_init` in the same order, on its own thread and outside the loader lock. Return 0 on success. Any other value, or a crash inside the call, is logged; the crash is caught, so it won't take the game down. Once every mod's `fh2mod_init` has returned, the platform writes all string-table edits and notices.

DLLs without `fh2mod_init` still load, and are logged as mods from before the platform. From platform 2.8.0 they load once the game Azimuth starts is known to be FH2, and not at all for another game ([game_base](#game_base-and-wait_for_game)), since they change the game by themselves.

## FH2ModApi fields

| Field | Meaning |
|---|---|
| `size` | `sizeof(FH2ModApi)` as the platform knows it. |
| `api_version` | The platform's `FH2MOD_API_VERSION`. |
| `platform_version` | The platform's version, like `"2.2.0"`. |
| `azimuth_dir` | The folder with `azimuth.exe`, without a trailing slash. Put your data folder here. |
| `game_dir` | The game's `Mount` folder (`game:\`), or `""` if it wasn't found. |

## register_mod

```c
FH2Mod* register_mod(const FH2ModInfo* info);
```

Call once, from `fh2mod_init`. It returns your handle, or null if `info` is missing or too small. Fill in every member of `FH2ModInfo`:

| Member | Meaning |
|---|---|
| `size` | `sizeof(FH2ModInfo)` |
| `name` | Display name, used in the log and in notices. |
| `version` | Your version, `x.y.z`. |
| `github_repo` | `"Owner/Repo"`, or null. When set (and `check_updates` is 1), the platform checks your latest release on GitHub, at most every six hours, and shows "Name x.y.z is out: Pause > MODS > UPDATES" on the title screen when it's newer than `version` ("Name x.y.z is available" up to platform 2.8.0). The six hours start over when a different `version` is installed, and a remembered release older than `version` is never trusted, so installing an older or newer build checks again straight away. |
| `platform_version` | The platform version you ship with. Pass `FH2MOD_PLATFORM_VERSION`. If the installed platform is older, players get "Old FH2 Mod Platform: reinstall Name". |
| `check_updates` | 0 to never go online for your mod. Give players a setting for it. |
| `description` | API 6: a sentence or two for your row on Pause > MODS > YOUR MODS and the "What it does" row on your mod's page, or null. It's the first thing players read about your mod. |
| `offline_only` | API 9: nonzero if your mod shouldn't run in online play. See [Online play](#online-play-api-9). |

The update check runs in the background. If GitHub can't be reached, the last answer is used.

## log

```c
void log(FH2Mod* mod, const char* utf8_line);
```

Writes one line to `fh2-mods\platform.log`, with a timestamp and `[YourMod]` in front. The log starts fresh at every launch. Use it for the things a player or you would want to see when something goes wrong. A mod with a lot to say should keep its own log as well.

## set_string

```c
void set_string(FH2Mod* mod, const char* table, uint16_t key, const wchar_t* text);
```

Sets the string with `key` in `table` (for example `"Main.str"`) to `text`, in every language. To find keys, see [`string-tables.md`](string-tables.md). If two mods set the same key, the one loaded later wins, and the log says so.

## replace_string

```c
void replace_string(FH2Mod* mod, const char* table, const wchar_t* from, const wchar_t* to);
```

Replaces every string in `table` that is exactly `from` (the whole string, case-sensitive) with `to`. This runs after key edits, so it sees their results. It's handy for text that's the same in every language, like the names in `AIPlayers.str`. Each language's table has its own text, so English `from` only matches the English tables (EN and GB, and CZ and HU, whose text is English); for your text in every language's own words, see [set_string_tr](#translations-api-10).

## clear_strings

```c
void clear_strings(FH2Mod* mod, const char* table);
```

Drops your edits to `table`, or to every table if `table` is null. When no mod edits a table any more, its override files are removed, so the game uses its own text again. That happens straight away when you call `clear_strings` and then `apply_strings`. For a table left over from an earlier launch, such as one from a mod that has since been removed, it happens about 15 seconds after startup, which leaves mods time to make their edits from their own threads first.

## apply_strings

```c
void apply_strings(FH2Mod* mod);
```

Writes every table that changed since the last write. It's done for you once all mods have returned from `fh2mod_init`, so call it yourself only for edits you make later. The game reads its string tables once, at startup, so a later edit shows from the next launch.

Edits are written to Extra Options' `mediaoverride` folder, as `mediaoverride\Stripped\StringTables\<language>\<table>`. Extra Options makes the game read that folder. When Extra Options isn't loaded, platform 2.7.3 and newer does it instead ([game-notes.md](game-notes.md#the-override-folder)). With Extra Options loaded and `EnableMediaOverrides=0`, the files are still written but the game won't read them. From 2.9.0, on Azimuth 1.0 or later without Extra Options, they go to the Azimuth folder's own layer instead (`data\titles\1A96545B\tmp\fh2-platform\media\Stripped\StringTables\...`), which the game reads ahead of `mediaoverride` ([car-packs.md](car-packs.md#this-azimuth-folders-own-files)). The tables the platform wrote are listed in `fh2-mods\platform-tables.txt`, so that ones no longer needed get cleaned up.

## set_notice

```c
void set_notice(FH2Mod* mod, const char* utf8_lines);
```

Sets your lines on the title screen, under "PRESS (A) TO START". Separate lines with `\n`; empty lines are dropped, and null or `""` clears them. Each line fits about 40 characters. Your lines appear together with every other mod's, sorted by mod name, with the platform's own update and version lines for your mod after yours.

From platform 2.9.0, lines you got from [tr()](#translations-api-10) are written into each language's `Main.str` in that language's words, so the title screen matches the game's language whatever it picks.

They're saved to `fh2-mods\notices\<name>.txt` and written into `Main.str` the next time strings are applied, which happens on its own after `fh2mod_init`. Mods from before the platform (WheresMyBuddy 1.1 and 1.2, FH2 Universal Radio 1.2) read and write the same files, so their notices and yours always end up together.

## game_base and wait_for_game

```c
void* game_base(void);
void* wait_for_game(uint32_t timeout_ms);
```

`game_base` returns the base address of `forza_durango_release_final.exe` once Azimuth has mapped it into the process, or null before that. Azimuth maps it when the player presses Play. `wait_for_game` waits until it's mapped, or until `timeout_ms` runs out (`0xFFFFFFFF` waits forever), and returns the base or null. It checks every 100 ms. Call it from your own thread, never from `fh2mod_init`, which would hold up every mod loaded after you.

The game is recognised by its PE header: the 24 November 2015 build, timestamp `0x565501ED` and image size `0x03A18000`. Azimuth maps it at its preferred base, `0x140000000`, but use the returned address anyway.

**Other games.** Azimuth 1.0.1 runs other Forza games from the same launcher (Forza Motorsport 5 and 6, for instance), and the platform and your mod load before the player picks one. Their exes have the same name, `forza_durango_release_final.exe`, and are mapped at the same address. From platform 2.8.0, once Azimuth has mapped one of those, or an FH2 exe of another build, `game_base` returns null for the rest of the session, and `wait_for_game` and `wait_for_code` return null straight away instead of at their timeout. So treat null as "there's no FH2 to change" and stop there, as for a timeout. Anything you did in `fh2mod_init` (text, files, the database) only ever goes to FH2's own folder, so it can stay. The log says which game was started: `game: Azimuth started Forza Motorsport 5 (title 7b35d917), not Forza Horizon 2. ...`, with the name from `data\azimuth.ini`. When Azimuth's command line already says it's another game (`--skip-launcher` with another title in `[launcher] game=`, or `--game=DIR`), no mods are loaded at all. Older platforms keep waiting, and a mod that finds the game by itself has to check the exe's timestamp and size, as above.

## set_file and clear_file (API 2)

```c
int set_file(FH2Mod* mod, const char* media_path, const void* data, size_t size);
void clear_file(FH2Mod* mod, const char* media_path);
```

`set_file` replaces a file the game reads from `game:\media`. Give the path relative to `media`, with forward or back slashes. For a file inside a zip, use the zip's name without `.zip` as a folder: `media\UI.zip\x.xml` is `"UI/x.xml"`. The file is written straight away to the `mediaoverride` folder, which Extra Options mounts over `media`, or the platform from 2.7.3 when Extra Options isn't loaded. From 2.9.0, on Azimuth 1.0 or later without Extra Options, it goes to the Azimuth folder's own layer instead, which the game reads ahead of `mediaoverride`. If the file there already has the same content, it isn't rewritten.

Only one mod can replace a file. `set_file` returns 1 when the file is yours and written. It returns 0 if another mod already replaces it, if the path isn't a plain path under `media`, if it's one of the platform's own files (below), or if the write failed, and the log says which. String tables and the game database can't be replaced whole; use the functions for those. If someone had put a file at that spot by hand, it's kept next to it as `<name>.bak` the first time.

**The platform's own files.** From 2.9.0 these are the platform's alone, and `set_file` refuses them in any spelling (any case, `/` or `\`, with `media\` in front or not, `UI.zip\` for `UI\`, or the path inside the zip on its own):

| File under `media` | What the platform uses it for |
|---|---|
| `UI\Scenes\Anthem\A_Hud.xaml` | the HUD notice about mods that didn't load, and the HUD layer |
| `UI\Scenes\Anthem\A_Splash.xaml` | the title screen's mod entries |
| `UI\Scenes\Anthem\A_PauseMenu.xaml` | the pause menu's MODS tab |
| `UI\Scenes\Anthem\A_PauseOptions.xaml` | the Mods tile in the pause menu's Settings |
| `UI\Scenes\Anthem\A_ModOptions.xaml` | the list that stands in for the MODS tab when its hooks don't go in |
| `UI\Scenes\Anthem\A_ModPage_NN.xaml` (any number) | the MODS tab's pages and the mods' pages |
| `stateflow\pause.xml` | how the pause menu opens the MODS tab's pages |
| `stateflow\ui_screens.xml` | the MODS tab's screens |

Before 2.9.0 the first to set one of these won, and since mods start before the platform writes its menus, a mod that replaced one took the MODS tab (or the title screen's entries, or the notice) away from every player. Other UI files, a HUD control such as `UI\Controls\HUDSpeedometerControl.xaml` say, are still yours to replace. For a page of your own, use [pages](#pages-api-8).

`clear_file` deletes your override, so the game reads its own file again. Pass null to clear every file you replace. The files the platform wrote are listed in `fh2-mods\platform-files.txt`. About 15 seconds after startup, any listed file that no mod has claimed this launch is deleted. That's what cleans up after a mod that was removed.

The game reads most files once, when it first needs them, so call `set_file` early, from `fh2mod_init` or soon after.

## add_sql, clear_sql and apply_database (API 2)

```c
void add_sql(FH2Mod* mod, const char* utf8_sql);
void clear_sql(FH2Mod* mod);
void apply_database(FH2Mod* mod);
```

`media\Stripped\gamedbRC.slt` is a SQLite database that holds most of the game's data: cars (`Data_Car`), events, AI drivers and around 280 more tables. `add_sql` queues statements to run against it. One call can hold several statements separated by semicolons, and calls add up.

The platform copies the database the game would read and runs every mod's statements, in load order, each mod inside its own transaction. If any statement from a mod fails, all of that mod's changes are rolled back and the SQLite error is logged, while the other mods' changes still apply. Car packs' statements run before the mods' ([car-packs.md](car-packs.md#database-rows)), and from 2.9.0 a car name a mod or pack gives as text (`Data_Car.DisplayName`, `ModelShort`) becomes a new key in `Data_Car.str`, since the top bar only shows names from the string tables. The result goes to `mediaoverride\Stripped\gamedbRC.slt`, which the game reads first.

Car packs' cars get Ids from 3000 to 8999, and their rows `Id * 1000 + n` ([car-packs.md](car-packs.md#database-rows)). A mod that adds a car by an Id of its own should give it one from 9000 up: the platform never gives those to a pack's car. Pack cars' Ids are picked around what mods' statements add, but one a pack car has had since an earlier start keeps it, and while a mod adds rows by it, that pack is left out. The MODS tab warns about a mod that adds rows by an Id from 3000 to 8999 while a car pack adds cars.

From 2.9.0, on Azimuth 1.0 or later without Extra Options, it goes to the Azimuth folder's own layer instead (`data\titles\1A96545B\tmp\fh2-platform\media\Stripped\gamedbRC.slt`), which only that Azimuth folder's game reads, so its mods' changes don't reach other Azimuth folders using the same game folder. That copy is built on another mod's database in `mediaoverride` (or a plain copy of the game's there), else on `mediapc`'s, else on `media`'s; never on another Azimuth folder's copy or on FH2MP's copy of the server's, and `mediaoverride`'s file isn't written. A copy the platform put in `mediaoverride` before is taken out, as when no mod has statements (below), so what another mod had there comes back. Its record is `fh2-mods\platform-db-layer.txt`. With FH2MP in the Azimuth folder (an `fh2mp` folder there) the database is built in `mediaoverride` as before: online play needs exactly the server's database, which FH2MP puts there, and a copy in the layer would hide it. The rest of this section is about `mediaoverride`'s copy.

Other mods put a database of their own there too, such as traffic car mods, and FH2MP puts the server's there for online play. From platform 2.7.2 the platform's copy is built on what's there:

- **Another mod's database in `mediaoverride\Stripped`** is built on. A copy of it is kept as `fh2-mods\platform-db-base.slt`.
- **Otherwise a car pack's in `mediapc\Stripped`** is built on, and without one, the original in `media\Stripped`.
- **FH2MP's online copy** is built over. FH2MP puts it back when you play online. The platform knows it by its bytes, or by the size and date the server's database had at earlier launches. A date up to 2 s off still counts when it's a whole hundredth of a second, as every date is on a game drive with FAT32 (which keeps dates to 2 s) or exFAT. Any other date has to be the same, so another mod's database that's the same size and dated a moment from the server's isn't taken for FH2MP's copy.
- **When no mod has statements any more**, or none of them could be applied, the platform's copy is deleted and the other mod's database is put back, byte for byte and with its own date. It's also put back when the platform's copy was deleted along with the last mod that changes the database, even after FH2MP put the server's database in its place online. While FH2MP is installed, the platform keeps a copy of another mod's database even when no mod has statements, and puts it back after you've played online.
- **A plain copy of the game's database, or a car pack's,** put in `mediaoverride\Stripped` means the other mod was uninstalled that way. Its kept copy isn't built on or put back any more, and goes to `fh2-mods\platform-db-base.previous.slt`.
- **A file the platform replaces that it can't make again**, such as a copy another install of the platform built, is kept as `fh2-mods\platform-db-base.previous.slt`. Only the last one is kept, and the log says each time.

Up to 2.7.1 the platform always built from `media`'s database, so another mod's database in `mediaoverride` was lost. A copy an older platform left there is built again, or deleted once no mod changes the database. That includes a plain copy of the game's, which 2.7.1 left when no statement changed a row.

Every copy the platform builds has the platform's mark in its header (`application_id` 0x46483244, "FH2D"; the game doesn't read it), and `fh2-mods\platform-db.txt` has its size and date. That's how the platform tells its own copy from other mods' databases. So:

- **Don't delete `mediaoverride\Stripped\gamedbRC.slt` in your mod's uninstall steps.** The platform deletes its own copy and puts back what was there. If another mod still changes the database, deleting it means the copy is built again without the other mod's database, which is then only kept in `platform-db-base.previous.slt`.
- **Don't ship the platform's copy in your mod.** Because of the mark, it's never built on: the platform builds over it from the game's own database. If your mod ships a database of its own, start from the game's original, or set `PRAGMA application_id = 0` on it.
- **Two Azimuth folders sharing one game folder** don't know each other's copy. Keep the mods that change the database in one of them. FH2MP can be in the other: from platform 2.7.2, each folder lists the size and date of its FH2MP's copies in `mediaoverride\Stripped\platform-db-fh2mp.txt`, so the other folder doesn't take the server's database for another mod's. From 2.9.0 each has its own on Azimuth 1.0 or later (above), unless FH2MP is in it.

Building the copy takes a moment, so it's only redone when something changed. `fh2-mods\platform-db.txt` remembers a hash of the database it's built on and every mod's statements, and the log repeats the results from the last build.

Statements added in `fh2mod_init` are applied for you. For later ones, call `apply_database` from your own thread. Like string tables, the database is read once at startup, so later changes show from the next launch.

The platform has SQLite built in (3.53.4 from platform 2.7.1; before that it used Windows' own `winsqlite3.dll`, which Wine and Proton don't have). If your mod reads a SQLite file itself, build SQLite in too rather than loading `winsqlite3.dll`, or it won't work for Linux players ([linux.md](linux.md)). To look around the database, open a copy of it in any SQLite browser.

## Interfaces between mods (API 2)

```c
void publish_interface(FH2Mod* mod, const char* name, uint32_t version, const void* iface);
const void* get_interface(const char* name, uint32_t min_version);
int wait_for_mods(uint32_t timeout_ms);
const char* mod_version(const char* name);
```

A mod can offer other mods a struct of function pointers. Publish it under a name, usually `"YourMod.Something"`, with a version number. Ship a small header that defines the struct, and treat it the way the platform treats `FH2ModApi`: start it with a `size` member, never change or remove a member, add new ones only at the end, and bump the version when you do. `iface` must stay valid for as long as the process runs. A name belongs to the first mod that publishes it. Publishing null withdraws it.

`get_interface` returns the struct if it's published with at least `min_version`, or null. Mods start in alphabetical order, so the mod you want may not have published yet while your `fh2mod_init` runs. `wait_for_mods` waits until every mod's `fh2mod_init` has returned, and returns 1 once they have. Call it from your own thread; from inside `fh2mod_init` it returns 0 straight away, since waiting there would hang every mod after yours.

`mod_version` gives the version a mod registered with, matched by name without regard to case, or null if it isn't installed. Use it to check for a mod you depend on and to tell the player, perhaps with `set_notice`, when it's missing or too old.

WheresMyBuddy publishes `WheresMyBuddy.Friends`, with the names on the Drivatars and the signed-in gamertag (see `include/wheresmybuddy.h` in its repo). FH2 Universal Radio publishes `FH2UniversalRadio.NowPlaying`, with the song that's playing (`include/fh2universalradio.h`).

The platform publishes three interfaces of its own, before any mod starts, so they're there from `fh2mod_init`, and platform 2.8.0 and older return null for them:

- **`FH2Platform.HudLayer`** (version 2, [`include/fh2mod_hud.h`](../include/fh2mod_hud.h)): a bar, labels, prompts and a feed of a mod's own inside the game's free-roam HUD. A preview: it isn't part of this API and will change ([`hud-layer-preview.md`](hud-layer-preview.md)).
- **`FH2Platform.PadLayer`** (version 1, [`include/fh2mod_pad.h`](../include/fh2mod_pad.h)): controller buttons of a mod's own while the player holds a modifier. A preview too ([`pad-layer-preview.md`](pad-layer-preview.md)).
- **`FH2Platform.Online`** (version 1, in `fh2mod.h`): for FH2MP, to say it's going online ([below](#for-fh2mp-fh2platformonline-platform-290)).

## Hooks (API 3)

```c
FH2Hook* hook_function(FH2Mod* mod, const char* name, void* target, void* detour, void** original);
FH2Hook* hook_call(FH2Mod* mod, const char* name, void* call_site, void* detour, void** original);
FH2Hook* hook_pointer(FH2Mod* mod, const char* name, void** slot, void* detour, void** original);
int enable_hooks(FH2Mod* mod, FH2Hook* hook);
int disable_hooks(FH2Mod* mod, FH2Hook* hook);
void* wait_for_code(uint32_t timeout_ms);
```

These change what game code does, and they're the way to do it when more than one mod might touch the same code. The platform keeps one list of every hook from every mod. It checks each new hook against that list and against the game as shipped, and it writes the patches safely while the game's threads are running. Function hooks are made with [MinHook](https://github.com/TsudaKageyu/minhook), which is built into the platform, so your mod doesn't need a copy of its own.

There are three kinds:

- **`hook_function`** puts an inline hook at the start of a function, so every call to it goes to your detour.
- **`hook_call`** retargets one `CALL rel32` (`E8`) or `JMP rel32` (`E9`) instruction, so only that call goes to your detour, and other callers of the same function aren't affected. `*original` is the function it called. Your detour can be anywhere: the platform puts a stub within reach of the call.
- **`hook_pointer`** replaces a function pointer in memory: a vtable slot, a callback in a table, an import. It has to be 8-byte aligned. `*original` is the pointer's old value.

A call site's 4-byte target is swapped in one atomic write where it fits in an aligned 8 or 16 bytes. Otherwise the platform pauses the other threads for the moment it takes, and makes sure none of them is on that instruction.

Each call returns a hook that is switched off, or null with the reason in the log. `name` appears in the log and in crash reports, so name what you hooked: `"FMOD::System::createSound"`. The detour must have exactly the signature and calling convention of the code it replaces. `*original` is set before the call returns. Call it from your detour to run the replaced code, which includes any other mod's detour that runs after yours. It's safe to call at any time. While your hook is switched off, it goes straight to the game's own code, past any other mod's detour.

`enable_hooks` and `disable_hooks` switch one hook, or all of yours if you pass null. Switching several at once is better, because MinHook pauses the game's threads once for all of them, not once each. They return 1 if every hook is now switched the way you asked, and 0 if any couldn't be, with the reason in the log. When the last hook on a place is switched off, the game's original bytes go back in.

**Several mods on one function.** When two mods hook the same function, call site or pointer, both hooks run. The most recently enabled detour runs first, and its `original` leads to the next one, and so on down to the game's own code. Any hook can be switched off without breaking the others: the chain closes up around it. Hooks that only overlap are refused, whatever their kind and whoever made them: a function hook on code that contains another mod's call hook, or a second function hook two bytes into a hooked function. The log names the mod already there.

**Game code that was already changed.** Before hooking game code, the platform compares the bytes it would change (or, for a function, the bytes MinHook copies) with `forza_durango_release_final.exe` in the game folder. If they differ, something outside the platform already patched them (Extra Options, or a mod patching the game by itself), and the hook is refused rather than layered on top. Pointers are only compared when the exe has an address in the image there, such as a vtable slot. Imports and tables the game fills in at runtime can't be checked this way, so they're allowed. Extra Options changes around 130 places in the game, listed in its `patches.json`. If you need one of those, talk to its author.

**wait_for_code.** Hooks in game code can't be made until `wait_for_code` has returned. It waits like `wait_for_game` and then, if Extra Options is installed, also until Extra Options has finished patching. Extra Options checks every place it patches before it changes any of them, and if a single one isn't as it expects, it patches nothing at all, so a mod hooking early could quietly switch Extra Options off. The platform knows Extra Options is done when its log (`FH2_ExtraOptions.log`, written this launch) says so. That's the status line it writes once it's done (`OK:`, `REFUSED:` or `DISABLED:`), or its `AZIMUTH: winmm proxy initialized` line. If it hasn't said so 20 seconds after the game was mapped, the platform goes ahead anyway and logs that. Hooks outside the game (in Azimuth or a Windows DLL) don't have to wait.

**Other games.** When Azimuth 1.0.1 has started another game (see [game_base](#game_base-and-wait_for_game)), `wait_for_code` returns null, and from platform 2.8.0 every hook or patch into that game's exe is refused, whatever address you pass: `can't hook function 0x140001000 (...): it's in Forza Motorsport 5 (title 7b35d917), which the platform leaves alone`.

```cpp
using SerializeFn = char (*)(void* self, void* archive);
static SerializeFn g_serialize;

static char my_serialize(void* self, void* archive) {
    const char r = g_serialize(self, archive);
    // ... look at the loaded object ...
    return r;
}

static DWORD WINAPI start(LPVOID) {
    char* base = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    void** slot = reinterpret_cast<void**>(base + 0x2D44F20) + 2; // ChampionshipDrivatarsState vtable, slot 2
    if (g_api->hook_pointer(g_mod, "ChampionshipDrivatarsState::Serialize", slot,
                            reinterpret_cast<void*>(&my_serialize), reinterpret_cast<void**>(&g_serialize)))
        g_api->enable_hooks(g_mod, nullptr);
    return 0;
}
```

Every hook is logged when it's made and switched (`[MyMod] hooked function game+0x1997740 (FMOD::System::createSound)`), and crash reports list them all, marking the ones that were on. A crash inside a MinHook trampoline (the copy of the game's first instructions) is reported as such, with the hook it belongs to.

## Byte patches and instruction hooks (API 7)

```c
FH2Hook* patch_bytes(FH2Mod* mod, const char* name, void* at, const void* expect, const void* bytes, uint32_t size);
FH2Hook* hook_instruction(FH2Mod* mod, const char* name, void* at, FH2InstructionFn fn, void* user);

typedef struct FH2Context {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint64_t reserved;
    FH2Xmm xmm[16];             /* .f[4], .d[2], .u32[4], .u64[2] */
} FH2Context;
typedef void (*FH2InstructionFn)(FH2Context* ctx, void* user);
```

For when a hook on a whole function, one call or one pointer is too blunt: a constant in the middle of a long function, a branch to flip, the value a register holds at one exact instruction. Both come back as an `FH2Hook`, switched off, and are switched with `enable_hooks` and `disable_hooks` like the hooks above. The same rules hold: hooks in game code wait for `wait_for_code`, the bytes are checked against `forza_durango_release_final.exe` and against every other hook, and everything is logged and listed in crash reports. They need platform 2.6.0, so check `api_version >= 7`.

Addresses here are exact instructions, so they belong to the game build you found them in (the one Azimuth runs). If the bytes aren't what you expect, `expect` and the check against the exe on disk refuse the change rather than writing over something else.

**patch_bytes** writes `size` bytes (1 to 32) of `bytes` at `at`. Pass the bytes you expect to find there in `expect` (the same size), or null to take whatever is there as long as it matches the exe on disk. Switching the patch off puts the original bytes back. Only one patch can cover a byte, and a patch can't overlap any hook. The bytes are written with the game's other threads paused, none of them half way through them.

```cpp
// mov r8d, 1 -> mov r8d, 0: the pause menu stops hiding its Marketplace tab
static const unsigned char hide[] = {0x41, 0xB8, 1, 0, 0, 0}, show[] = {0x41, 0xB8, 0, 0, 0, 0};
if (FH2Hook* p = api->patch_bytes(mod, "Marketplace tab: show", base + 0xDDACAD, hide, show, sizeof show))
    api->enable_hooks(mod, p);
```

**hook_instruction** calls `fn(ctx, user)` every time the game reaches the instruction at `at`, before that instruction runs. `ctx` holds every general-purpose register, the flags and `xmm0` to `xmm15` as they are at that point. Change any of them (except `rsp`, which is there to read) and the game carries on with your values. `at` can be any instruction in a function, not just its first, as long as:

- it's the start of an instruction (the platform decodes the function from its start to make sure);
- it's past the function's prolog, the instructions that set up its stack frame;
- the hook's jump fits: it replaces 5 bytes, so the instructions covering them have to stay inside the function, and nothing in the function may jump into them other than to `at` itself.

When it refuses, the log says which of these it was. Inside, the replaced instructions are moved to a stub of the platform's, rewritten where they refer to nearby code or data (rip-relative operands, `CALL`, `JMP` and conditional jumps), and run there after `fn`, before the game continues after them. The stub has unwind information chained to the game function's own, so a stack walk from inside `fn`, or in a crash report, carries on through to the game function's callers. On a CPU with AVX the upper halves of `ymm0` to `ymm15` are kept too, so a `fn` compiled for AVX can't disturb the game's 256-bit values.

Several mods can hook the same instruction; all of their `fn`s run, the most recently enabled first, each seeing the changes of the ones before it. `fn` runs on the game's thread, often every frame, so keep it short. If it crashes, the crash is caught, logged under your mod's name, and that hook is skipped from then on; the game keeps running. Instruction hooks work in the game and in any loaded DLL with unwind information, like your own.

```cpp
// The freeroam HUD being shown (the Hud page's vtable +0x488, game+0xD38DD0): after
// "mov rbx, rcx", rbx is the page.
static void hud_shown(FH2Context* ctx, void*) {
    void* page = reinterpret_cast<void*>(ctx->rbx);
    // ... remember it ...
}

if (api->api_version >= 7)
    if (FH2Hook* h = api->hook_instruction(mod, "Hud shown", base + 0xD38DF2, &hud_shown, nullptr))
        api->enable_hooks(mod, h);
```

Prefer `hook_function`, `hook_call` or `hook_pointer` when one of them can do the job: they don't depend on the exact instructions inside a function. An instruction hook's register meanings come from reading the game's code at that spot, so note them next to the address, as above.

## Your mod's page (API 4; sliders API 5)

```c
int add_setting(FH2Mod* mod, const FH2Setting* setting);
int get_setting(FH2Mod* mod, const char* key);
void set_setting(FH2Mod* mod, const char* key, int value);
float get_slider(FH2Mod* mod, const char* key);              /* API 5 */
void set_slider(FH2Mod* mod, const char* key, float value);  /* API 5 */

typedef struct FH2Setting {
    uint32_t size;                 /* sizeof(FH2Setting) */
    const char* key;               /* unique within your mod: "Trees" */
    const char* label;             /* the row's label: "Breakable trees" */
    const char* description;       /* beside the list while the row is selected; may be NULL */
    int value;                     /* 0 = Off, 1 = On */
    int needs_restart;             /* nonzero: a change only takes effect the next time the game starts */
    FH2SettingChangedFn on_change; /* void (*)(void* user, const char* key, int value); may be NULL */
    void* user;
    /* API 5: sliders. Zeroed, the row is On/Off. */
    int kind;                      /* FH2_SETTING_ON_OFF, _SLIDER, _LINK (API 6) or _CHOICE (API 9) */
    float min, max;                /* the slider's range */
    float step;                    /* one press of left or right; 0 = a hundredth of the range */
    float slider_value;            /* its current value */
    float display_scale;           /* the number shown is value * this (100 shows 0.12 as 12); 0 = 1 */
    int decimals;                  /* digits after the decimal point, 0 to 3 */
    FH2SliderChangedFn on_slide;   /* void (*)(void* user, const char* key, float value); may be NULL */
    /* API 6: links */
    const char* url;               /* with kind = FH2_SETTING_LINK */
    /* API 9: choices */
    const char* const* choices;    /* with kind = FH2_SETTING_CHOICE: the names, UTF-8 */
    uint32_t choice_count;         /* 2 to FH2_SETTING_MAX_CHOICES (32) */
} FH2Setting;
```

These put On/Off switches and sliders for your mod in the game's own menus, on your mod's page under Pause > MODS > YOUR MODS. Pause > Settings gets a seventh tile, MODS, in a sixth column, whenever a mod is installed; from platform 2.9.0 it's a second door to the same YOUR MODS list. The page is laid out like the HUD options page, with the row's description beside the list, and holds everything your mod added, in a fixed order (platform 2.9.0):

1. A red "Not working" row when your mod didn't load or start (it takes the focus), or a yellow "Update available" row when a newer release is out.
2. Your rows, in the order you added them. A row that needs a restart ends in " *".
3. Your hotkeys' rows, under a "Keys and buttons" heading ([Hotkeys](#hotkeys-api-9)).
4. Your link rows (value "Browser") and your pages (value "Open"; [Pages](#pages-api-8)).
5. An "About <Mod>" heading, then "What it does" (your `FH2ModInfo.description`, your version and your GitHub page), **Load this mod** (the player's switch for your whole mod) and, where FH2MP is installed, Load when playing online.

The page opens with the focus on your first row, so put your main switch first. Labels have half the list's width: keep them to about 24 characters, and leave your mod's name out of them (the page's title is your mod's name). On the YOUR MODS list your row shows your version and state, and your description, ending in what A shows ("Press A for its 13 settings."), is the first thing players read about your mod. Up to platform 2.8.0, the rows were on a page of their own shared by every mod (Settings > MODS), with "(Mod)" after each description; from 2.9.0 that page only stands in for the MODS tab when the tab's hooks don't go in.

Add your rows from `fh2mod_init`. The platform writes the page once every mod's `fh2mod_init` has returned, so a row added after that only shows from the next launch (the log says so). `key` is letters, digits and `_`, and unique within your mod. `add_setting` returns 1, or 0 with the reason in the log.

**Your mod owns the value.** The platform doesn't store it anywhere. Pass the current value in `value`, from your ini or wherever you keep your settings. When the player changes the row, `on_change` gets the new value with your `user` pointer: save it, and start using it if it can change on the fly. `on_change` runs on the game's UI thread, inside the menu's own code, so keep it short and don't call into the game from it. A crash in it is caught and logged. `get_setting` returns a row's current value (-1 if you have no row called `key`). If you change a value yourself, from a hotkey say, `set_setting` makes the page show it the next time it opens; `on_change` isn't called for that.

**Switches that need a restart.** Some changes can only take effect when the game starts, like anything the game reads once while loading. Mark those rows `needs_restart`. Their label then ends in " *", their description ends with "Takes effect the next time you start the game.", and the panel under the list explains the mark. While any of them differs from the value the game started with, the panel names them ("Restart the game for: Breakable trees."). When the player leaves YOUR MODS with such a change, and only when a restart row has changed since the last time it asked, the game's own message box offers **Restart now**, **Restart later** and **Go back**. Restart now closes the game the way Azimuth's own Quit does, then starts Azimuth again with `--skip-launcher`, straight back into the game; **Go back** returns to the page and row the player left. Your `on_change` has already been called by then, so save there and the restart picks up the new value.

```cpp
static void changed(void*, const char* key, int value) {
    WritePrivateProfileStringA("MyMod", key, value ? "1" : "0", g_ini.c_str());
    if (std::strcmp(key, "Sparks") == 0) g_sparks = value;   // takes effect at once
}

// in fh2mod_init, after register_mod:
if (api->api_version >= 4) {
    FH2Setting s{sizeof s};
    s.key = "Sparks"; s.label = "Sparks"; s.description = "Sparks fly when you scrape a wall.";
    s.value = g_sparks; s.on_change = changed;
    api->add_setting(mod, &s);
}
```

**Sliders (API 5, platform 2.4.0).** Set `kind = FH2_SETTING_SLIDER` and the row is the game's own slider, like the audio volumes: left and right move it by `step` between `min` and `max`, and the number at its end is the value times `display_scale`, with `decimals` digits after the point. A value kept as a share, like 0.12, reads best with `display_scale = 100` and no decimals ("12"). Pass the current value in `slider_value`; `on_slide` gets each new one, already kept inside the range and on a step. `get_slider` and `set_slider` work like `get_setting` and `set_setting`. `needs_restart` works for sliders too. An older platform would show a slider as an On/Off row, so only add one when `api_version >= 5`:

```cpp
static void slid(void*, const char* key, float value) {
    char v[32];
    std::snprintf(v, sizeof v, "%g", value);
    WritePrivateProfileStringA("MyMod", key, v, g_ini.c_str());
    if (std::strcmp(key, "SparkAmount") == 0) g_spark_amount = value;
}

if (api->api_version >= 5) {
    FH2Setting s{sizeof s};
    s.key = "SparkAmount"; s.label = "Spark amount"; s.description = "How many sparks fly, in percent.";
    s.kind = FH2_SETTING_SLIDER; s.min = 0; s.max = 2; s.step = 0.05f;
    s.slider_value = g_spark_amount; s.display_scale = 100; s.on_slide = slid;
    api->add_setting(mod, &s);
}
```

**Choices (API 9, platform 2.8.0).** Set `kind = FH2_SETTING_CHOICE` and give `choices` and `choice_count`, and the row shows one of them by name, between the same left and right arrows as the game's own option rows ("Speedometer: Analogue / Digital"). Left and right step through them, stopping at the ends. `value` is the index of the one chosen, from 0, and `on_change`, `get_setting` and `set_setting` use that index (`set_setting` keeps it in range). The names are copied when you add the row; there can be 2 to 32 of them, and a dozen or so characters of each fit. `needs_restart` works as for a switch. Use it wherever a number would otherwise stand for something with a name, like a button or a mode. An older platform would show it as an On/Off row, so only add one when `api_version >= 9`:

```cpp
static const char* const kButtons[] = {"LB", "RB", "A", "B", "X", "Y"};

if (api->api_version >= 9) {
    FH2Setting s{sizeof s};
    s.key = "Button"; s.label = "Nitro button"; s.description = "The button that burns nitro.";
    s.kind = FH2_SETTING_CHOICE; s.choices = kButtons; s.choice_count = 6;
    s.value = g_button; s.on_change = changed;   // changed() gets the index: 0 for LB, 1 for RB...
    api->add_setting(mod, &s);
}
```

**Link rows (API 6).** Set `kind = FH2_SETTING_LINK` and `url` (an `http://` or `https://` address), and the row sits with your pages on your mod's page, with the value "Browser" and the address under its description. A opens it in the player's browser, behind the game, and the page stays. A link row has no value: `get_setting` and `set_setting` ignore it.

The log (`[MODS]`, `[Mods page]` up to platform 2.8.0) says when the pages are ready, and each change is logged under your mod's name ("Trees set to 0 on its MODS page"). If the pause menu's code doesn't look as expected, there's no MODS tab (its files are removed again) and Pause > Settings > MODS opens the list that stands in for it, with the same rows; if the Settings page's code doesn't either, there's no tile or page at all, and your mod keeps running and simply isn't told of changes. [`game-notes.md`](game-notes.md#menus) explains how the page is put together.

## Pages (API 8)

```c
int add_page(FH2Mod* mod, const FH2Page* page);
int set_page_rows(FH2Mod* mod, const char* page_key, const FH2PageRow* rows, uint32_t count);

typedef struct FH2Page {
    uint32_t size;                 /* sizeof(FH2Page) */
    const char* key;               /* unique within your mod: "List" */
    const char* title;             /* "Achievements" (shown in capitals) */
    const char* label;             /* the row on your mod's page that opens it; NULL = the title */
    const char* description;       /* that row's description; may be NULL */
    const char* tile;              /* a pause-menu tile that opens it: "Achievements", or NULL */
    FH2PageOpenFn on_open;         /* void (*)(void* user, const char* page_key); may be NULL */
    FH2PageSelectFn on_select;     /* void (*)(void* user, const char* page_key, const char* row_key); may be NULL */
    void* user;
} FH2Page;

typedef struct FH2PageRow {
    uint32_t size;                 /* sizeof(FH2PageRow) */
    const char* key;               /* passed to on_select; NULL: A does nothing on it */
    const char* label;             /* left: "Island Explorer" */
    const char* value;             /* right: "12 / 50"; may be NULL */
    const char* description;       /* beside the list while it's selected; may be NULL */
    const char* footer;            /* a line at the bottom of the description panel; may be NULL */
    int style;                     /* FH2_ROW_NORMAL, FH2_ROW_DIM or FH2_ROW_HEADING */
    int value_alert;               /* nonzero: the value in red */
} FH2PageRow;
```

A page is a list of your own in the game's menus, laid out like your mod's page: each row has a label on the left and a value on the right, and the selected row's description shows beside the list, with your footer line at the bottom of it. It's for showing things (stats, a collection, what your mod has found) and, with `on_select`, for simple menus where A on a row does something.

Your mod's own page under Pause > MODS > YOUR MODS gets a row for each of your pages (`label`, with `description` beside it and the value "Open"), and A on it shows the page; B goes back. A page can also take over a pause-menu tile that does nothing on PC. For now that's `"Achievements"`, the tile on the PROGRESS tab and the one on the PORSCHE tab, which on Xbox opened the console's achievements app. Only one mod can have a tile; a second one is told so in the log, and its page is still on its mod's page.

Add pages from `fh2mod_init`. `add_page` returns 1, or 0 with the reason in the log. Pages are written when every mod's `fh2mod_init` has returned, and a page added later only shows from the next launch.

**Rows.** `set_page_rows` gives a page its rows, copied, in order, replacing the ones it had (up to 1000). You can call it at any time, from any thread. The page is written again each time it's opened, from the rows you last set, so keep them up to date as your data changes. If working them out is costly, do it in `on_open`, which runs just before the page is written: when its tile opens it, and when MODS > YOUR MODS opens (from either door), since that page holds every mod's pages. Until you set any rows, the page says it has nothing on it yet.

- `style` `FH2_ROW_DIM` greys the label and value, for something locked or not available yet. `FH2_ROW_HEADING` puts the label in the game's orange, as a heading over the rows after it. The selected row is the game's usual orange bar with white text, whatever its style.
- `value_alert` shows the value in red.
- The value column is half the list's width: keep values short. Text doesn't break at `\n`; descriptions wrap on their own.

**A.** When the page has an `on_select` and the selected row has a `key`, A calls `on_select(user, page_key, row_key)` on the game's UI thread, and the help bar says Select. Keep it short and don't call into the game from it; a crash in it is caught and logged. The page shows the same rows until it's opened again, so `on_select` can do something but can't change what's on screen. Without `on_select`, the help bar only has Back.

```cpp
static void list_opening(void*, const char*) {
    std::vector<FH2PageRow> rows;
    for (const Thing& t : g_things) {
        FH2PageRow r{sizeof r};
        r.label = t.name.c_str();
        r.value = t.found ? "Found" : "Not yet";
        r.description = t.hint.c_str();
        r.style = t.found ? FH2_ROW_NORMAL : FH2_ROW_DIM;
        rows.push_back(r);
    }
    g_api->set_page_rows(g_mod, "List", rows.data(), static_cast<uint32_t>(rows.size()));
}

// in fh2mod_init, after register_mod:
if (api->api_version >= 8) {
    FH2Page p{sizeof p};
    p.key = "List"; p.title = "Things found"; p.description = "Every thing, and whether you've found it.";
    p.on_open = list_opening;
    api->add_page(mod, &p);
}
```

The log (`[MODS]`) says when a tile opens a page, and each `on_select` is logged under your mod's name. If the tile's code doesn't look as expected, the tile keeps doing what it did and the page is only on your mod's page. [`game-notes.md`](game-notes.md#menus) explains how the pages are put together.

**Text on the tile.**

```c
int set_page_tile_text(FH2Mod* mod, const char* page_key, const char* text);
```

A page on a tile can put a line of text on it, at the tile's bottom left, in the pause menu's own style for a tile's count (the club tile's member count: the game's orange, small): a score, say, `"340 / 2,000 G"`. Plain ASCII, up to 40 characters (others show as `?`); `NULL` or `""` shows nothing. Call it from any thread, whenever the text changes: the pause menu reads it each time it's shown. It returns 1, or 0 if you have no page called `page_key` or it isn't on a tile (the log says which).

```c
int set_page_tile_colors(FH2Mod* mod, const char* page_key, uint32_t background, uint32_t text);
```

The same tile's colours, as `0xAARRGGBB`: `background` for the tile, `text` for its title and your line. 0 leaves the game's own (the light grey tile, the dark title, the orange line). The icon takes the background's colour, so it stays a shade of it. The pause menu reads them each time it's shown, like the text, and the return values are the same.

The tile keeps the game's title, icon and badge; the platform gives it a copy of the game's icon tile template with the line added and the colours bound, and only on tiles a mod's page has taken over. Until you set them, the text is empty and the colours are the game's, so the tile looks as it did.

### Pages of your own design

```c
int set_page_xaml(FH2Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
int get_page_slot(FH2Mod* mod, const char* page_key, int slot);
int set_page_slot(FH2Mod* mod, const char* page_key, int slot, int value);
int focus_page_element(FH2Mod* mod, const char* page_key, const char* name);

/* in FH2Page, after user: */
FH2PageButtonFn on_button;   /* int (*)(void* user, const char* page_key, int button) */
FH2PageSlotFn on_slot;       /* void (*)(void* user, const char* page_key, int slot, int value) */
```

Rows give you the options pages' look. For anything else, a page on a tile can be XAML you write: the game's own controls (`CommonListBox`, `Button`, `local:FZTextBlock`, `local:FZMaterial`), styles, colours and textures, laid out however you like. Everything in `media\UI.zip` is there to use: `Resources/Anthem/` has the shared styles, and `Scenes/Anthem/` every page of the game, which are the best examples. Textures are referred to as `/ForzaUI;component/Textures/Anthem/<path>.png` (`media\UI\textures\Anthem.zip` lists them, as `.swatchbin`).

The page keeps the frame of the game's options pages: the blurred background, the player bar, the help bar, and the screen's padding and tilt. `content` takes the place of what the HUD options page has inside that (its title, list and description panel), so it includes your own title (`A_Style_ContentControl_ScreenTitleBlock` is the game's). It has to name one element `x:Name="FirstItem"`, which gets the focus when the page opens; name your main list `OptionsList`, as the game's options pages do. `resources` goes into the page's `ResourceDictionary`, and `help`, if you give one, replaces the help bar's `DefaultHelpPanel` (keep one element with that name); without it the help bar has only Back. The fragments use the page's XML namespaces: `local:` is the game's (ForzaUI). `set_page_xaml` returns 1, or 0 if you have no such page, `content` has no FirstItem, or a fragment's tags don't balance (then the page keeps the XAML it had, and the log says where), or if it fails the check below.

**The check (2.9.0).** The game builds the page from its XAML as it opens, and a resource key or an image it can't find crashes the game there and then, with nothing in any log. So the platform checks each `set_page_xaml` against the game's own files first (`media\UI.zip` and the texture zips in `media\ui\textures`, read once):

- Every `{StaticResource K}` and `{DynamicResource K}` (and `<StaticResource ResourceKey="K"/>`), in any of the three parts, has to name a key that every page has, or one of your own. Every page has the keys of the dictionaries `AnthemApp.xaml` merges (the `Global_*` and `Shared_*` files in `Resources/Anthem/`, about 1,100 keys) and the three converters the platform adds (`FH2Mod_ShowIf`, `FH2Mod_ShowIfHidden`, `FH2Mod_HideIf`). Your own are any `x:Key` in your fragments. A key that a game page keeps in its own `<Page.Resources>` (`A_Style_FZTextBlock_TileClubMemberCount` is only in `A_PauseMenu.xaml`), or that's only in a dictionary no page merges (the `Common_*` files, `Resources\*ResourceDictionary.xaml`), isn't there: copy its markup into your `resources`. Merging a dictionary yourself doesn't add its keys to the check. Keys given as markup (`{StaticResource {x:Type Button}}`) aren't checked.
- Every image path has to be one of the game's textures, written as the game writes them: `/ForzaUI;component/Textures/<folder>/<path>.png` (or `/Textures/...`, with `/` or `\`), which is `<path>.swatchbin` in `media\ui\textures\<folder>.zip`, and for `Data_Bound/<name>/...` in `media\ui\textures\data_bound\<name>.zip` (or `Data_Bound.zip`). A loose file counts too, as the game reads one: `ui\textures\<folder>\<path>.swatchbin` in any media folder the game reads (the folder `set_file` writes to, a car pack's, `mediaoverride`, `mediapc` or `media`), so a texture of your own that you `set_file` before you set the XAML passes. The platform looks at `Source`, `ImageSource`, `UriSource` and `Icon`, a `Setter`'s `Value` for one of them, an `<ImageSource>` element's text, and any value written in one of those two forms. A path in any other form (`Images/mine.png`, `pack://...`) is refused too.

A fragment that fails is refused: `set_page_xaml` returns 0 and the log says which key or path and in which part. As with a missing FirstItem, the page keeps the last XAML you set that passed. If none has passed yet, the page shows the platform's own row instead, "This page couldn't be shown", saying why, rather than crashing the game, until you set XAML that passes; the page's row on your mod's page under MODS > YOUR MODS says so too, in red. If the game's files can't be read, nothing is checked (a log line says so), but the page can't be written then either.

**The crash guard (2.9.0).** Just before the game loads a page of your own design, the platform marks it as opening in `fh2-mods\platform-pages.txt`, and takes the mark off once the page has been up for 10 seconds or the player has left it. If the next session finds a mark, the game stopped while it was loading that page. That alone isn't proof (the player may have closed Azimuth's window), so the page is left out only when there's also a crash report in `fh2-mods\crashes` newer than the mark, or when the same page was left opening in two sessions running; otherwise the mark is dropped and the log says so. A page left out shows "This page couldn't be shown", and its row on your mod's page under YOUR MODS says the game crashed opening it, until your mod's DLL changes, which an update or a reinstall does; the log says so each session. While you work on a page, delete its line from the file to try it again with the same DLL.

As with rows, the XAML is copied, and the page is written from the last you set each time it opens, so it's static: put your data straight into it, and call `set_page_xaml` again when it changes (or from `on_open`). On your mod's page under MODS > YOUR MODS such a page shows its rows if you've set any, and otherwise its row is dim and says where the tile is ("Open it from Pause > PROGRESS > ACHIEVEMENTS, or the PORSCHE tab's ACHIEVEMENTS tile."); a page of your own design needs a tile to be seen.

**Slots** are what makes it move. Each page has 16 ints, slot 0 to 15, which its XAML binds by name: `{Binding FH2Mod_Slot_<n>, Mode=TwoWay}` (a list's `SelectedIndex`, say), and `{Binding FH2Mod_Slot_<n>_Is_<v>}`, true while slot n is v (v from 0 to 9999). The platform puts three converters in the page's resources for `Visibility`: `FH2Mod_ShowIf` (collapsed when false), `FH2Mod_ShowIfHidden` (hidden when false: it keeps its place, and can take the focus as soon as it shows) and `FH2Mod_HideIf`. Slots are 0 when the page opens; set any you want otherwise from `on_open`. When a binding changes one, `on_slot` tells you. `get_page_slot` reads one; `set_page_slot` changes one, and from inside `on_button` the page's bindings see it at once. From `on_slot` they do too, except for the slot a binding is setting at that moment. Set from anywhere else, a slot is only stored: the page doesn't show it, and opening resets it.

A list's `SelectedIndex` follows the focus, and goes to -1 while the focus is outside the list: the game's list items are only selected while they have it. So don't show things straight from a list's selection if the focus is going to leave it. Bind the list to one slot, and in `on_slot` copy the value to another when it's 0 or more; show things from that one. Otherwise, when the focus moves to what you're showing, it hides, and the game moves the focus back to the page's main list.

**Buttons.** A, B, X, Y, LB and RB go to `on_button` first (`FH2_BUTTON_A`, `FH2_BUTTON_B`, `FH2_BUTTON_X`, `FH2_BUTTON_Y`, `FH2_BUTTON_LB`, `FH2_BUTTON_RB`). Return `FH2_PAGE_IGNORED` for what they usually do (B leaves the page; A does nothing on a page of your own design; the others whatever the game does with them there), or `FH2_PAGE_HANDLED`, `FH2_PAGE_HANDLED_SELECT` or `FH2_PAGE_HANDLED_BACK`, which also play the menus' select or back sound. From `on_button`, `focus_page_element` moves the focus to a `Button`, `Slider` or `local:SpinnerControl` your XAML names (`x:Name`); other elements can't take the focus that way. A list whose `KeyboardNavigation.DirectionalNavigation` is `Contained` keeps the d-pad inside it, so moving between two lists is yours to do: A into the second, B back, say. The page puts the focus back on `OptionsList` whenever it's lost, so while the focus is on another list, switch `OptionsList` off (`IsEnabled` bound to a slot), and switch it back on before focusing it again. Other buttons aren't passed on; the log names their codes (other than the d-pad's) the first time each is pressed on a page of your own design. To show what LB and RB do, the help bar takes `local:ControllerButtonPanel.LBumperDescription` and `RBumperDescription` (`{local:Loc HelpButtons.IDS_Previous}` and `IDS_Next` are the game's "Previous" and "Next").

A two-pane page, a list of sections and the last selected section's items beside it (slot 0: the list's selection, slot 1: which list has the focus, slot 2: the section shown):

```xml
<!-- content (abridged) -->
<StackPanel Orientation="Horizontal" VerticalAlignment="Center" HorizontalAlignment="Center">
    <local:CommonListBox x:Name="OptionsList" SelectedIndex="{Binding FH2Mod_Slot_0, Mode=TwoWay}"
                         IsEnabled="{Binding FH2Mod_Slot_1_Is_0}"
                         KeyboardNavigation.DirectionalNavigation="Contained" Width="400" Height="600"
                         Style="{StaticResource A_Style_ListBox_List_Default}"
                         ItemContainerStyle="{StaticResource A_Style_ListBoxItem_List_Default}">
        <Button x:Name="FirstItem" Template="{StaticResource My_Plain}"> ... section 0 ... </Button>
        <Button x:Name="Section_1" Template="{StaticResource My_Plain}"> ... section 1 ... </Button>
    </local:CommonListBox>
    <Grid>
        <local:CommonListBox x:Name="Items_0" Visibility="{Binding FH2Mod_Slot_2_Is_0, Converter={StaticResource FH2Mod_ShowIfHidden}}" ...>
            <Button x:Name="Item_0_0" ... />
        </local:CommonListBox>
        <local:CommonListBox x:Name="Items_1" Visibility="{Binding FH2Mod_Slot_2_Is_1, Converter={StaticResource FH2Mod_ShowIfHidden}}" ...>
            <Button x:Name="Item_1_0" ... />
        </local:CommonListBox>
    </Grid>
</StackPanel>
```

```cpp
static void page_slot(void*, const char* key, int slot, int value) {
    if (slot == 0 && value >= 0) g_api->set_page_slot(g_mod, key, 2, value);   // a section: show it
}

static int page_button(void*, const char* key, int button) {
    const int pane = g_api->get_page_slot(g_mod, key, 1), section = g_api->get_page_slot(g_mod, key, 2);
    if (button == FH2_BUTTON_A && pane == 0) {   // into the section's items (the sections off first)
        g_api->set_page_slot(g_mod, key, 1, 1);
        if (!g_api->focus_page_element(g_mod, key, ("Item_" + std::to_string(section) + "_0").c_str())) {
            g_api->set_page_slot(g_mod, key, 1, 0);
            return FH2_PAGE_HANDLED;
        }
        return FH2_PAGE_HANDLED_SELECT;
    }
    if (button == FH2_BUTTON_B && pane == 1) {   // back to the section (the sections on first)
        g_api->set_page_slot(g_mod, key, 1, 0);
        g_api->focus_page_element(g_mod, key, section == 0 ? "FirstItem" : ("Section_" + std::to_string(section)).c_str());
        return FH2_PAGE_HANDLED_BACK;
    }
    return FH2_PAGE_IGNORED;   // B on the sections leaves the page
}
```

The game's XAML reader isn't forgiving: a page it can't read can take the game down when it opens. Base your XAML on the game's own pages, change a little at a time, and check every `StaticResource` you use exists. The Achievements mod's page (`src/ui.cpp` in its repo) is a complete example.

## Title-screen entries (API 9)

```c
int add_title_entry(FH2Mod* mod, const FH2TitleEntry* entry);
const char* title_started_by(FH2Mod* mod);

typedef struct FH2TitleEntry {
    uint32_t size;                 /* sizeof(FH2TitleEntry) */
    const char* key;               /* unique within your mod: "Storm" */
    const char* label;             /* "Start on Storm Island" (shown in capitals) */
    FH2TitlePressFn on_press;      /* int (*)(void* user, const char* key); NULL for a list */
    void* user;
    int button;                    /* FH2_BUTTON_Y, _X, _RB or _LB; 0 = the next free one */
    const char* const* choices;    /* a list instead: its choices (UTF-8), or NULL */
    uint32_t choice_count;         /* 1 to FH2_TITLE_MAX_CHOICES (100) */
    FH2TitleChooseFn on_choose;    /* int (*)(void* user, const char* key, uint32_t choice) */
    const char* list_title;        /* the box's title; NULL = the label */
    const char* list_message;      /* text over the choices; may be NULL */
} FH2TitleEntry;
```

An entry is a line of your own on the title screen, under "PRESS (A) TO START" and the notices, with a button that runs your code: "(Y) START ON STORM ISLAND". Use it for something the player chooses before the game loads, like starting somewhere other than where the save left off.

- **Buttons.** Each entry has its own: Y, X, RB, then LB, in the order mods add them. Ask for one with `button` and you get it if it's free; if it's taken, you get the next free one and the log says so. There are four at most, from every mod together, so add one entry, not one per option.
- **When.** Add entries from `fh2mod_init`. The title screen is written once every mod's `fh2mod_init` has returned, so an entry added later only shows from the next launch.
- **The label.** UTF-8, shown in capitals in the game's small title-screen text, the way the game's language writes them (so "Żółw" shows as "ŻÓŁW"; platform 2.9.0, before that only a-z). About 24 characters fit beside the button; anything past 32 characters is cut, and the log says so. Before 2.9.0 the cut was at 32 bytes, which is fewer characters in Polish or Russian.
- **`on_press`** runs on the game's UI thread when the player presses the entry's button while the title screen waits for A. Keep it short: set a flag, or start a thread of your own. Return `FH2_TITLE_HANDLED` to stay on the title screen, or `FH2_TITLE_START` for the platform to press A for the player straight after (not after showing a message box: the box stays and the game doesn't start). The game then signs in, loads the save and loads the world as it always does. A press while the game is already signing in goes to the game, not to you.
- **Lists.** Give `choices` (and `choice_count`, `on_choose`) and the press shows the game's message box over the title screen instead, titled `list_title` (or the label), with `list_message` over the choices and a Back under them. Picking a choice calls `on_choose` with its position from 0, on the UI thread, with the same return values as `on_press`; Back just closes the box. `on_press` isn't called for a list and can be `NULL`. The choices are copied when you add the entry, so they're fixed until the next launch. Up to 10 fit in the box at once, and about 30 characters of each. A longer list (up to 100) comes in pages of 8, with Previous above them and Next below, and the box's title says which page it is ("PICK A TRACK  2/5"). `on_choose` still gets the choice's position in the whole list.
- **`title_started_by`** returns the key of your entry that started the game, or `NULL` if the player pressed A, another mod's entry started it, or it hasn't started yet. Pressing A on the title screen later (after going back to it) sets it back to `NULL`. Call it from any thread, typically from a hook on the world or save loading, to know whether this start is yours.

```cpp
static int on_storm(void*, const char*) { return FH2_TITLE_START; }

// in fh2mod_init
if (api->api_version >= 9) {
    FH2TitleEntry e{sizeof e, "Storm", "Start on Storm Island", on_storm, nullptr, FH2_BUTTON_Y};
    api->add_title_entry(mod, &e);
}

// later, where your mod picks the world to load
const char* by = api->api_version >= 9 ? api->title_started_by(mod) : nullptr;
if (by && std::strcmp(by, "Storm") == 0) { /* this start is Storm Island's */ }
```

A list, with the title and Back added by the game's message box:

```cpp
static const char* const kWorlds[] = {"Mainland", "Storm Island"};
static int on_world(void*, const char*, uint32_t choice) { g_world = choice; return FH2_TITLE_START; }

FH2TitleEntry e{};
e.size = sizeof e; e.key = "World"; e.label = "Choose where to start"; e.button = FH2_BUTTON_Y;
e.choices = kWorlds; e.choice_count = 2; e.on_choose = on_world;
api->add_title_entry(mod, &e);
```

The lines are part of the platform's own copy of `UI/Scenes/Anthem/A_Splash.xaml`, hidden until its hook on the title screen's buttons is in place, so if the game's code doesn't look as expected they never show and `platform.log` says why. [`game-notes.md`](game-notes.md#menus) has how the title screen works.

## Online play (API 9)

```c
int playing_online(void);
/* and in FH2ModInfo: */
int offline_only;
```

[FH2MP](https://github.com/Charelene/FH2MP) brings back online play, and runs on the platform itself (its `fh2-mods\FH2MP.dll`). Some mods shouldn't run in online play, because they change how cars drive or what other players see: a nitro boost, say. From platform 2.8.0 such a mod isn't loaded when the game is started from the FH2MP launcher.

- **Online is decided before any mod loads, and on the safe side.** It's online when FH2MP is in `fh2-mods` and turned on, and has a session to play with: the `fh2mp-session.txt` its launcher writes before starting the game and removes when it ends (in `data\bin`, or next to `azimuth.exe` in a folder without `data\bin`), or `FH2MP_TOKEN` in the environment. The platform only checks that the file is there, and never reads it, as it holds the player's token. A session file counts even when FH2MP will find it too old and stay offline, and one the platform can't check counts as there. FH2MP is `FH2MP.dll`, or a DLL under another name that registered as FH2MP at an earlier launch. Only FH2MP itself (`FH2MP.dll`, else the DLL FH2MP registers from) always loads online and has no **Load when playing online** switch; another mod that registers as FH2MP doesn't get that. `playing_online` is never true without FH2MP. The log says which DLL and which session, then "started from the FH2MP launcher (online play)".
- **Online found later (platform 2.9.0).** Mods load before FH2MP connects and can't be unloaded, so the platform looks again when a mod registers as FH2MP (a renamed DLL it didn't know), when FH2MP's `fh2mod_init` returns (FH2MP reads its session there, and the launcher may have written it after the loader looked), and when FH2MP calls `going_online` (below). If FH2MP is online after all, the game counts as online from then on: `playing_online` returns 1, and the same rule as at load decides which mods run (the player's **Load when playing online** switch, else `offline_only`). Mods after FH2MP in load order that shouldn't run online aren't started, and `register_mod` turns them away. The ones that had already started are **stopped** until the game restarts:
  - their hooks and byte patches go off, and `enable_hooks` is refused;
  - their game text (`set_string`, `replace_string`), their SQL and their `set_file` files are taken out, and new ones are refused. Once the mods have started, the string tables and the database are written again without them;
  - their hotkeys, commands, pad layers and title-screen entries do nothing, and their HUD layer can only be hidden, so it lapses.

  What isn't stopped: the mod's own threads and anything it does from them, its rows' and pages' callbacks on the MODS tab (the player opens those), its HUD layer's `on_status`, and interfaces it published, which other mods can still call. The log says "online play after all" and names the stopped mods, and the MODS tab shows them as "Off online", stopped for online play. A mod from before the platform can't be stopped: the log says so. This is [issue #22](https://github.com/Trinculo54/fh2-mod-platform/issues/22).
- **`offline_only`**: set it to 1 in your `FH2ModInfo` if your mod shouldn't run online. Online, `register_mod` then returns `NULL`, so call `register_mod` first in `fh2mod_init` and return straight away when it gives you `NULL` (as every mod should anyway); what you return then isn't reported as a problem. The platform remembers it, and from the next online launch doesn't load your DLL at all. Offline, nothing changes. Older platforms ignore the member, so set it whatever the version.
- **The player decides in the end.** Where FH2MP is installed, each mod's page under MODS > YOUR MODS has a **Load when playing online** switch, set from your `offline_only` until the player changes it. Their choice is kept in `fh2-mods\platform-online.txt` and takes effect the next time the game starts from the launcher. It works for any DLL in `fh2-mods`, platform mods or not, so players can also turn off a mod whose author hasn't set the flag.
- **`playing_online`** returns 1 when the game is online in that sense, 0 otherwise. Call it from anywhere, `fh2mod_init` included. From platform 2.9.0 it can turn from 0 to 1 during the session (online found later, above), never back, so check it when it matters, not once at the start. Use it to turn off just the part of your mod that shouldn't run online and keep the rest; anything your mod does from its own threads rather than through hooks is yours to stop when it turns 1. Check `api_version >= 9` first.
- **What it can't switch off**: files you copied into `mediaoverride` yourself (your mod's zip, say). The game reads those online too. Files you put there with `set_file` are your mod's, and go with it.

```cpp
FH2ModInfo info{sizeof info, "Nitro", "1.2.0", "Me/Nitro", FH2MOD_PLATFORM_VERSION, 1};
info.description  = "Fast & Furious style nitro.";
info.offline_only = 1;   // the boost would be a cheat online
g_mod = api->register_mod(&info);
if (!g_mod) return 0;    // online (or a bad info): nothing to do
```

### For FH2MP: `FH2Platform.Online` (platform 2.9.0)

```c
const FH2PlatformOnline* o = (const FH2PlatformOnline*)api->get_interface(FH2_ONLINE_INTERFACE, 1);
/* o->playing_online(): as FH2ModApi.playing_online
   o->going_online(names, size): call before connecting */
```

The platform publishes this interface itself, before any mod starts, so FH2MP finds it from `fh2mod_init` on; older platforms return `NULL`. FH2MP calls `going_online` before it connects. The game then counts as online from that moment (if it didn't already), with the mods that shouldn't run online stopped as above. It returns how many such mods had already started in this game, and puts their names in `names` ("Nitro, Other", UTF-8, cut short between characters to fit `size`, NUL-terminated); `names` may be `NULL` (or `size` 0) to get only the count. 0 means none did: connect. Over 0, those mods ran offline first, and may have changed the game in ways the platform can't undo (memory they wrote from their own threads, say), so FH2MP should stay offline and ask the player to start the game from the FH2MP launcher. Call it from any thread, as often as you like.

## Sessions (API 9)

```c
const char* session_id(void);
const wchar_t* session_log_path(FH2Mod* mod, const wchar_t* folder, uint32_t keep);
```

Each run of the game is a session, with an id like `20261021-203224-1234`: the time the platform started, and the process id. The game can start twice in one launch, and the MODS page's Restart now starts a new process, so "the newest log" isn't always the one you want; the id says which run a file belongs to.

- **`platform.log`** is still rewritten every time the game starts, and its third line names the session ("session 20261021-203224-1234: started 2026-10-21 20:32:24, process 1234; ..."). The platform also keeps a copy of each session's log in `fh2-mods\platform-logs\platform-<id>.log`, the newest 20.
- **`session_id`** returns the id. Call it from anywhere, `fh2mod_init` included.
- **`session_log_path`** makes `<azimuth_dir>\<folder>\logs\` and returns `<folder>\logs\<id>.log` in it, for your own log: give your mod's own folder next to `azimuth.exe` (`L"MyMod"`), and how many logs to keep there, counting this one. Older `*.log` files in that `logs` folder are deleted. Open the file yourself (the platform doesn't create it); the path stays valid for the session. It returns `NULL`, and the log says why, if `folder` isn't a plain folder name or can't be made.
- **`fh2-mods\platform-session.json`** says what's running, for tools such as a test runner or a log collector. It's written to a `.tmp` and moved over, so a reader never sees half of it, once the mods have started and again when something in it changes:

  ```json
  {
    "session": "20261021-203224-1234",
    "pid": 1234,
    "started": "2026-10-21 20:32:24",
    "platform": "2.8.0",
    "azimuth": "D:\\Games\\Azimuth",
    "mods": {
      "WreckingBall": "1.3.2"
    },
    "pipe": "\\\\.\\pipe\\fh2mod-1234",
    "state": "running"
  }
  ```

  `mods` has every mod that registered. `pipe` is the command pipe (see Commands), or `""` while it isn't up. `state` is `running`, or `crashed` after a crash, with `"crash_report": "crashes\\crash-....txt"` (relative to `fh2-mods`). Azimuth ends the game with `TerminateProcess`, so nothing can write `ended`: a tool should take a `running` whose `pid` isn't alive any more (or whose process started at another time than `started`) as a session that's over.

## Save snapshots (API 9)

```c
int request_save_snapshot(FH2Mod* mod, const char* reason, uint32_t keep);
```

Before an experimental session, the safe thing is a copy of the saves. Ask the platform for one from `fh2mod_init` (a mod being tested, or one that changes what gets saved), and once every mod's `fh2mod_init` has returned it copies `<azimuth_dir>\data\titles\1A96545B\saves` to `fh2-mods\save-snapshots\<session id>\`: once, however many mods ask, keeping the newest `keep` copies (the most any mod asked for). `platform.log` says how much it copied and for which mods and reasons. The game only reads the saves when A is pressed on the title screen, long after.

The platform only ever reads the saves. To go back to a snapshot, close the game and copy the whole snapshot over the saves folder: Azimuth checks the saves' hashes, so don't mix files from different copies. It returns 1 if a copy will be made, 0 if there's no saves folder or the call came after `fh2mod_init` (the log says which).

## Hotkeys (API 9)

```c
int add_hotkey(FH2Mod* mod, const FH2Hotkey* hotkey);

typedef struct FH2Hotkey {
    uint32_t size;                 /* sizeof(FH2Hotkey) */
    const char* key;               /* unique within your mod: "Mark" */
    const char* label;             /* "Mark this spot", for its rows on your mod's page */
    uint32_t vk;                   /* a Windows virtual-key code (VK_F9, 'M'); 0 = none */
    uint32_t modifiers;            /* FH2_KEY_CTRL | FH2_KEY_SHIFT | FH2_KEY_ALT */
    uint32_t pad_chord;            /* FH2_PAD_LB | FH2_PAD_RB | FH2_PAD_Y ...; 0 = none */
    FH2HotkeyFn on_press;          /* void (*)(void* user, const char* key) */
    FH2HotkeyRebindFn on_rebind;   /* void (*)(void* user, const char* key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord) */
    void* user;
} FH2Hotkey;
```

A key on the keyboard, a chord on the controller or both, that call `on_press` once per press: a key that writes "the problem is here" to your log, or a live switch while you test something.

- **Only while the game is in front.** The platform polls the keyboard (`GetAsyncKeyState`) and the controllers (XInput, from System32) every 15 ms while Azimuth's window has the focus. It never takes a key from other programs (no `RegisterHotKey`), and the game still gets every key and button, so pick ones that do nothing harmful in game: an F-key, or LB+RB with a face button. The modifiers have to match exactly, so Ctrl+F5 doesn't set off a hotkey on F5.
- **`on_press`** runs on the platform's hotkey thread, not the game's. Keep it short, and hand anything that touches the game to a hook. If it crashes, the log says so and the next press calls it again.
- **Your mod's page.** Each hotkey gets two choice rows on your mod's page under Pause > MODS > YOUR MODS, under a "Keys and buttons" heading after your own rows: "<label>: key" (Off, F1 to F12, Ctrl+F1 to F12, or your own binding if it's none of those) and "<label>: controller" (Off, or LB+RB with Y, X, B, A or a d-pad direction, or your own chord). A change there takes effect straight away and calls `on_rebind` with the new binding. The platform doesn't keep bindings: save them in your settings and pass them in next time, as with your rows.
- **When.** Add hotkeys from `fh2mod_init`, so their rows show (rows added later only show from the next launch). It returns 1 if the hotkey was added, 0 if not, and the log says why.

```cpp
FH2Hotkey mark{sizeof mark};
mark.key       = "Mark";
mark.label     = "Mark this spot";
mark.vk        = saved_vk ? saved_vk : VK_F9;
mark.pad_chord = FH2_PAD_LB | FH2_PAD_RB | FH2_PAD_Y;
mark.on_press  = [](void*, const char*) { g_mark_now = true; };   // the frame hook writes the position
mark.on_rebind = [](void*, const char*, uint32_t vk, uint32_t mods, uint32_t chord) { save_binding(vk, mods, chord); };
api->add_hotkey(g_mod, &mark);
```

**Using buttons while driving.** A hotkey never stops the game seeing its buttons. To have the d-pad (or other buttons) to yourself while a modifier is held, see the pad layer preview, [pad-layer-preview.md](pad-layer-preview.md).

## Commands (API 9)

```c
int add_command(FH2Mod* mod, const char* name, const char* help, FH2CommandFn fn, void* user);
typedef void (*FH2CommandFn)(void* user, const char* name, const char* args, char* reply, size_t reply_size);
```

A command is something another program can ask your mod to do while the game runs: flip a setting for an A/B test without restarting, print some numbers, mark a spot. Run them with `fh2mod.exe`, in the `tools` folder of the platform's zip:

```
fh2mod list
fh2mod MyMod shadows off
fh2mod --pid 1234 MyMod stats
```

- **`add_command`** registers `name` (letters, digits and `_`, unique within your mod, matched in any case) with a line of `help` for `fh2mod list` (or `NULL`). Add commands whenever you like; it returns 1 if added, 0 if not, and the log says why.
- **`fn`** runs on the platform's command thread, not the game's, one command at a time, with the rest of the line in `args`. Write your answer into `reply` (UTF-8, `reply_size` bytes with the NUL: 16 KB). Start it with `error: ` for a failure: `fh2mod.exe` prints it and exits with 1. Hand anything that touches the game to a hook or your own thread. If `fn` crashes, the request gets an error and the log says so.
- **The pipe.** It starts when the first command is added: `\\.\pipe\fh2mod-<process id>`, also in `fh2-mods\platform-session.json`. It refuses remote clients, and its DACL lets in only the Windows user the game runs as, so only that user's programs on that PC can connect. A request is one line of UTF-8: `list`, or `<mod> <command> [args]`, with a mod name that has spaces in double quotes. The reply is text, and the connection then closes. Any tool can use it the way `fh2mod.exe` does (`tools/fh2mod.cpp`).
- **Which game.** Without `--pid`, `fh2mod.exe` finds the one running game with a command pipe; when there are more (the game started twice, or two installs), it lists their process ids for `--pid`.
- **On Linux** the pipe is inside the game's Wine prefix, so run `fh2mod.exe` with the same prefix (`wine`, or `protontricks -c`).
- **The platform's own command** (platform 2.9.0), `fh2mod Platform ui`, says what pop-ups and tips are on screen and waiting, and why ([Trying templates](#trying-templates-fh2mod-platform-ui-and-the-test-page)). It's listed with the others, but adding it never starts the pipe: it works once a mod's command has, or with `Developer=1` in `fh2-mods\platform-popups.ini`.
- **Arguments** reach your command as `fh2mod.exe` got them, joined with single spaces; a shell takes quotes away first, so for text with spaces in it, take the rest of the line, or a separator such as `|`.

```cpp
api->add_command(g_mod, "debris", "debris clear: removes every broken piece now",
                 [](void*, const char*, const char* args, char* reply, size_t size) {
                     if (strcmp(args, "clear") != 0) { snprintf(reply, size, "error: debris clear"); return; }
                     g_clear_debris = true;   // the game's update hook does it
                     snprintf(reply, size, "clearing");
                 }, nullptr);
```

## Text in the game's language (API 10)

```c
int game_language(char* code, size_t size);
int set_setting_text(FH2Mod* mod, const char* key, const char* label, const char* description,
                     const char* const* choices, uint32_t choice_count);
void set_mod_description(FH2Mod* mod, const char* description);
```

The game picks its text language a few seconds after it starts, from its own language option, well after `fh2mod_init`. Rows added there can only be in a language you guess, and when the player has just changed the game's language the guess is wrong: Japanese, Korean or Chinese text in another language's font even shows as boxes. These let your text follow the game:

- **`game_language`** puts the game's text language in `code`: the code its string tables are named by, in capitals (`EN`, `GB`, `DE`, `FR`, `ES`, `MX`, `IT`, `BR`, `NL`, `PL`, `RU`, `JP`, `KO`, `CHT`, `CZ`, `HU`; the game's Czech and Hungarian text is English). It returns 1, or 0 before the game has a language, or with no game. Cheap enough to ask every frame.
- **`set_setting_text`** changes a row's label, description and, for a choice row, its choices' names. `NULL` leaves that part as it was; `choices` must have as many names as the row has. Link rows take a label and a description. It returns 1, or 0 when you have no row called `key` or the choices don't match, and the log says why.
- **`set_mod_description`** changes your `FH2ModInfo.description`, shown on the YOUR MODS list and in "What it does" on your mod's page.

YOUR MODS (with every mod's page), the MODS tab's other pages and the list that stands in for them are written each time they're opened, so new text shows the next time the player opens them. Call these from any thread, as often as you like: from your own thread once `game_language` answers, say.

```cpp
// in your own thread
char lang[8];
while (!api->game_language(lang, sizeof lang)) Sleep(500);
api->set_setting_text(g_mod, "Trees", my_text(lang, "Breakable trees"), my_text(lang, "Trees break when you hit them."), nullptr, 0);
api->set_mod_description(g_mod, my_text(lang, "Breakable trees and bushes."));
```

In Japanese, Korean and Chinese the game's fonts only have the characters its own text uses, so check your translations against them (`media\ui\Fonts.zip`; its `fontsettings.xml` says which font each language uses): a character the font lacks shows as a box.

For most mods, [translations](#translations-api-10) do all of this for you: text from `tr()` follows the game's language without these calls.

## Translations (API 10)

```c
const char* language(void);
int add_translation(FH2Mod* mod, const char* lang, const char* po_utf8, size_t size);
const char* tr(FH2Mod* mod, const char* key, const char* english);
const char* tr_plural(FH2Mod* mod, const char* key, const char* one, const char* other, int64_t n);
void set_string_tr(FH2Mod* mod, const char* table, uint16_t key, const char* msg_key, const wchar_t* english);
int format_number(double v, int decimals, char* out, size_t n);
int format_date(int year, int month, int day, char* out, size_t n);
```

Your text in the player's language. You write it in English with a key for each text, translators turn the list of them into a gettext PO file per language, and you build those files into your DLL. [`translating.md`](translating.md) has the whole workflow, for you and for your translators; [`include/fh2mod_l10n.h`](../include/fh2mod_l10n.h) has helpers that also work on older platforms (they give the English there), and `tools/l10n` the scripts that make the files. Anything without a translation shows in English, so you can add translations a language at a time.

```cpp
#include "fh2mod_l10n.h"
#include "mymod_l10n.inc"    // tools/l10n/po2inc.py: l10n/pl.po, l10n/pt_BR.po ...

g_mod = api->register_mod(&info);
FH2_ADD_TRANSLATIONS(api, g_mod, mymod_translations);

FH2Setting s{sizeof s};
s.key = "Trees";
// L10N: the row on the mod's page
s.label = FH2_TR(api, g_mod, "row.Trees.label", "Breakable trees");
s.description = FH2_TR(api, g_mod, "row.Trees.description", "Trees break when you hit them.");
api->add_setting(g_mod, &s);
```

**Following the game's language.** The game picks its language a few seconds after `fh2mod_init`, so text you look up there can be in the wrong one. You don't have to wait for it: every text `tr()` and `tr_plural()` give you is the platform's own copy, and the platform remembers which key and English each one came from. Where you pass one on as it is, the platform looks it up again in the game's language every time it writes the page: a row's label, description and choices (`add_setting`, `set_setting_text`), a page's title, label, description and rows (`add_page`, `set_page_rows`), a title-screen entry's label and its list (`add_title_entry`), a hotkey's label (`add_hotkey`), your title-screen notice (`set_notice`, which gets each language's own words in each language's `Main.str`) and `set_mod_description`. The title screen's entry lines are written once, as the game starts, so they're in the language the game had last time; they follow it from the next launch. Text you make from `tr()`'s, with a number filled in, say, is yours, and stays as you made it: set it again when the page opens (`FH2Page.on_open`). Text you pass that isn't `tr()`'s stays as you gave it, as before.

- **`language`** says which language text is looked up in, as a BCP-47 tag: `en`, `en-GB`, `pl`, `pt-BR`, `de`, `fr`, `es-ES`, `es-MX`, `it`, `ja`, `ko`, `zh-Hant`, `cs`, `hu`, `ru` or `nl`, for the game's EN, GB, PL, BR, DE, FR, ES, MX, IT, JP, KO, CHT, CZ, HU, RU and NL. It's the game's language once the game has one. Before that it's the one the game had last time, which the platform keeps in `fh2-mods\platform-language.ini`. The first time, it's the one the game will pick for Windows' regional format (Settings > Time & language > Region), from the game's own table: Polish for `pl-PL`, Brazilian Portuguese for any Portuguese, Spain's Spanish for `es-ES`, Mexican Spanish for other Spanish, UK English for most English but `en-US`, `en-AU` and `en-CA`, and English for languages the game has no table for there (Korean, Czech, Hungarian, Dutch and the rest). `qps` is the test language (below). Call it from any thread, `fh2mod_init` included.
- **`add_translation`** takes one language's PO file, as UTF-8 text in memory (`size` bytes; the platform copies what it needs). `lang` is the language's tag (`pl`, `pt-BR`; `pt_BR` reads the same). Call it once per language, straight after `register_mod`: `FH2_ADD_TRANSLATIONS` does that for the array `po2inc.py` makes. It reads `msgctxt`, `msgid`, `msgid_plural`, `msgstr` and `msgstr[n]`, text over several lines and C escapes (`\n`, `\"`, `\t`...). It skips entries marked `#, fuzzy` (not checked yet) and untranslated ones, and leaves out an entry whose `{placeholders}` don't match its English, with a line in the log. A plural form that `lang` uses for 1 alone (Polish's first, but not Russian's, which is also for 21 and 31) may leave the number out: "Jedno drzewo" for "{n} tree broken". It returns how many entries it took, or 0 (the log says why).
  If `fh2-mods\lang\<your mod's name>\<lang>.po` exists, that file is used instead of what you pass, and the log says so. Translators can try their work in game that way, without a build; a file for a language your DLL hasn't got is read too.
- **`tr`** gives `english` in the language now, or `english` itself when there's no translation. `key` is the PO file's `msgctxt`, a name of your choosing that stays the same when the English changes: `"row.Trees.label"`. `NULL` looks for an entry without one, by its English alone. A translation made from other English than you pass isn't used, so a text you've reworded shows in English until it's translated again rather than with its old meaning (the log says so once). It looks in the language's own translation, then its parent: `pt-BR` then `pt`, `en-GB` then `en`, `es-MX` then `es-ES` then `es`; English comes last. The text stays valid for the session; don't free it, and don't change it. Pass the same English each time: the platform keeps every different text it hands out for the session.
- **`tr_plural`** is `tr` for text with a count. `one` and `other` are the English forms (the PO file's `msgid` and `msgid_plural`), and the form for `n` is picked by the language's own rules, as in Unicode's CLDR: English and most languages have one form for 1 and another for the rest; Polish has three (1; 2-4, 22-24...; the rest), as do Russian and Czech; Portuguese and French use the singular for 0 and 1; Japanese, Korean and Chinese have one form. It gives you the form with its `{n}` still in it, so fill that in: `fh2_fill` and `fh2_number` in `fh2mod_l10n.h` do it with the number in the language's own style.
  ```cpp
  // L10N: under the Achievements page's title
  const char* t = FH2_TR_PLURAL(api, g_mod, "count.unlocked", "{n} achievement unlocked", "{n} achievements unlocked", n);
  const std::string unlocked = fh2_fill(t, {{"n", fh2_number(api, n)}});   // "12 345 ..." in Polish (and "1234": four digits stay together)
  row.value = unlocked.c_str();   // copied by set_page_rows
  ```
- **`set_string_tr`** is `set_string` with a key: each language's string table gets the text for its own language, so the game shows the right one whatever language it picks, with nothing to follow. `msg_key` and `english` are looked up as `tr()` looks them up (`english` is UTF-16, as `set_string`'s text is). Make it from `fh2mod_init`, as `set_string`: a later one shows from the next launch. `set_string` and `set_string_tr` on the same key replace each other.
  ```cpp
  api->set_string_tr(g_mod, "LoadingMessages.str", 0x6875, "loading.tip", L"Trees break when you hit them.");
  ```
- **`format_number`** writes `v` with `decimals` digits after the point (0 to 6) as the language writes numbers: `1,234.5` in English (and Japanese, Korean, Chinese and Mexican Spanish), `1 234,5` in Russian, Czech, Hungarian and French (with a no-break space, U+00A0, which the game's fonts have), and `1.234,5` in Brazilian Portuguese, German, Italian and Dutch. Polish and Spain's Spanish, as CLDR has them, group only from five digits: `1234,5` but `12 345,5` in Polish and `12.345,5` in Spanish. **`format_date`** writes a date: `24.10.2026` in Polish, German, Russian and Czech; `24/10/2026` in Portuguese, French, Spanish and Italian; `24-10-2026` in Dutch; `2026/10/24` in Japanese and Chinese; `2026. 10. 24.` in Korean and Hungarian; and `2026-10-24` in English, as the platform writes dates. Both write UTF-8 into `out` and return its length, or 0 (with `""` in `out`) when `n` bytes don't hold it, or the number isn't finite or the date isn't one.
- **`FH2ModInfo.description`** reaches `register_mod` before your translations do, so the platform looks it up itself, under the key `mod.description`, each time it shows it. Mark it `FH2_TR_DESCRIPTION("...")` so `extract.py` lists it.

**The test language.** With `test=1` in `fh2-mods\platform-language.ini`, every text that goes through `tr()`, `tr_plural()` or `set_string_tr` shows in the test language, `qps`: the English with its letters swapped for Polish and Portuguese ones and padded by how much translations tend to grow (twice the length up to 10 characters, 1.7 times to 20, 1.5 times to 50, 1.35 times beyond), in `[brackets]`. `{placeholders}` stay as they are. Text that's cut off, text that never goes through a translation and capitals that don't work beyond A to Z are then easy to see, before anyone translates anything. Numbers and dates come out as in Polish. There's no row for it on purpose: put `test=0` back, or delete the file, when you're done.

```ini
; fh2-mods\platform-language.ini (the platform writes "last" itself)
last=pl
test=1
```

## UI templates (API 10)

```c
uint32_t ui_status(FH2Mod* mod);
int ui_uses(FH2Mod* mod, uint32_t uses);
int play_ui_sound(FH2Mod* mod, uint32_t sound);
int show_toast(FH2Mod* mod, const FH2Toast* toast);
void clear_toast(FH2Mod* mod, const char* key);
int show_tip(FH2Mod* mod, const FH2Tip* tip);
int tip_seen(FH2Mod* mod, const char* key);
void set_tip_seen(FH2Mod* mod, const char* key, int seen);
int show_message_box(FH2Mod* mod, const FH2MessageBox* box);
int set_page_help(FH2Mod* mod, const char* page_key, const FH2Prompt* prompts, uint32_t count);
int set_page_hints(FH2Mod* mod, const char* page_key, const char* const* texts, uint32_t count);
int show_page_hint(FH2Mod* mod, const char* page_key, int index, uint32_t kind);
```

Ready-made pieces of the game's own UI that you fill with text: pop-ups on the game's own HUD notification, tips that show once, the game's message box, UI sounds, and the help bar and hint line of your pages. The platform draws them in the game's look, decides when they show, and never takes XAML from you, so nothing you pass can break the game's screens.

**Which one for what:**

| You want to | Use |
|---|---|
| Tell the player something happened (an unlock, a file written) | `show_toast` (or `fh2ui_notify`) |
| Answer the player's own key press | `show_toast` with `FH2_TOAST_FLAG_REPLY`; `play_ui_sound` if it returns 0 |
| Report a fault the player must act on | `show_toast` with `FH2_TOAST_FLAG_URGENT`, saying what to do |
| Teach a control once | `show_tip` |
| Ask for a confirmation or a choice | `show_message_box`, from your page |
| Say what the buttons do on your page | `set_page_help` |
| Show a short status on your page ("Saved.") | `set_page_hints` and `show_page_hint` |
| Say something before the game starts | `set_notice` and `add_title_entry` (API 9) |

**Rules for all of them:**

- **Threads.** Every call is quick and can be made from any thread, `fh2mod_init` included, unless it says otherwise. Each copies every string and struct you pass before it returns, so `tr()` results and buffers on your stack are fine; only callbacks and their `user` pointers are kept.
- **Text** is UTF-8. Pass `tr()` and `tr_plural()` results ([Translations](#translations-api-10)), in sentence case: titles are shown in capitals that the platform makes in the player's language ("Osiągnięcia" becomes "OSIĄGNIĘCIA"). Limits are in characters, not bytes, and longer text is cut with an ellipsis. Where a button goes inside a sentence, write `{button}` in the text, so each language can put it where its word order needs it.
- **When they show.** Pop-ups and tips wait while the player is in the pause menu, photo mode, a replay, loading or a cutscene, and show once the driving view is back. Time spent waiting there doesn't count toward a pop-up's `expire_s`.
- **Everything shown is logged**, its text included, because testers attach `platform.log` to bug reports. Flag text that can name people (a gamertag, an account name) `FH2_TOAST_FLAG_PRIVATE`.
- **Faults.** When the platform catches a crash in one of your callbacks, it drops your waiting pop-ups and tip. A message box of yours that's open stays until the player answers it, and your `on_choose` isn't called.
- **Your callbacks** are where the page templates work best. The platform knows when it's calling one of your page, setting, title-screen or message box callbacks (on the game's UI thread): a message box shows only from there, a sound from there plays at once, and a hint changed from one of the page's own shows at once.
- **Nothing works by itself on an older platform**: use [`include/fh2mod_ui.h`](../include/fh2mod_ui.h) (below), and it does nothing there instead of crashing.
- **Don't hook the game's notification code yourself** (`Show`, `MakeDisplayString`, the notification component's constructor and destructor, the achievements tick): the platform chains them with the mods that already do, Achievements 1.0.x checks the first 15 bytes of `Show` and of `MakeDisplayString` in memory, and FH2MP 1.2.0 the first 16 of `MakeDisplayString` ([game-notes.md](game-notes.md#code-released-mods-check-in-memory)).

### ui_status and ui_uses

- **`ui_status`** says what works now, as `FH2_UI_` bits. It's cheap, so ask it whenever it matters:

  | Bit | Means |
  |---|---|
  | `FH2_UI_TOASTS` | Pop-ups and tips can show this session: the game's notification code is as expected, and the platform's hooks for them are in. The hooks go in only in a session where a mod declared `FH2_UI_USES_TOASTS` or `FH2_UI_USES_TIPS` in `fh2mod_init` (see `ui_uses`), or where `Developer=1` is set in `fh2-mods\platform-popups.ini`; otherwise this is never set. Even then it isn't set until the game's code is ready, a few seconds after start-up: pop-ups and tips sent before then wait, as they do for the first drive. Without the platform's watch on the game's own notifications (the log says so), they still show, spaced 5.5 seconds apart. |
  | `FH2_UI_TOASTS_OFF` | The player turned mod pop-ups off: the platform's "Mod pop-ups" row, your mod's "Pop-ups" row, or the game's Notifications setting (Pause > Settings > HUD, the "Minor notifications" option inside the game), which the platform follows. |
  | `FH2_UI_TIPS_OFF` | The player turned tips off. |
  | `FH2_UI_ON_SCREEN` | The free-roam HUD page is on screen now. |
  | `FH2_UI_GAME_HUD_HIDDEN` | The game's HUD is suspended: by the game (a car meet, a Rivals event), or held hidden by a mod's HUD layer (`hold_game_hud`, [hud-layer-preview.md](hud-layer-preview.md)). Pop-ups and tips wait meanwhile, since the game's notification is part of that HUD. |
  | `FH2_UI_KEYBOARD` | The keyboard or mouse was used last, so buttons are written as keys. |

- **`ui_uses`** says what you'll use, as `FH2_UI_USES_` bits, from `fh2mod_init` only (later, or from a thread of your own, it returns 0 and the log says so). Several calls add up. It returns 1.
  - `FH2_UI_USES_TOASTS`: you show pop-ups. The platform adds a "Pop-ups" row (On/Off) to your mod's page, so a player can quieten your mod without turning every mod's pop-ups off.
  - `FH2_UI_USES_OWN_SWITCH`: you have a pop-ups row of your own, so the platform adds none.
  - `FH2_UI_USES_TIPS`: you show tips. The platform then watches the controller as well as the keyboard, to know which the player used last.

  Declare them: the platform hooks the game's notification only in a session where a mod declared pop-ups or tips, as soon as the game's code is ready, since it has to see the notification being made long before the first pop-up. Its sites are shared with other mods' hooks (Achievements, CommunityGarage), so a session without such a mod leaves them alone. In that session `show_toast` and `show_tip` return 0 and the log says why. When another mod declared them, an undeclared mod's pop-ups still show, but it gets no row, and the log asks it to declare them.

### play_ui_sound

Plays one of the game's own UI sounds, wherever the player is. You don't need a HUD layer for it. Never make a sound the only cue: some players have the sound off.

| Sound | What it is | Where it plays |
|---|---|---|
| `FH2_SOUND_SELECT` | the menus' select | everywhere |
| `FH2_SOUND_BACK` | the menus' back | everywhere |
| `FH2_SOUND_OPEN` | a menu opening | everywhere |
| `FH2_SOUND_CLOSE` | a menu closing | everywhere |
| `FH2_SOUND_ERROR` | the menus' "can't do that" | everywhere; only a low thump over the driving view |
| `FH2_SOUND_NOTIFY` | the game's notification chime, for a milestone | over the driving view only |
| `FH2_SOUND_INFO` | a pop-up opening | everywhere; very faint over the driving view |
| `FH2_SOUND_TICK`, `_TICK_LOW`, `_TICK_END` | the race countdown's beeps | over the driving view only |
| `FH2_SOUND_SHUTTER` | the photo shutter | everywhere |

All eleven were heard in game, through the same player the game itself uses for them (the HUD layer preview's tests). The same numbers are the HUD layer preview's sounds ([hud-layer-preview.md](hud-layer-preview.md)).

- **When.** From one of your page, setting, title-screen or message box callbacks it plays at once. From anywhere else (a hotkey, a command, a thread of your own) it plays at the game's next tick, on the game's thread: the game's sound player isn't safe to call from other threads. Four can wait for your mod at once (a fifth drops your oldest), one plays a tick, and one that waited half a second is dropped. The first time a mod plays a sound from outside its callbacks, the platform hooks the game's tick for it, so it may take a moment the first time.
- **The world-only four.** NOTIFY and the three countdown beeps are silent under the game's menus, so they're refused from your callbacks (a menu or the title screen is up) and while the driving view is known to be off screen. For an answer the player can hear to a hotkey press while driving, use SHUTTER, SELECT or BACK.
- **The same sound** plays at most once per 100 ms from your mod.
- **Returns** 1 if it played or is on its way, 0 if not: an unknown sound, a repeat, a world-only sound where it can't be heard, before the game's sound is up, or the game's sound code not as expected this session. Each reason is said once in the log, and each sound's first play too ("UI sound SELECT played").

### show_toast and clear_toast

```c
typedef struct FH2Toast {
    uint32_t size;                 /* sizeof(FH2Toast) */
    const char* key;               /* letters, digits and '_': replaces your waiting pop-up with the same key; NULL = never */
    const char* title;             /* UTF-8, shown in capitals; 28 characters */
    const char* subtitle;          /* UTF-8, as given; 45 characters; may be NULL */
    uint32_t icon;                 /* FH2_ICON_: one of the game's nine pictures, or none */
    uint32_t sound;                /* FH2_SOUND_ when it appears; 0 = none, as the game's own pop-ups */
    uint32_t flags;                /* FH2_TOAST_FLAG_ */
    uint32_t expire_s;             /* dropped once it could have shown for this long and didn't; 0 = 120; 1 to 600 */
} FH2Toast;
```

A pop-up on the game's own HUD notification: one of its pictures, a title and a line, at the bottom centre of the driving view for about 5 seconds, exactly as the game shows a discovered road or an unlocked achievement.

- **One queue for every mod**, spaced around the game's own notifications: one starts only once nothing has shown for 5 seconds, whether it was the game's (a road discovered, a barn find), another mod's or yours. One with the same `key` as one of yours that is waiting replaces it where it stands (a second F8 press replaces the first's pop-up); one already on screen runs its 5 seconds, and the same one again isn't queued. Up to `FH2_TOAST_MAX_WAITING` (4) of yours wait at once, and a 5th drops your oldest waiting one that isn't urgent.
- **The player decides** whether mod pop-ups show: the game's Notifications setting (Pause > Settings > HUD), the platform's "Mod pop-ups" row and your mod's "Pop-ups" row (`FH2_UI_TOASTS_OFF`). Turning either row Off also drops the pop-ups of yours it covers that are waiting, each with a log line. With the game's setting off, the game draws no notification at all, so `show_toast` returns 0 and the ones waiting are dropped.
- **Waiting.** A pop-up waits while the player is in a menu, photo mode, a replay, loading or a cutscene, and while the game's HUD is suspended: by the game (a car meet, a Rivals event), or held hidden by a mod's HUD layer (`hold_game_hud`), since the notification is part of the HUD it hides. `expire_s` counts only the time it could have shown, waiting behind other pop-ups included, so one sent from `fh2mod_init` waits for the first drive.
- **Flags.** `FH2_TOAST_FLAG_URGENT`: a fault the player must know about and act on; it goes first in the queue, twice a session per mod (later ones are treated as normal), and waits up to 10 minutes of time it could have shown. `FH2_TOAST_FLAG_REPLY`: it answers the player's own press, honoured when it comes within 1 s of the platform calling your hotkey, command, page, setting or title-screen callback (otherwise it's a normal pop-up, and the log says so once), goes before the normal ones, and is dropped if it couldn't show within 6 s of such time: a late answer confuses more than it helps. `FH2_TOAST_FLAG_PRIVATE`: the text may name people, so the log gets your mod's name and the key only, and "Recent pop-ups" shows "(private)".
- **Text.** The title is shown in capitals made in the player's language, cut at 28 characters; the line as given, cut at 45. Until the platform's wide text has been seen working in game, text beyond ASCII reaches the game's notification with plain letters ("ł" as "l", "ß" as "ss", "…" as "..."), and letters with no plain form (Cyrillic, Japanese, Korean, Chinese) as "?": the game's own way in for text takes the Windows code page. `fh2-mods\platform-popups.ini` with `[popups]` and `WideText=1` turns the wide text on, for testing it.
- **Sounds.** None by default: the game's notification chimes by itself, so `FH2_SOUND_NOTIFY` is never played with one. Any other sound plays as the pop-up appears.
- **Icons** are the game's own nine pictures, which always show: `FH2_ICON_TROPHY` (an achievement), `_ROADS` (a road discovered), `_PHOTO`, `_BOARDS` (a bonus board), `_BUCKET` (a bucket list challenge), `_CARMEET`, `_CLUB` (a club payout), `_PAYOUT` (a Drivatar payout) and `_BARNFIND`; or `FH2_ICON_NONE`.
- **Returns** 1 if queued (it can still expire or be dropped), 0 if not: pop-ups off, no title, a bad key; the log says which. Lines about replacing, dropping and queuing are written at most once per 10 s for each key, with a count of the rest.
- **`clear_toast`** takes down your waiting pop-up `key`, or all your waiting ones for `NULL`. One already on screen runs its 5 seconds.

### show_tip, tip_seen and set_tip_seen

```c
typedef struct FH2Tip {
    uint32_t size;                 /* sizeof(FH2Tip) */
    const char* key;               /* the seen key: once seen, show_tip with it returns 0; NULL = no seen state */
    const char* text;              /* UTF-8, with "{button}" where the button goes; 45 characters in all */
    FH2Prompt button;              /* glyph 0, vk 0 and no hotkey = no button */
    uint32_t delay_s;              /* seconds of driving, with nothing else showing, before it shows; 0 = 60 */
    uint32_t max_sessions;         /* FH2_TIP_FLAG_UNTIL_DONE only: shown in at most this many sessions; 0 = 1 */
    uint32_t flags;                /* FH2_TIP_FLAG_ */
} FH2Tip;
```

A hint with a button in it, "Press {button} to keep it", shown once rather than every session: a pop-up with your mod's name as its title and the tip as its line, after `delay_s` seconds of driving with nothing else showing and at least 3 minutes after any other mod's tip, never in the first 12 seconds the driving view is up (the platform's own notice), and after every waiting pop-up. One tip per mod a session (a second `show_tip` takes the waiting one's place), and three from every mod together.

- **The button** is written for the device the player used last, as it is when the tip shows: "(A)" on a controller, "[SPACE]" on the keyboard, a chord as "(LB) + (RB) + (X)", a key with modifiers as "[CTRL] + [Z]". Set `button.vk` and `button.glyph` both, so it's right on either; one without the other is used on both. The space bar, the arrow keys and the d-pad are named in the game's language ("[SPACJA]" and "(PAD KIERUNKOWY GÓRA)" in Polish), or in English where the text reaches the game in plain letters and the word has none (Russian, Japanese, Korean, Chinese). The other keys are named as the player's keyboard layout names them, which is what the hotkey rows show too ("[ENTER]", "[F9]", "[ENTF]" on a German keyboard), or in English when that name isn't in plain letters. Set `button.hotkey` to one of your hotkeys' keys (API 9) and it shows the player's current binding; a tip whose hotkey is bound to nothing then isn't shown.
- **Seen.** The platform remembers which tips were seen, in `fh2-mods\platform-seen.txt`, by your DLL's file name and the tip's `key` (written there as `tip.<key>`). A tip counts as seen only once it was really drawn: with the game's Minor notifications off, `show_tip` returns 0 and the tip isn't used up. With `FH2_TIP_FLAG_UNTIL_DONE` it's seen only when you call `set_tip_seen(mod, key, 1)`, once the player has done the thing, and `max_sessions` caps how many sessions it shows in until then.
- **`tip_seen`** returns 1 if the tip `key` has been seen. **`set_tip_seen`** marks it seen (1), which also drops it if it's waiting, or not seen (0), to show it again.
- **The player** can turn tips off ("Tips" on the platform's page), which also drops a waiting tip without marking it seen, and bring every mod's tips back ("Show tips again"). Deleting `platform-seen.txt` brings them back too, which is also how to test yours.
- **Returns** 1 if queued, 0 if it was seen, tips or the game's Minor notifications are off, or a button is set but the text has no `{button}` (the log says which).

### show_message_box

```c
typedef void (*FH2BoxChooseFn)(void* user, const char* key, uint32_t choice);

typedef struct FH2MessageBox {
    uint32_t size;                 /* sizeof(FH2MessageBox) */
    const char* key;               /* passed to on_choose */
    const char* title;             /* UTF-8, shown in capitals; 30 characters */
    const char* message;           /* UTF-8, up to 200 characters, wrapped; may be NULL */
    const char* const* choices;    /* UTF-8, copied; put the safe one first; no Back of your own */
    uint32_t choice_count;         /* 1 to FH2_TITLE_MAX_CHOICES - 1 (the platform adds Back); over 10, 8 at a time */
    uint32_t focus;                /* the choice the focus starts on */
    FH2BoxChooseFn on_choose;      /* may be NULL */
    void* user;
} FH2MessageBox;
```

The game's own message box over the page the player is on: the screen behind blurred, a title, a message, your choices and Back, which the platform adds last.

- **Only from your callbacks**: a page's `on_select`, `on_button`, `on_slot` or `on_open`, a setting's `on_change` (or a slider's `on_slide`), or a title-screen entry's `on_press` or `on_choose`. From anywhere else (a hotkey, a command, while driving), or from a box's own `on_choose`, it returns 0 and the log says so: send the player to your page instead.
- **`on_open`**: a page on a tile runs it as the tile opens it, and the box shows as soon as the page has loaded. A page under YOUR MODS runs it as YOUR MODS opens, for every mod's pages at once, before any of them is shown, so a box from there is refused: show it from the page's `on_select` instead.
- **One box at a time**, and nothing is queued, so a box never appears later over a page the player didn't ask on. A callback shows one box at most, and that includes the callbacks it sets off and is called from (an `on_slot` that `set_page_slot` runs inside your `on_button` counts as the same callback). A box is also refused while another is waiting for its page to open, or has only just been shown: two in the same moment would leave the game with one it can't answer.
- **A box or the game, not both**: a title-screen entry's `on_press` or `on_choose` that shows a box and returns `FH2_TITLE_START` keeps the box, and the game doesn't start (the log says so). Return `FH2_TITLE_HANDLED` whenever you show one.
- **Text.** The title is shown in capitals made in the player's language, cut at 30 characters; the message at 200, wrapped; each choice at 32. Cut text ends in an ellipsis, and the log says what was cut. The text reaches the game's box as UTF-16, so any letter its fonts have shows.
- **Choices.** 1 to 99, and the platform adds the game's Back as the last item. Over 10, they're shown 8 at a time, with Previous and Next items and the page's number after the title, as the title screen's lists are. The box opens on the page holding `focus`; the game's box puts the cursor on that page's first item.
- **`on_choose`** gets the choice (from 0) or `FH2_BOX_BACK`, for the Back item or the B button, on the game's UI thread: keep it short, and don't call into the game from it. Previous and Next don't call it. It may never be called if the player leaves the box another way (and B calls nothing in a session where the game's code for it isn't as expected, which the log says), so never rely on it being called. From it, `show_toast`, `set_tip_seen`, `show_page_hint` and `play_ui_sound` work; `show_message_box` doesn't.
- **Writing one:** ask a question in the title; say in the message what happens and what's kept; put the safe answer first, where the cursor starts; add no Back of your own.
- **Logged**: the box shown, with its title and key, and the answer ("message box answered: RESET YOUR PROGRESS? -> Keep my progress").
- **Returns** 1 if shown (or, from a tile page's `on_open`, on its way), 0 if not: not from a callback, `on_open` as YOUR MODS opens, a box already shown from this callback or still coming up, no title or no choices (the log says why).

### set_page_help, set_page_hints and show_page_hint

```c
typedef struct FH2Prompt {
    uint32_t glyph;                /* FH2_GLYPH_ */
    uint32_t vk;                   /* the key, a Windows virtual-key code (VK_SPACE, 'R'); 0 = the glyph always */
    uint32_t modifiers;            /* FH2_KEY_CTRL | FH2_KEY_SHIFT | FH2_KEY_ALT (API 9), shown before it */
    uint32_t word;                 /* FH2_WORD_: the game's own word instead of `label` */
    const char* hotkey;            /* or the key of one of your hotkeys: shows its current binding; NULL = not used */
    const char* label;             /* UTF-8: "Keep it"; ignored when `word` is set */
    int32_t hold;                  /* nonzero: "Hold" before it, with the hold fill (the HUD layer only) */
    uint32_t reserved[5];          /* zero */
} FH2Prompt;
```

- **`set_page_help`** sets your page's help bar: these prompts (copied), beside the platform's own (B) Back and, when the page has `on_select`, (A) Select, in the game's order (A, B, X, Y, LB, RB, whatever order you give them in). A prompt for A or B relabels the platform's. Only buttons a callback of the page can handle are accepted: A (with `on_select`) and B on a page of rows; A, B, X, Y, LB and RB on a page of your own design with `on_button`, and only B without it. The last prompt you give for a button wins. A label is cut at 24 characters; `vk`, `modifiers`, `hotkey` and `hold` mean nothing on pages, which the controller drives. On a page of your own design, the help bar replaces the help fragment you gave `set_page_xaml`. A page that's both (rows under YOUR MODS, your own design on its tile) shows only its A and B prompts as rows. It returns how many prompts were accepted, and the log says why any weren't. The bar changes the next time the page opens.
- **`set_page_hints`** gives the texts your page's hint line can show (copied; up to 8, each up to 80 characters, cut with an ellipsis). Call it from `fh2mod_init` or the page's `on_open`; it takes effect the next time the page opens. Pass `tr()` results: they follow the game's language, as rows do. It returns 1, or 0 for no such page.
- **`show_page_hint`** shows hint `index` in `kind`, or none for -1: `FH2_HINT_INFO` (in the grey of the platform's own hints), `FH2_HINT_ATTENTION` (orange, with the game's info icon) or `FH2_HINT_FAULT` (red, with its alert icon). Yellow is never used: on the platform's pages yellow means an update. From the page's own callbacks while it's up (its `on_select`, `on_button` or `on_slot`, or the `on_choose` of a box shown from them) it changes on screen at once, when the page has its hint line; from `on_open`, or at any other time, it shows the next time the page opens. A page on its tile gets the hint line only if it had hints when it opened, so call `set_page_hints` from `fh2mod_init` or `on_open`, not first from a callback while the page is up. It returns 1, or 0 for no such page, hint or kind.
- **Where the hint line is**: under the list on a page of rows (YOUR MODS' own hint and the restart notice use the same place), and under your content on a page of your own design. It's there only while a hint is shown, and takes no room on a page that has no hints. On a page of your own design with hints, your content gets the height above the hint line (the "*" row of a Grid, the hint line in an "Auto" row under it), so star-sized rows and scrolling lists in it work as they do without hints.
- **Glyphs** (`FH2_GLYPH_`) are the game's own controller pictures, 0 to 24: A, B, X, Y, LB, RB (1 to 6, the same numbers as `FH2_BUTTON_A` to `FH2_BUTTON_RB`, so a page's button code is its glyph), LT, RT, Menu, View, the d-pad's four directions and its up/down and left/right pairs, L3, R3, and each stick with its up/down and left/right. The HUD layer preview uses the same numbers.
- **Words** (`FH2_WORD_`) are in the player's language with no translation work: `SELECT`, `BACK`, `ACCEPT`, `TOGGLE`, `PREVIOUS` and `NEXT` are the game's own help-bar words, and `ON` and `OFF` the platform's (in the same 12 languages as its own text).
- `FH2Prompt` is embedded in `FH2Tip` and passed in arrays, so it's frozen: it never grows, and `reserved` must be zero. `FH2Toast`, `FH2Tip` and `FH2MessageBox` start with `size`, and the platform reads only that many bytes of them, so a mod built against a later header still works here, and one built against this header works on a later platform.

### fh2mod_ui.h: the same calls on any platform

[`include/fh2mod_ui.h`](../include/fh2mod_ui.h), header-only, wraps every member as `fh2ui_<member>`, with the member's own arguments but without `api` and `mod`. On a platform whose `FH2ModApi` hasn't got the member (it checks `api->size`), each does nothing and returns 0, so a mod built against API 10 runs on an older platform without its templates and needs no version checks. Call `fh2_helpers_init(api, mod)` once, straight after `register_mod`; it keeps the two pointers ([`include/fh2mod_helpers.h`](../include/fh2mod_helpers.h): automatic in C++, and in C define `FH2_HELPERS_IMPLEMENTATION` in one `.c` file). Two more: `fh2ui_notify(title, line)`, a pop-up with no key, icon or sound, in one call; and `fh2ui_notify_or_log(title, line)`, which writes "title: line" to `platform.log` when the pop-up isn't shown.

What to do when a call returns 0 is yours to choose. Some suggestions: for a pop-up, your own path or a line in the log; for a tip, a title-screen notice (`set_notice`); for a message box, a page row with a description; for page help and the hint line, the help fragment and row descriptions you have today. Every mod's release still ships the platform it was built with, and an older platform from another mod's zip takes the templates away until a newer one is installed again.

```cpp
#include "fh2mod_ui.h"
#include "fh2mod_l10n.h"

// in fh2mod_init
g_mod = api->register_mod(&info);
fh2_helpers_init(api, g_mod);
fh2ui_ui_uses(FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS);

// a hotkey's on_press: a pop-up that answers it, or a sound and a log line
static void on_snapshot(void*, const char*) {
    FH2Toast t{};
    t.size     = sizeof t;
    t.key      = "Snapshot";   // a second press replaces it while it waits
    // L10N: pop-up title
    t.title    = FH2_TR(FH2_API, FH2_MOD, "toast.snapshot", "Snapshot written");
    t.subtitle = FH2_TR(FH2_API, FH2_MOD, "toast.snapshot.where", "WreckingBall-defs.txt, next to the game");
    t.icon     = FH2_ICON_PHOTO;
    t.flags    = FH2_TOAST_FLAG_REPLY;
    if (!fh2ui_show_toast(&t)) {   // off, or an older platform
        fh2ui_play_ui_sound(FH2_SOUND_SELECT);
        FH2_API->log(FH2_MOD, "Snapshot written (no pop-up shown).");
    }
    fh2ui_set_tip_seen("FirstSnapshot", 1);   // the tip below has done its job
}

// a tip that follows the hotkey's binding, until the player has used it
FH2Tip tip{};
tip.size = sizeof tip;
tip.key  = "FirstSnapshot";
tip.text = FH2_TR(FH2_API, FH2_MOD, "tip.snapshot", "Press {button} to write a snapshot");
tip.button.hotkey = "Snapshot";   // [F8], or the player's own binding
tip.flags = FH2_TIP_FLAG_UNTIL_DONE;
tip.max_sessions = 3;
fh2ui_show_tip(&tip);

// a confirm box from your page's on_select (the row "Reset")
static void on_reset_choice(void*, const char*, uint32_t choice) {
    if (choice == 1) reset_progress();   // 0 is "Keep my progress"; FH2_BOX_BACK is Back
}
static void on_select(void*, const char*, const char* row_key) {
    if (strcmp(row_key, "Reset") != 0) return;
    const char* choices[] = {FH2_TR(FH2_API, FH2_MOD, "box.keep", "Keep my progress"), FH2_TR(FH2_API, FH2_MOD, "box.reset", "Reset")};
    FH2MessageBox b{};
    b.size = sizeof b;
    b.key = "Reset";
    b.title = FH2_TR(FH2_API, FH2_MOD, "box.title", "Reset your progress?");
    b.message = FH2_TR(FH2_API, FH2_MOD, "box.msg", "Every achievement and counter goes back to locked. Your save isn't touched.");
    b.choices = choices;
    b.choice_count = 2;
    b.on_choose = on_reset_choice;
    fh2ui_show_message_box(&b);   // works here: on_select is a page callback
}
```

### What the player controls

The player decides whether pop-ups and tips show, in rows the platform adds itself. None of them is an API call: write your pop-ups so that they can be off.

- **The platform's own page.** "FH2 Mod Platform", the last line of YOUR MODS's list of mods (Pause > MODS > YOUR MODS), opens the platform's page:
  - **Mod pop-ups**: "Same as Notifications" (the default) or "Off". The game's notification only shows while its own Notifications option is on (Pause > Settings > HUD), so the platform can't offer more than following it. Off: no mod's pop-ups show, those waiting are dropped, and `ui_status` says `FH2_UI_TOASTS_OFF`.
  - **Tips**: On or Off, for every mod's tips (`FH2_UI_TIPS_OFF`). It doesn't silence the platform's own hints about where mod settings are, which stop by themselves once the player has opened YOUR MODS.
  - **Show tips again**: A marks every mod's tips unseen, so each shows once more.
  - **Recent pop-ups**: a page of the last 10 pop-ups and tips, newest first, each with the time it showed, its title (a tip's line) and the mod that sent it, "(private)" for one flagged `FH2_TOAST_FLAG_PRIVATE`. It answers "which mod keeps popping up?", since a pop-up in the game's look can't be told from the game's own. It's up to date each time YOUR MODS opens.
- **Your mod's page.** With `FH2_UI_USES_TOASTS` declared and no `FH2_UI_USES_OWN_SWITCH`, your page gets a **Pop-ups** row (On/Off) above Load this mod, so a player can quieten your mod without turning every mod's pop-ups off. With `FH2_UI_USES_OWN_SWITCH`, your own row is the switch: check it before calling `show_toast`.
- **At once, and kept.** A change takes effect straight away, needs no restart, and is kept in `fh2-mods\platform-popups.ini`: `[player]` `ModPopups` and `Tips` (1 or 0), and `[mods]` with a line per mod whose row is off (`MyMod.dll=0`, by its DLL's file name). Players can edit the file too; it's read when the game starts.
- The list that stands in for the MODS tab when its menus can't show (Pause > Settings > MODS) has the same rows, under each mod and under "FH2 Mod Platform".

### Trying templates: `fh2mod Platform ui` and the test page

The platform has a command of its own, run with `fh2mod.exe` like a mod's ([Commands](#commands-api-9)):

```
fh2mod Platform ui            what's on screen and waiting, and why; the player's choices
fh2mod Platform ui recent     Recent pop-ups, as the page lists them
```

`ui` prints whether pop-ups can show this session (and why not), whether the platform's watch on the game's own notifications is in, the game's Notifications option, the HUD page's state, what's on screen and each pop-up and tip waiting with what it waits for, then the player's choices. A private pop-up shows by its key only, so the output can go into a bug report as it is. The command never starts the command pipe by itself: it works whenever one of your mod's commands has started it, or with `Developer=1`.

**Developer=1.** Add it to `fh2-mods\platform-popups.ini` and start the game:

```ini
[popups]
Developer=1
```

Players never see what it adds:

- The command pipe starts even when no mod has a command, and the pop-up hooks go in even when no mod declared pop-ups or tips.
- `ui`'s tests, each from the platform (its own pop-ups use none of your mod's limits; the player's Mod pop-ups and Tips rows still apply):

  ```
  fh2mod Platform ui toast Snapshot written | WreckingBall-defs.txt, next to the game | photo reply
  fh2mod Platform ui tip Press {button} to try it | A
  fh2mod Platform ui keyboard      buttons in tips written as keys, whatever was used last
  fh2mod Platform ui pad           ... for the controller
  fh2mod Platform ui auto          ... for the device used last again
  fh2mod Platform ui demo          four pop-ups (normal, reply, urgent, private) and a tip, each saying what it is, and a sound
  fh2mod Platform ui set Tips off  Mod pop-ups (ModPopups), Tips or a mod's Pop-ups row (by its name), as the player sets them
  ```

  `toast` takes the title, the line, then any of an icon (`none`, `trophy`, `roads`, `photo`, `boards`, `bucket`, `carmeet`, `club`, `payout`, `barnfind`) and `urgent`, `reply`, `private`, separated by `|` (so no quotes are needed; text in double quotes, `\"Title\" \"Line\"` on a command line, works too). A test pop-up has the key `Test`, so the next replaces it while it waits. `tip` takes its text, with `{button}`, and a button: `A`, `B`, `X`, `Y`, `LB` or `RB` (with Space, Esc, X, Y, Q and E for the keyboard); it shows after 5 s of driving, at least 3 minutes after the last tip. `set` takes `ModPopups`, `Tips` or a mod's name, then `on` or `off`, and changes that row as the player would: at once, kept in `fh2-mods\platform-popups.ini` (written a moment later, with its comments when the file is new). A mod has a Pop-ups row only when it declared pop-ups without a row of its own.
- **UI templates**, a page on the platform's own page, whose rows each fire one template once when you press A: a pop-up, an urgent one, a reply, a private one, a tip, the next UI sound, a message box, the next of the page's hint texts, the three button settings above, everything in turn, and taking down the platform's waiting test pop-ups. Pop-ups and tips wait while you're in the menus, so they show once you're back driving. The log says what each did. The page's text is English: it's for developers only.
  There's no row for the test language: switch it on with `test=1` in `fh2-mods\platform-language.ini` and start the game again ([Translations](#translations-api-10)). The platform reads it once at start-up, because text set in `fh2mod_init` and the title screen's lines can't change language during a session, so a row would leave a mix of the two.

Everything the tests do is logged in the platform's name (`[Platform]`), as a mod's would be.

## Crash reports

These need no code from you. If the game crashes, the platform writes `fh2-mods\crashes\crash-<date>-<time>.txt` and a minidump (`.dmp`) beside it, and adds the summary to `platform.log`. The report has the exception, the call stack as module+offset (`game+0x...`, `YourMod.dll+0x...`), the mod the crash came from, if any, and every hook in place (see above). When Azimuth 1.0.1 was running another game, it says which, so a crash there isn't taken for a mod's. The ten newest reports are kept. Open the minidump in Visual Studio or WinDbg with your mod's debug symbols to see the exact line.

From platform 2.8.0:

- **Your mod's state is in the dump.** The minidump has the writable data (the globals) of every DLL the platform loaded: the mods, fh2wheel and the platform itself. Other modules' data stays out, so the dump stays small. `platform.log` says how many DLLs' data and how many added ranges went in.
- **`add_dump_memory(mod, p, n)`** (API 9) adds memory of your own to every dump from then on, for state that lives on the heap behind your globals: your tables, what your hooks keep track of. 1 byte to 64 MB a range, 64 ranges at most from every mod together. The memory has to stay allocated; a range that can't be read at the crash is skipped.
- **A full-memory dump** while you chase a crash: put `full` in `fh2-mods\platform-dump.txt`. The next dump then has the whole process, which under Azimuth is several GB, so it's named `crash-<date>-<time>-full.dmp` and only the newest one is kept. Delete the file (or the word) to go back to small dumps.
- **Symbols.** The report lists the PDB (name, GUID and age) of each loaded DLL that has a CodeView record, which is what a debugger needs to load the right symbols. Release builds with llvm-mingw are stripped and have none; build with `-gcodeview -Wl,--pdb=` and keep the PDB if you want them.
- **`fh2-mods\platform-session.json`** says `crashed`, with the report's name (see Sessions).

Wine's dbghelp writes the dumps too; whether it honours the mods' data and added ranges there hasn't been checked yet.

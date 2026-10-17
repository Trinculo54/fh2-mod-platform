# API reference

Everything is in [`include/fh2mod.h`](../include/fh2mod.h). This page goes through it member by member. API version 1 came with platform 2.0, and version 2 with platform 2.1. Version 2 added file overrides, database patches and interfaces between mods; the sections for those say so.

## Rules that apply everywhere

- **Threads:** every function is thread-safe and can be called from any thread.
- **Strings:** strings passed in are copied, so you can free yours straight after. Names, versions, log lines and notices are UTF-8 (`const char*`). Game text is UTF-16 (`const wchar_t*`), because that's what the game uses.
- **Memory:** nothing the platform returns needs freeing, and the `FH2ModApi` pointer stays valid until the process ends.
- **Compatibility:** members are only ever added at the end of `FH2ModApi`. If you use one added after version 1, check `api->api_version` (or `api->size`) first, so your mod still runs on an older platform.

## Entry point

```c
FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api);
```

Export this from your DLL. The platform loads every DLL in `fh2-mods` in alphabetical order, then calls each one's `fh2mod_init` in the same order, on its own thread and outside the loader lock. Return 0 on success. Any other value, or a crash inside the call, is logged; the crash is caught, so it won't take the game down. Once every mod's `fh2mod_init` has returned, the platform writes all string-table edits and notices.

DLLs without `fh2mod_init` still load, and are logged as mods from before the platform.

## FH2ModApi fields

| Field | Meaning |
|---|---|
| `size` | `sizeof(FH2ModApi)` as the platform knows it. |
| `api_version` | The platform's `FH2MOD_API_VERSION`. |
| `platform_version` | The platform's version, like `"2.1.0"`. |
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
| `github_repo` | `"Owner/Repo"`, or null. When set (and `check_updates` is 1), the platform checks your latest release on GitHub, at most every six hours, and shows "Name x.y.z is available" on the title screen when it's newer than `version`. |
| `platform_version` | The platform version you ship with. Pass `FH2MOD_PLATFORM_VERSION`. If the installed platform is older, players get "Old FH2 Mod Platform: reinstall Name". |
| `check_updates` | 0 to never go online for your mod. Give players a setting for it. |

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

Replaces every string in `table` that is exactly `from` (the whole string, case-sensitive) with `to`. This runs after key edits, so it sees their results. It's handy for text that's the same in every language, like the names in `AIPlayers.str`.

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

Edits are written to Extra Options' `mediaoverride` folder, as `mediaoverride\Stripped\StringTables\<language>\<table>`. Without Extra Options (or with `EnableMediaOverrides=0`), the files are still written but the game won't read them. The tables the platform wrote are listed in `fh2-mods\platform-tables.txt`, so that ones no longer needed get cleaned up.

## set_notice

```c
void set_notice(FH2Mod* mod, const char* utf8_lines);
```

Sets your lines on the title screen, under "PRESS (A) TO START". Separate lines with `\n`; empty lines are dropped, and null or `""` clears them. Each line fits about 40 characters. Your lines appear together with every other mod's, sorted by mod name, with the platform's own update and version lines for your mod after yours.

They're saved to `fh2-mods\notices\<name>.txt` and written into `Main.str` the next time strings are applied, which happens on its own after `fh2mod_init`. Mods from before the platform (WheresMyBuddy 1.1 and 1.2, FH2 Universal Radio 1.2) read and write the same files, so their notices and yours always end up together.

## game_base and wait_for_game

```c
void* game_base(void);
void* wait_for_game(uint32_t timeout_ms);
```

`game_base` returns the base address of `forza_durango_release_final.exe` once Azimuth has mapped it into the process, or null before that. Azimuth maps it when the player presses Play. `wait_for_game` waits until it's mapped, or until `timeout_ms` runs out (`0xFFFFFFFF` waits forever), and returns the base or null. It checks every 100 ms. Call it from your own thread, never from `fh2mod_init`, which would hold up every mod loaded after you.

The game is recognised by its PE header: the 24 November 2015 build, timestamp `0x565501ED` and image size `0x03A18000`. Azimuth maps it at its preferred base, `0x140000000`, but use the returned address anyway.

## set_file and clear_file (API 2)

```c
int set_file(FH2Mod* mod, const char* media_path, const void* data, size_t size);
void clear_file(FH2Mod* mod, const char* media_path);
```

`set_file` replaces a file the game reads from `game:\media`. Give the path relative to `media`, with forward or back slashes. For a file inside a zip, use the zip's name without `.zip` as a folder: `media\UI.zip\x.xml` is `"UI/x.xml"`. The file is written straight away to Extra Options' `mediaoverride` folder, which Extra Options mounts over `media`. If the file there already has the same content, it isn't rewritten.

Only one mod can replace a file. `set_file` returns 1 when the file is yours and written. It returns 0 if another mod already replaces it, if the path isn't a plain path under `media`, or if the write failed, and the log says which. String tables and the game database can't be replaced whole; use the functions for those. If someone had put a file at that spot by hand, it's kept next to it as `<name>.bak` the first time.

`clear_file` deletes your override, so the game reads its own file again. Pass null to clear every file you replace. The files the platform wrote are listed in `fh2-mods\platform-files.txt`. About 15 seconds after startup, any listed file that no mod has claimed this launch is deleted. That's what cleans up after a mod that was removed.

The game reads most files once, when it first needs them, so call `set_file` early, from `fh2mod_init` or soon after.

## add_sql, clear_sql and apply_database (API 2)

```c
void add_sql(FH2Mod* mod, const char* utf8_sql);
void clear_sql(FH2Mod* mod);
void apply_database(FH2Mod* mod);
```

`media\Stripped\gamedbRC.slt` is a SQLite database that holds most of the game's data: cars (`Data_Car`), events, AI drivers and around 280 more tables. `add_sql` queues statements to run against it. One call can hold several statements separated by semicolons, and calls add up.

The platform copies the original database and runs every mod's statements, in load order, each mod inside its own transaction. If any statement from a mod fails, all of that mod's changes are rolled back and the SQLite error is logged, while the other mods' changes still apply. The result goes to `mediaoverride\Stripped\gamedbRC.slt`. When no mod has statements any more, it's deleted.

Building the copy takes a moment, so it's only redone when something changed. `fh2-mods\platform-db.txt` remembers a hash of the original database and every mod's statements, and the log repeats the results from the last build.

Statements added in `fh2mod_init` are applied for you. For later ones, call `apply_database` from your own thread. Like string tables, the database is read once at startup, so later changes show from the next launch.

SQLite comes from Windows itself (`winsqlite3.dll`, part of Windows 10 and 11). To look around the database, open a copy of it in any SQLite browser.

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

## Crash reports

These need no code from you. If the game crashes, the platform writes `fh2-mods\crashes\crash-<date>-<time>.txt` and a minidump (`.dmp`) beside it, and adds the summary to `platform.log`. The report has the exception, the call stack as module+offset (`game+0x...`, `YourMod.dll+0x...`), and the mod the crash came from, if any. The ten newest reports are kept. Open the minidump in Visual Studio or WinDbg with your mod's debug symbols to see the exact line.

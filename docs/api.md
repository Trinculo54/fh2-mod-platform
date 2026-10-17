# API reference

Everything is in [`include/fh2mod.h`](../include/fh2mod.h). This page goes through it member by member. API version 1 is the platform 2.0 API.

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
| `platform_version` | The platform's version, like `"2.0.0"`. |
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

Drops your edits to `table`, or to every table if `table` is null. A table nobody edits any more gets its override files removed the next time strings are applied, so the game uses its own text again.

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

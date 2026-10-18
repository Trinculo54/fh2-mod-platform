# API reference

Everything is in [`include/fh2mod.h`](../include/fh2mod.h). This page goes through it member by member. API version 1 came with platform 2.0, version 2 with platform 2.1, version 3 with platform 2.2 and version 4 with platform 2.3. Version 2 added file overrides, database patches and interfaces between mods, version 3 added hooks, and version 4 the Mods page; the sections for those say so.

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
| `github_repo` | `"Owner/Repo"`, or null. When set (and `check_updates` is 1), the platform checks your latest release on GitHub, at most every six hours, and shows "Name x.y.z is available" on the title screen when it's newer than `version`. The six hours start over when a different `version` is installed, and a remembered release older than `version` is never trusted, so installing an older or newer build checks again straight away. |
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

## The Mods page (API 4)

```c
int add_setting(FH2Mod* mod, const FH2Setting* setting);
int get_setting(FH2Mod* mod, const char* key);
void set_setting(FH2Mod* mod, const char* key, int value);

typedef struct FH2Setting {
    uint32_t size;                 /* sizeof(FH2Setting) */
    const char* key;               /* unique within your mod: "Trees" */
    const char* label;             /* the row's label: "Breakable trees" */
    const char* description;       /* beside the list while the row is selected; may be NULL */
    int value;                     /* 0 = Off, 1 = On */
    int needs_restart;             /* nonzero: a change only takes effect the next time the game starts */
    FH2SettingChangedFn on_change; /* void (*)(void* user, const char* key, int value); may be NULL */
    void* user;
} FH2Setting;
```

These put On/Off switches for your mod in the game's own menus. When at least one mod adds a row, Pause > Settings gets a sixth tile, MODS, and the page behind it lists every mod's rows in load order. It's laid out like the HUD options page: the row's description, followed by your mod's name, shows beside the list.

Add your rows from `fh2mod_init`. The platform writes the page once every mod's `fh2mod_init` has returned, so a row added after that only shows from the next launch (the log says so). `key` is letters, digits and `_`, and unique within your mod. `add_setting` returns 1, or 0 with the reason in the log.

**Your mod owns the value.** The platform doesn't store it anywhere. Pass the current value in `value`, from your ini or wherever you keep your settings. When the player changes the row, `on_change` gets the new value with your `user` pointer: save it, and start using it if it can change on the fly. `on_change` runs on the game's UI thread, inside the menu's own code, so keep it short and don't call into the game from it. A crash in it is caught and logged. `get_setting` returns a row's current value (-1 if you have no row called `key`). If you change a value yourself, from a hotkey say, `set_setting` makes the page show it the next time it opens; `on_change` isn't called for that.

**Switches that need a restart.** Some changes can only take effect when the game starts, like anything the game reads once while loading. Mark those rows `needs_restart`. Their description then ends with "Takes effect the next time you start the game." While any of them differs from the value the game started with, a notice under the list says so. When the player leaves the Mods page with such a change, the game's own message box offers **Restart now**, **Restart later** and **Go back**. Restart now closes the game the way Azimuth's own Quit does, then starts Azimuth again with `--skip-launcher`, straight back into the game. Your `on_change` has already been called by then, so save there and the restart picks up the new value.

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

The log (`[Mods page]`) says when the page is ready, and each change is logged under your mod's name. If the game's menu code doesn't look as expected, there's no tile or page and the platform's three files for it are removed again; your mod keeps running and simply isn't told of changes. [`game-notes.md`](game-notes.md#menus) explains how the page is put together.

## Crash reports

These need no code from you. If the game crashes, the platform writes `fh2-mods\crashes\crash-<date>-<time>.txt` and a minidump (`.dmp`) beside it, and adds the summary to `platform.log`. The report has the exception, the call stack as module+offset (`game+0x...`, `YourMod.dll+0x...`), the mod the crash came from, if any, and every hook in place (see above). The ten newest reports are kept. Open the minidump in Visual Studio or WinDbg with your mod's debug symbols to see the exact line.

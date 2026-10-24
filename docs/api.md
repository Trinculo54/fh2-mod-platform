# API reference

Everything is in [`include/fh2mod.h`](../include/fh2mod.h). This page goes through it member by member. API version 1 came with platform 2.0, version 2 with 2.1, 3 with 2.2, 4 with 2.3, 5 with 2.4, 6 with 2.5, 7 with 2.6 and 8 with 2.7. Version 2 added file overrides, database patches and interfaces between mods, version 3 hooks, version 4 the Mods page, version 5 its sliders, version 6 mod descriptions and link rows for the MODS tab, version 7 byte patches and instruction hooks, and version 8 pages of your own; the sections for those say so.

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

The platform copies the database the game would read and runs every mod's statements, in load order, each mod inside its own transaction. If any statement from a mod fails, all of that mod's changes are rolled back and the SQLite error is logged, while the other mods' changes still apply. The result goes to `mediaoverride\Stripped\gamedbRC.slt`, which the game reads first.

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
- **Two Azimuth folders sharing one game folder** don't know each other's copy. Keep the mods that change the database in one of them. FH2MP can be in the other: from platform 2.7.2, each folder lists the size and date of its FH2MP's copies in `mediaoverride\Stripped\platform-db-fh2mp.txt`, so the other folder doesn't take the server's database for another mod's.

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

## The Mods page (API 4; sliders API 5)

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
    int kind;                      /* FH2_SETTING_ON_OFF or FH2_SETTING_SLIDER */
    float min, max;                /* the slider's range */
    float step;                    /* one press of left or right; 0 = a hundredth of the range */
    float slider_value;            /* its current value */
    float display_scale;           /* the number shown is value * this (100 shows 0.12 as 12); 0 = 1 */
    int decimals;                  /* digits after the decimal point, 0 to 3 */
    FH2SliderChangedFn on_slide;   /* void (*)(void* user, const char* key, float value); may be NULL */
} FH2Setting;
```

These put On/Off switches and sliders for your mod in the game's own menus. When at least one mod adds a row, Pause > Settings gets a sixth tile, MODS, and the page behind it lists every mod's rows in load order. It's laid out like the HUD options page: the row's description, followed by your mod's name, shows beside the list.

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

The log (`[Mods page]`) says when the page is ready, and each change is logged under your mod's name. If the game's menu code doesn't look as expected, there's no tile or page and the platform's three files for it are removed again; your mod keeps running and simply isn't told of changes. [`game-notes.md`](game-notes.md#menus) explains how the page is put together.

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

A page is a list of your own in the game's menus, laid out like the Mods page: each row has a label on the left and a value on the right, and the selected row's description shows beside the list, with your footer line at the bottom of it. It's for showing things (stats, a collection, what your mod has found) and, with `on_select`, for simple menus where A on a row does something.

Your mod's own page under MODS > Installed mods gets a row for each of your pages (`label`, with `description` beside it), and A on it shows the page; B goes back. A page can also take over a pause-menu tile that does nothing on PC. For now that's `"Achievements"`, the tile on the pause menu and the one on its Storm Island tab, which on Xbox opened the console's achievements app. Only one mod can have a tile; a second one is told so in the log, and its page is still under Installed mods.

Add pages from `fh2mod_init`. `add_page` returns 1, or 0 with the reason in the log. Pages are written when every mod's `fh2mod_init` has returned, and a page added later only shows from the next launch.

**Rows.** `set_page_rows` gives a page its rows, copied, in order, replacing the ones it had (up to 1000). You can call it at any time, from any thread. The page is written again each time it's opened, from the rows you last set, so keep them up to date as your data changes. If working them out is costly, do it in `on_open`, which runs just before the page is written: when its tile opens it, and when MODS > Installed mods opens, since that page holds every mod's pages. Until you set any rows, the page says it has nothing on it yet.

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

The log (`[Mods page]`) says when a tile opens a page, and each `on_select` is logged under your mod's name. If the tile's code doesn't look as expected, the tile keeps doing what it did and the page is only under Installed mods. [`game-notes.md`](game-notes.md#menus) explains how the pages are put together.

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

The page keeps the frame of the game's options pages: the blurred background, the player bar, the help bar, and the screen's padding and tilt. `content` takes the place of what the HUD options page has inside that (its title, list and description panel), so it includes your own title (`A_Style_ContentControl_ScreenTitleBlock` is the game's). It has to name one element `x:Name="FirstItem"`, which gets the focus when the page opens; name your main list `OptionsList`, as the game's options pages do. `resources` goes into the page's `ResourceDictionary`, and `help`, if you give one, replaces the help bar's `DefaultHelpPanel` (keep one element with that name); without it the help bar has only Back. The fragments use the page's XML namespaces: `local:` is the game's (ForzaUI). `set_page_xaml` returns 1, or 0 if you have no such page, `content` has no FirstItem, or a fragment's tags don't balance (then the page keeps the XAML it had, and the log says where).

As with rows, the XAML is copied, and the page is written from the last you set each time it opens, so it's static: put your data straight into it, and call `set_page_xaml` again when it changes (or from `on_open`). Under MODS > Installed mods such a page shows its rows if you've set any, and otherwise its row says which tile opens it; a page of your own design needs a tile to be seen.

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

## Crash reports

These need no code from you. If the game crashes, the platform writes `fh2-mods\crashes\crash-<date>-<time>.txt` and a minidump (`.dmp`) beside it, and adds the summary to `platform.log`. The report has the exception, the call stack as module+offset (`game+0x...`, `YourMod.dll+0x...`), the mod the crash came from, if any, and every hook in place (see above). The ten newest reports are kept. Open the minidump in Visual Studio or WinDbg with your mod's debug symbols to see the exact line.

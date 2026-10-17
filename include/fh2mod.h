/*
 * fh2mod.h -- FH2 Mod Platform API (MIT licensed, see LICENSE).
 *
 * The FH2 Mod Platform is the d3dcompiler_47.dll next to azimuth.exe. It
 * loads every DLL in the fh2-mods folder and offers them the services in
 * FH2ModApi below, so mods don't each reinvent them and don't step on each
 * other's files.
 *
 * A mod is a 64-bit Windows DLL in fh2-mods that exports:
 *
 *     FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api);
 *
 * The platform calls it once, on its own thread (never under the Windows
 * loader lock), right after loading the DLL. Do your setup there, start
 * your own threads for anything slow, and return 0. The api pointer stays
 * valid for the life of the process.
 *
 * Strings passed in are copied; nothing you get back needs freeing. All
 * functions are thread-safe.
 *
 * Compatibility: api->api_version only ever grows, and new members are only
 * ever added at the end. Check api->size (or api_version) before using a
 * member newer than the version you built against.
 */
#ifndef FH2MOD_H
#define FH2MOD_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH2MOD_API_VERSION 3

#ifdef __cplusplus
#define FH2MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define FH2MOD_EXPORT __declspec(dllexport)
#endif

/* Opaque handle for one registered mod. */
typedef struct FH2Mod FH2Mod;
/* Opaque handle for one hook (API 3). */
typedef struct FH2Hook FH2Hook;

typedef struct FH2ModInfo {
    uint32_t size;                /* sizeof(FH2ModInfo) */
    const char* name;             /* display name, UTF-8: "WheresMyBuddy" */
    const char* version;          /* "1.3.0" */
    const char* github_repo;      /* "Owner/Repo" to check for new releases, or NULL */
    const char* platform_version; /* the platform version this mod ships with, e.g. FH2MOD_PLATFORM_VERSION */
    int check_updates;            /* 0 = never go online for this mod */
} FH2ModInfo;

typedef struct FH2ModApi {
    uint32_t size;                /* sizeof(FH2ModApi) as the platform knows it */
    uint32_t api_version;         /* FH2MOD_API_VERSION of the platform */
    const char* platform_version; /* "2.2.0" */
    const wchar_t* azimuth_dir;   /* folder with azimuth.exe, no trailing slash */
    const wchar_t* game_dir;      /* the game's Mount folder (game:\), or "" if not found */

    /* Call once from fh2mod_init. Returns your handle for the calls below.
       The platform then shows a title-screen notice when a newer release of
       your mod is out on GitHub (checked at most every 6 hours, and again as
       soon as a different version of your mod is installed), or when
       the installed platform is older than info->platform_version. */
    FH2Mod* (*register_mod)(const FH2ModInfo* info);

    /* One line to fh2-mods\platform.log, prefixed with your mod's name. */
    void (*log)(FH2Mod* mod, const char* utf8_line);

    /* ---- String tables -----------------------------------------------------
       The game's text lives in string tables (media\Stripped\StringTables\
       <language>.zip\<Table>.str). Edits are applied to every language and
       written to Forza Horizon 2 Extra Options' mediaoverride folder, merged
       with every other mod's edits to the same table. The game reads its
       string tables once at startup: edits made in fh2mod_init are always in
       time; later ones show from the next launch. See docs/string-tables.md
       for how to find keys. */

    /* Set the string with `key` in `table` ("Main.str") to `text`. */
    void (*set_string)(FH2Mod* mod, const char* table, uint16_t key, const wchar_t* text);
    /* Replace every string in `table` that is exactly `from` with `to`. */
    void (*replace_string)(FH2Mod* mod, const char* table, const wchar_t* from, const wchar_t* to);
    /* Forget this mod's edits to `table`, or to every table if NULL. */
    void (*clear_strings)(FH2Mod* mod, const char* table);
    /* Write the changed tables now. Done for you after fh2mod_init returns;
       call it yourself after edits you make later. */
    void (*apply_strings)(FH2Mod* mod);

    /* ---- Title screen ------------------------------------------------------
       Lines shown under "PRESS (A) TO START", with every other mod's. Keep
       each under ~40 characters. "\n" separates lines; NULL or "" clears.
       Written when apply_strings runs (or right away after init). */
    void (*set_notice)(FH2Mod* mod, const char* utf8_lines);

    /* ---- The game ----------------------------------------------------------
       Azimuth maps forza_durango_release_final.exe (the 24 Nov 2015 Xbox One
       build) into its own process when you press Play. */

    /* Base address of the mapped game, or NULL if it isn't mapped yet. */
    void* (*game_base)(void);
    /* Waits until the game is mapped (or timeout_ms passes; 0xFFFFFFFF =
       forever) and returns its base, or NULL. */
    void* (*wait_for_game)(uint32_t timeout_ms);

    /* ==== API version 2 (platform 2.1.0): check api_version >= 2 ======== */

    /* ---- Game files --------------------------------------------------------
       Replace a file the game reads from game:\media, through Extra Options'
       mediaoverride folder. `media_path` is relative to media, with a zip's
       name standing in for the zip: "drivatarglobalcache/DefaultCache.bin",
       or "UI/somefile.xml" for a file inside media\UI.zip. One mod per
       file: returns 1 if written (or already identical), 0 if another mod
       already replaces it or the write failed (the log says which). String
       tables and the game database have their own functions above and
       below, and can't be replaced whole. */
    int (*set_file)(FH2Mod* mod, const char* media_path, const void* data, size_t size);
    /* Stop replacing a file (or all of yours, if NULL): the override is
       deleted and the game reads its own copy again. Files a mod no longer
       replaces at all (because it was removed) are cleaned up shortly after
       startup. */
    void (*clear_file)(FH2Mod* mod, const char* media_path);

    /* ---- Game database -----------------------------------------------------
       media\Stripped\gamedbRC.slt is a SQLite database with most of the
       game's data (cars, events, AI drivers). add_sql queues statements
       (UTF-8, several per call is fine) to run against a copy of it; every
       mod's statements run in load order, each mod's in a transaction of its
       own, and the copy goes to mediaoverride. If a mod's SQL fails, only
       that mod's changes are rolled back, and the error is logged. The copy
       is rebuilt only when something changed. Statements added in
       fh2mod_init are applied for you; later ones need apply_database. */
    void (*add_sql)(FH2Mod* mod, const char* utf8_sql);
    /* Drop all of your statements (call apply_database after). */
    void (*clear_sql)(FH2Mod* mod);
    /* Rebuild the database copy now if anything changed. Slow (seconds):
       call it from your own thread. The game reads the database once, at
       startup. */
    void (*apply_database)(FH2Mod* mod);

    /* ---- Other mods --------------------------------------------------------
       Mods can offer each other functions. Publish a struct of function
       pointers under a name, like "WheresMyBuddy.Friends", and treat it like
       this API: never change a published member, only add new ones at the
       end and bump the version. */

    /* Offer `iface` (which must stay valid) as `name`, version `version`.
       Another mod can't take a name you published. */
    void (*publish_interface)(FH2Mod* mod, const char* name, uint32_t version, const void* iface);
    /* The interface published as `name`, if its version is at least
       `min_version`; otherwise NULL. Mods start in alphabetical order, so
       look up interfaces after wait_for_mods. */
    const void* (*get_interface)(const char* name, uint32_t min_version);
    /* Waits until every mod's fh2mod_init has returned (or timeout_ms
       passes). Returns 1 when they have. Call it from your own thread:
       from inside fh2mod_init it returns 0 straight away. */
    int (*wait_for_mods)(uint32_t timeout_ms);
    /* The version string of the registered mod called `name`, or NULL if
       it isn't installed. */
    const char* (*mod_version)(const char* name);

    /* ==== API version 3 (platform 2.2.0): check api_version >= 3 ======== */

    /* ---- Hooks -------------------------------------------------------------
       Change what game code does, through the platform, so every mod's
       changes to the same code fit together: several mods can hook one
       function, each calls on to the next through `original`, and they can
       be switched on and off in any order. Hooks on exactly the same function,
       call site or pointer share it; the platform refuses a hook whose bytes
       overlap any other hook's, or game code that Extra Options (or a mod
       patching the game by itself) already changed, and logs who hooked
       what. Crash reports list every hook in place.

       Hooks in game code have to wait for wait_for_code (below). Hooks
       elsewhere (Azimuth, a Windows DLL) can be made at any time.

       Each call makes a hook, switched off, and returns it (or NULL, with
       the reason in the log). `name` is for the log and crash reports, like
       "RadioStreamFmod::createSound"; it may be NULL. `detour` is your
       function, with the same signature and calling convention as the code
       it replaces. `*original` is set before the call returns: call it from
       your detour to run the code you replaced (and any other mod's detour
       behind yours). It's safe to call at any time; while your hook is off
       it goes straight to the game's own code.

       When several mods hook the same place, the most recently enabled
       detour runs first. */

    /* An inline hook at the start of a function (`target`), with MinHook. */
    FH2Hook* (*hook_function)(FH2Mod* mod, const char* name, void* target, void* detour, void** original);
    /* One CALL rel32 or JMP rel32 instruction at `call_site`: only that
       call goes to your detour; `*original` is the function it called. */
    FH2Hook* (*hook_call)(FH2Mod* mod, const char* name, void* call_site, void* detour, void** original);
    /* A function pointer in memory, like a vtable slot: `*slot` is replaced,
       and `*original` is what it held. */
    FH2Hook* (*hook_pointer)(FH2Mod* mod, const char* name, void** slot, void* detour, void** original);
    /* Switch `hook` on or off, or all of your hooks if it's NULL (in one go:
       the game's threads are paused once, for all of them). Returns 1 if
       they all are now; 0 if any couldn't be, with the reason in the log. */
    int (*enable_hooks)(FH2Mod* mod, FH2Hook* hook);
    int (*disable_hooks)(FH2Mod* mod, FH2Hook* hook);
    /* Like wait_for_game, but also waits until Extra Options has finished
       patching the game (it checks the code it patches first, and patches
       nothing if any of it was changed). Returns the game's base when it's
       safe to hook game code, or NULL on timeout. Call it from your own
       thread. */
    void* (*wait_for_code)(uint32_t timeout_ms);
} FH2ModApi;

/* What a mod exports. */
typedef int (*FH2ModInitFn)(const FH2ModApi* api);

#ifdef __cplusplus
}
#endif

#endif /* FH2MOD_H */

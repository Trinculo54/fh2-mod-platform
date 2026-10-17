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

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FH2MOD_API_VERSION 1

#ifdef __cplusplus
#define FH2MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define FH2MOD_EXPORT __declspec(dllexport)
#endif

/* Opaque handle for one registered mod. */
typedef struct FH2Mod FH2Mod;

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
    const char* platform_version; /* "2.0.0" */
    const wchar_t* azimuth_dir;   /* folder with azimuth.exe, no trailing slash */
    const wchar_t* game_dir;      /* the game's Mount folder (game:\), or "" if not found */

    /* Call once from fh2mod_init. Returns your handle for the calls below.
       The platform then shows a title-screen notice when a newer release of
       your mod is out on GitHub (at most one check every 6 hours), or when
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
} FH2ModApi;

/* What a mod exports. */
typedef int (*FH2ModInitFn)(const FH2ModApi* api);

#ifdef __cplusplus
}
#endif

#endif /* FH2MOD_H */

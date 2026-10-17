# Notes on the game

What's been worked out about Forza Horizon 2 on PC while building the mods so far. Addresses are offsets from the game's base address (RVAs), since `wait_for_game()` gives you the base. Everything here was found in the one build everyone plays; treat it as a starting point and check before you patch.

## The build and Azimuth

Everyone plays the Xbox One release `forza_durango_release_final.exe`, built on 24 November 2015. Its PE header has timestamp `0x565501ED` and image size `0x03A18000`, which is how the platform recognises it in memory. Azimuth doesn't start it as a process. It maps the exe into `azimuth.exe` itself, at the preferred base `0x140000000`, when the player presses Play, and translates the Xbox APIs underneath it. So your mod, the game and Azimuth all share one process.

Azimuth's settings are in `data\azimuth.ini`, a UTF-16 file. The `[launcher]` section holds the player's name (`player_name`) and graphics settings, and `[title-1a96545b] folder=` names the game folder (usually `FH2`). That folder's `Mount` subfolder is what the game sees as `game:\`. `1A96545B` is FH2's title ID, and Fast & Furious is `5A9771A4`. Saves are under `data\titles\1A96545B\saves\player-1\User_...\revisions\<n>\data\`. Azimuth checks those files' hashes, and Extra Options can turn that check off, but mods should never need to touch saves.

## Files and Extra Options

The game reads its data from `game:\media`. Most of it is in zips, whose directories are listed in `media\zipmanifest.xml`; the game relies on those offsets, so don't edit the zips. [Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions) adds an override folder: a file at `media\X.zip\path\file` can be replaced by `mediaoverride\X\path\file`, and a loose file at `media\path\file` by `mediaoverride\path\file`. The priority is `mediaoverride`, then `mediapc`, then the original. Extra Options loads through `winmm.dll`, and its settings are in `FH2_ExtraOptions.ini` next to `azimuth.exe`. `Gamerpic.png` in the same folder is the player's avatar when `CustomGamerpic=1`.

Extra Options also patches game code: around 84 byte patches and 47 hooks, listed with their RVAs in `patches.json` in its repo. It applies them all at once, while Azimuth loads its TLS helper just after mapping the game, and only after checking that every one of those places looks as it expects. If any doesn't, it applies none of them. That's why the platform's hooks wait for `wait_for_code`, and refuse game code that no longer matches the exe on disk.

## Text

See [`string-tables.md`](string-tables.md). The title screen's "PRESS (A) TO START" is two strings in `Main.str`: `0xB197` (PRESS) and `0x18F0` (TO START). TO START is its own text box, centred vertically on the button, so the line can grow sideways only about as far as the screen edge.

## The game database

`media\Stripped\gamedbRC.slt` is a plain SQLite database, about 6 MB, with around 290 tables. The game opens it as `game:\media\Stripped\gamedbRC.slt` through its own file system, the one Extra Options mounts `mediaoverride` over, and reads it at startup. Tables worth knowing: `Data_Car` (every car, with `DisplayName`, `BaseCost`, `PerformanceIndex`, 0-60 times and handling values), `Data_Engine`, `Events`, `EventCars`, `CarMeets`, `AIPlayers` and `DriverModels`. Text columns hold string-table references like `_&2149394699` rather than text, so names and descriptions are changed through the string tables. The exe also mentions `media\db\gamedb.slt`, which isn't in the shipped game.

## Drivatars

The built-in Drivatars are in `media\DrivatarGlobalCache\DefaultCache.bin`. The file starts with `u32 4` (version) and `u32 125` (count), followed by 125 records of `0x2658` bytes each. They use only 25 different gamertags, five records each. A record's gamertag is UTF-16 in its first 32 bytes (15 characters and a NUL), and the Drivatar's id, 1 to 25 and the same for all five copies, is the `u32` at `+0x2650`. The game reads the file once at startup (code at `0xA2D0E0`) into a manager object, with the record count at `+0x4D58` and a `std::vector` of records at `+0x4D78`. WheresMyBuddy replaces it through `mediaoverride`.

A championship copies the Drivatars it picked into the save, as `SaveStates::ChampionshipDrivatarsState`. The Storm Island championships appear to use the same class (saved as `IslandChampionshipDrivatarsState`). Its vtable is at `0x2D44F20`, and slot 2 is `Serialize(archive)` at `0xA68870`. The record count is at `+0x1C` of the object and the `std::vector` of records at `+0x20`. Records saved there are byte-for-byte copies of `DefaultCache.bin` records, id included. WheresMyBuddy swaps that vtable slot to rename them after loading.

Car meets and some road-trip traffic aren't Drivatars. They're named from `AIPlayers.str`, the 25 gamertags plus "HRZN Ben", through the game database's `AIPlayers` table (`SELECT Name, DriverModelId FROM AIPlayers WHERE Id=%d`).

## Radio

FMOD Studio 1.x is linked into the game, and its low-level API is exported from the exe by name, for example `FMOD::System::createSound` at `0x1997740`, `FMOD::Sound::getSubSound` at `0x199F510` and `FMOD::Sound::getUserData` at `0x199FBE0`. Look them up through the export table rather than hard-coding them. The radio is the `RadioStreamFmod` class (vtable `0x2E76220`), which plays songs from FMOD sound banks. FH2 Universal Radio redirects its `createSound` call at `0x1018A13` and its `getSubSound` call at `0x10221F2` to hand the game a stream of its own. Song samples are one of four classes, with vtables `MusicSample 0x2E74EF8`, `DJSample 0x2E76570`, `IdentSample 0x2E76590` and `RadioSample 0x2E74ED8`. Each has a pointer to its properties at `+0x08`: sound name at `+0x00`, event name `+0x20`, display name `+0x40`, artist `+0x60`, length in ms `+0x8C`, markers `+0x90`. The radio station list is in `RadioInfo_EN.xml`.

## Finding more

Most of this came from the exe's own strings and RTTI. The exe keeps MSVC type names like `.?AVChampionshipDrivatarsState@SaveStates@@`, which lead to vtables. SQL in the exe shows which tables of the game database it reads, and how. When you find something, a pull request adding it here helps the next person.

# Notes on the game

What's been worked out about Forza Horizon 2 on PC while building the mods so far. Addresses are offsets from the game's base address (RVAs), since `wait_for_game()` gives you the base. Everything here was found in the one build everyone plays; treat it as a starting point and check before you patch.

## The build and Azimuth

Everyone plays the Xbox One release `forza_durango_release_final.exe`, built on 24 November 2015. Its PE header has timestamp `0x565501ED` and image size `0x03A18000`, which is how the platform recognises it in memory. Azimuth doesn't start it as a process. It maps the exe into `azimuth.exe` itself, at the preferred base `0x140000000`, when the player presses Play, and translates the Xbox APIs underneath it. Under Proton it can't have that address, so it maps the exe elsewhere (players have seen `0x14690000`, `0x18940000` and `0x19D40000`) and moves its absolute pointers to match: `game_base` gives the real address, and a check of pointers against the exe on disk has to allow for the move ([linux.md](linux.md)). So your mod, the game and Azimuth all share one process.

Azimuth's settings are in `data\azimuth.ini`, a UTF-16 file (see [below](#several-games-azimuth-101) for Azimuth 1.0.1, which runs other Forza games too). The `[launcher]` section holds the player's name (`player_name`) and graphics settings, and `[title-1a96545b] folder=` names the game folder (usually `FH2`). That folder's `Mount` subfolder is what the game sees as `game:\`. `1A96545B` is FH2's title ID, and Fast & Furious is `5A9771A4`. Saves are under `data\titles\1A96545B\saves\player-1\User_...\revisions\<n>\data\`. Azimuth checks those files' hashes, and Extra Options can turn that check off, but mods should never need to touch saves.

## Files and Extra Options

The game reads its data from `game:\media`. Most of it is in zips, whose directories are listed in `media\zipmanifest.xml`; the game relies on those offsets, so don't edit the zips. [Extra Options](https://github.com/RokkuDayo/forzahorizon2_extraoptions) adds an override folder: a file at `media\X.zip\path\file` can be replaced by `mediaoverride\X\path\file`, and a loose file at `media\path\file` by `mediaoverride\path\file`. The priority is `mediaoverride`, then `mediapc`, then the original. Extra Options loads through `winmm.dll`, and its settings are in `FH2_ExtraOptions.ini` next to `azimuth.exe`. `Gamerpic.png` in the same folder is the player's avatar when `CustomGamerpic=1`.

### Several games (Azimuth 1.0.1)

Azimuth 1.0.1 runs several Forza games from one launcher, inside one `azimuth.exe` process: Forza Horizon 2, Forza Motorsport 5 and 6, and others. Each has a `[title-<id>]` section in `azimuth.ini` with its `folder=` and `name=`, and `[launcher] game=` is the one picked last. Every game keeps its own `data\titles\<ID>\`. The launcher is part of the process, so `d3dcompiler_47.dll`, and with it the platform and every mod, loads when `azimuth.exe` starts, before the player picks a game; the game is only mapped when they press Play. The process ends with the game ("session ended: the game window was closed"), so it's one game per launch.

All of them ship an exe called `forza_durango_release_final.exe`, with the same preferred base, `0x140000000`, where Azimuth maps it by hand (it isn't a loaded module). Only the PE header tells them apart:

| Game | Title | TimeDateStamp | SizeOfImage |
|---|---|---|---|
| Forza Horizon 2 | `1a96545b` | `0x565501ED` (24 Nov 2015) | `0x03A18000` |
| Forza Motorsport 5 | `7b35d917` | `0x53D8D1F2` (30 Jul 2014) | `0x0279D000` |
| Forza Motorsport 6 | `64f9397e` | `0x58AFB1CD` (24 Feb 2017) | `0x01FC8000` |

`azimuth.exe` 1.0.1 itself also has `0x140000000` as its preferred base, but it's relocated, so the game gets that address. From 2.8.0 the platform watches that address, and once a game is mapped there that isn't FH2's 24 Nov 2015 exe, it names it from `azimuth.ini` (by comparing the header with each title's exe on disk) and leaves it alone: nothing is hooked or patched in it, `game_base`, `wait_for_game` and `wait_for_code` return null, mods from before the platform aren't loaded, and the platform stops searching memory for FH2. A header has to stay the same for 50 ms before it counts, so a read while Azimuth is still copying it in can't. Azimuth can also be told which game to start: `--skip-launcher` starts the one `[launcher] game=` names, and `--game=DIR` (a scripted run) the one in `DIR`. When that isn't FH2, the platform loads no mods at all; when `DIR` holds FH2's exe, that's the folder the platform writes `mediaoverride` to.

### The override folder

The game finds every file through its file system's resolver, which a vtable slot at game+0x2E6EF28 points to (game+0x11F49B0). The file system is a list of volumes, each mounted at a path with a priority; the game's own zips and folders are volumes under `game:\media\`. The game also has a folder volume class (constructor game+0x11E05A0, vtable game+0x2EAB918, its RTTI locator just before it) that serves the files in one folder. Mounting two of those, for `game:\mediaoverride\` and `game:\mediapc\`, over `game:\media\` at priorities above everything else (0x7FFFFF00 and 0x7FFFFEFF; up to 2.8.0, 0x7FFFFFFE and 0x7FFFFFFD) is what makes `mediaoverride` work: the file system's vtable +0x1E8 mounts a volume (no hash-manifest check when its last three arguments are 0) and +0x1F8 sorts the volumes again. The folder volume's "does this file exist" method (+0xE8) is handed archive selectors such as `<*>0` too, which aren't file names, so they have to be answered "no".

Extra Options does this from its `winmm.dll`. Azimuth 1.0.1 runs without Extra Options for now, so from 2.7.3 the platform does the same when Extra Options' `winmm.dll` isn't loaded: it swaps the resolver pointer as soon as Azimuth maps the game (before the game's code is executable) and mounts both folders on the first lookup. `platform.log` says so ("media overrides: ..."). It checks the game's bytes against the exe on disk first and leaves everything alone if they differ. When Extra Options is loaded the platform doesn't touch the resolver: Extra Options refuses all of its patches if any place it patches was changed first, and its `EnableMediaOverrides` setting decides. Azimuth 1.0.1's log lists the folder under "modifications" (`the game folder's Mount\mediaoverride (loose files)`); that's only a note of what's in the game folder, not Azimuth reading it.

Extra Options also patches game code: around 84 byte patches and 47 hooks, listed with their RVAs in `patches.json` in its repo. It applies them all at once, while Azimuth loads its TLS helper just after mapping the game, and only after checking that every one of those places looks as it expects. If any doesn't, it applies none of them. That's why the platform's hooks wait for `wait_for_code`, and refuse game code that no longer matches the exe on disk.

### Azimuth's drives and this folder's layer

The game reads files only through the drives Azimuth gives it. `game:\` is the game folder's `Mount`. A `game:\` path with `..` in it stops the game ("Invalid runtime path component", raised from the folder volume's `GetFileAttributesExA`), and a host path such as `D:\Games\...` isn't found: the lookup just fails. `T:\`, where the game keeps `T:\Turn10Temp.scratch`, is FH2's temporary drive, which Azimuth 1.0 keeps in its own folder as `data\titles\1A96545B\tmp` and doesn't empty between starts. A folder volume for a folder under `T:\` works as the one for `game:\mediaoverride\` does, loose files in it standing for zip entries the same way, and only that Azimuth folder's game sees it.

So from 2.9.0, on Azimuth 1.0 or later without Extra Options, the platform mounts `T:\fh2-platform\media\` (its layer, priority 0x7FFFFFFE), then each car pack's `T:\fh2-platform\packs\<pack>\` (from 0x7FFFFFF0 down), then `mediaoverride` and `mediapc`, and writes to its layer what it used to write to `mediaoverride` ([car-packs.md](car-packs.md#this-azimuth-folders-own-files)).

## Text

See [`string-tables.md`](string-tables.md). The title screen's "PRESS (A) TO START" is two strings in `Main.str`: `0xB197` (PRESS) and `0x18F0` (TO START). TO START is its own text box, centred vertically on the button, so the line can grow sideways only about as far as the screen edge.

## The game database

`media\Stripped\gamedbRC.slt` is a plain SQLite database, about 6 MB, with around 290 tables. The game opens it as `game:\media\Stripped\gamedbRC.slt` through its own file system, the one `mediaoverride` is mounted over, and reads it at startup. Tables worth knowing: `Data_Car` (every car, with `DisplayName`, `BaseCost`, `PerformanceIndex`, 0-60 times and handling values), `Data_Engine`, `Events`, `EventCars`, `CarMeets`, `AIPlayers` and `DriverModels`. Text columns hold string-table references like `_&2149394699` rather than text, so names and descriptions are changed through the string tables. The exe also mentions `media\db\gamedb.slt`, which isn't in the shipped game.

## Cars

A car is a `Data_Car` row and a zip, `media\cars\<MediaName>.zip`. The game's car Ids go up to 2501 (FM5's to 2169). The tables keyed by `Ordinal` or `CarId` hold a car's upgrades (`List_Upgrade*`), suspension and anti-roll bar physics, colours (`Combo_Colors`), camera offsets and part positions. Their own ids are `Id * 1000 + n` (2155000 and up for car 2155), except `Combo_Colors`', and so is the car's body (`List_UpgradeCarBody.CarBodyID`), whose part tables are keyed by `CarBodyID`. Engines and drivetrains are shared between cars; an engine's `Data_Engine.MediaName` names its sound, `media\audio\fmodanthem\durango\PG_<name>.bank`. `Data_Car_Buckets` puts a car in an Autoshow group, and a new car works with `ContentId` 0.

`DisplayName` (the model, "Mustang GT") and `ModelShort` ("Ford Mustang '15") refer to `Data_Car.str` ([string-tables.md](string-tables.md#keys)). The Autoshow shows a `DisplayName` given as text, but the top bar (year, make, `DisplayName`) shows only names from the string tables. `Thumbnail` is `game:\Media\Stripped\RC<n>\Thumbnail_<Id>.png`, which the game reads as `RC<n>.zip`'s `Thumbnail_<Id>_Big.swatchbin` and `_Small.swatchbin`. Loose files at that path in an override folder are read too, also under a name the zip doesn't have.

### Car zips

A car's zip holds its `<MediaName>.carbin`, its models (`scene\...\*.modelbin`), materials, shaders, textures (`*.swatchbin`) and lights. Their paths name the car in lower case: `game:\media\cars\for_mustanggt_15\scene\...`. Entries are in upper-case name order, local headers have no extra field, and each entry's central-directory record has a 4-byte extra field `0x1123` with the offset of the entry's data. The game finds each zip's directory from `media\zipmanifest.xml` (UTF-8 with a BOM, CRLF), one line per zip:

```xml
<Zip version="1" path="game:\media\cars\for_mustanggt_15.zip" priority="80" dirstart="19831020" dirsize="67706" direntries="548" />
```

`dirstart` is the central directory's offset, `dirsize` its size to the end of the file (the end record included), and `direntries` the number of entries. The game takes those as they are, so a changed zip needs a changed line.

### Thumbnails

A `.swatchbin` is a Grub bundle (`burG`) with one `TXCB` texture block: its header at `0x28` (`TXCH`, with the width and height as u16s at `+24`) and its data at `0x53`. A car's thumbnails are BC7, one mip, 816 x 459 (`_Big`) and 192 x 108 (`_Small`), padded to whole macro tiles of 64 x 32 blocks (1024 x 512 and 256 x 128 pixels), and tiled as the Xbox One GPU tiles them (2D thin): 4 pipes (`P4_16x16`), 8 banks of width and height 1, macro tile aspect 2, a 256-byte pipe interleave. With `xN` for bit `N` of a block's x (in blocks) and so on:

```
t    = 16 * (x0 | y0 << 1 | x1 << 2 | y1 << 3 | x2 << 4 | y2 << 5)     place in its 8 x 8 micro tile
       + 1024 * (macro tiles before it, in row order)
pipe = (x3 ^ y3 ^ x4) | (x4 ^ y4) << 1
bank = (tx0 ^ ty2) | (tx1 ^ ty1 ^ ty2) << 1 | (tx2 ^ ty0) << 2           tx = x / 32, ty = y / 8
the block's offset = (t & 255) | pipe << 8 | bank << 10 | (t >> 8) << 13
```

The game's own thumbnails decode exactly so. Other textures, with mips, haven't been checked.

### Rows the game trusts

The game reads its database once at start and uses what it finds without checking, so a row a car needs that isn't there usually stops the game rather than showing an error. Two such places are known:

- **The drivetrain in the menus (`DriveTrainConverter`, `0xB99BE0`, slot 0 of its vtable `0x2DA6938`).** The menus' XAML converter for a car's drivetrain takes the car's `Data_Car.PowertrainID` and looks it up in the car database cache's `Powertrains` table (`CCarDBCache`, at `[0x34ABA60]+0x3A0`: a vector of record pointers, one per Id, from `+0x68` to `+0x70`, read by `0x26F900`). With the parameter `text` it shows the drive type's name (the record's byte `+1`, a `List_DriveType` Id: FWD, RWD, AWD); with any other parameter, the icon's path (`+0x30`, `IconPath`), and as an image, the icon. The lookup returns NULL for an Id past the end, and reads one record past the end for an Id equal to the count (it compares with `jbe`); every use of the record is unchecked, so an Id the table doesn't have stops the game at `0xB99CD3` (reading `0x1`), `0xB99D35` or `0xB99D9E` as soon as the Autoshow or a garage lists that car (issue #23). The stock game's Powertrains Ids are 0 to 7.
  - Before any lookup the converter asks whether it can make the type the binding wants: a string (`0x154F960` at `0xB99C9B`, tested at `0xB99CA0`), else the image type (tested at `0xB99D74`). For neither it goes to `0xB99E37`: it releases its parameter (AddRef'd at `0xB99C8A`) and returns the game's empty value (`0x160C340`, a static object, AddRef'd). Nothing else is held there.
  - The platform's database guard (2.9.0, `src/db_guards.cpp`) hooks both tests and answers "no" when the Id isn't in the table, so the car is listed without its drivetrain. It goes in only when the game reads a database that isn't its own, and only when those bytes are as on disk. `tests/hook_test.cpp` runs the converter natively to show the references come out as the game's own way out leaves them.
- **A car's physics with a saved upgrade that's gone (`0x105BEEC` in `0x105BEA0`).** The physics builder (`0x1056210`) makes a record for each of the car's 43 parts from the saved configuration's part Ids (or the stock ones), through five loops (`Create*PartUpgradeRecord`, `0x10482F0`, `0x1048990`, `0x1048640`, `0x1047FA0` and `0x1048CE0`). A part Id with no row, or whose owner column isn't the car's, gives a NULL record, and the readers use it unchecked: the clutch's (`0x105BEA0`, reading `ClutchMaxTorque`, `ClutchInTime` and `ClutchOutTime` into the physics at `+0x49C`) faults at `0x105BEEC`, and the transmission's (`0x10645C0`) the same way at `0x1064614` right after. There's no guard for this one:
  - a stand-in record would only move the crash to the next reader, or give the car made-up clutch and gear numbers (a record set with no row reads every number as `FLT_MAX`, which is no clutch a car can drive with);
  - the safe fallback is the stock part's record, which needs the game's own record and SQL calls (`GetStockPartId` `0x104D5F0`, then the `Create*PartUpgradeRecord` for that Id) made from a hook on the car-loading thread, or the saved Ids checked against the database before the loops (the 43 Ids are at the builder's `rbp+0x928` by `0x10567F3`). Neither can be shown to be safe without the game.
  - FH2MP guards a related case, three optional torque record sets (`0x105E530`), by handing in an empty record set: those readers check the row count, the clutch's doesn't.

## Drivatars

The built-in Drivatars are in `media\DrivatarGlobalCache\DefaultCache.bin`. The file starts with `u32 4` (version) and `u32 125` (count), followed by 125 records of `0x2658` bytes each. They use only 25 different gamertags, five records each. A record's gamertag is UTF-16 in its first 32 bytes (15 characters and a NUL), and the Drivatar's id, 1 to 25 and the same for all five copies, is the `u32` at `+0x2650`. The game reads the file once at startup (code at `0xA2D0E0`) into a manager object, with the record count at `+0x4D58` and a `std::vector` of records at `+0x4D78`. WheresMyBuddy replaces it through `mediaoverride`.

A championship copies the Drivatars it picked into the save, as `SaveStates::ChampionshipDrivatarsState`. The Storm Island championships appear to use the same class (saved as `IslandChampionshipDrivatarsState`). Its vtable is at `0x2D44F20`, and slot 2 is `Serialize(archive)` at `0xA68870`. The record count is at `+0x1C` of the object and the `std::vector` of records at `+0x20`. Records saved there are byte-for-byte copies of `DefaultCache.bin` records, id included. WheresMyBuddy swaps that vtable slot to rename them after loading.

Car meets and some road-trip traffic aren't Drivatars. They're named from `AIPlayers.str`, the 25 gamertags plus "HRZN Ben", through the game database's `AIPlayers` table (`SELECT Name, DriverModelId FROM AIPlayers WHERE Id=%d`).

## Radio

FMOD Studio 1.x is linked into the game, and its low-level API is exported from the exe by name, for example `FMOD::System::createSound` at `0x1997740`, `FMOD::Sound::getSubSound` at `0x199F510` and `FMOD::Sound::getUserData` at `0x199FBE0`. Look them up through the export table rather than hard-coding them. The radio is the `RadioStreamFmod` class (vtable `0x2E76220`), which plays songs from FMOD sound banks. FH2 Universal Radio redirects its `createSound` call at `0x1018A13` and its `getSubSound` call at `0x10221F2` to hand the game a stream of its own. Song samples are one of four classes, with vtables `MusicSample 0x2E74EF8`, `DJSample 0x2E76570`, `IdentSample 0x2E76590` and `RadioSample 0x2E74ED8`. Each has a pointer to its properties at `+0x08`: sound name at `+0x00`, event name `+0x20`, display name `+0x40`, artist `+0x60`, length in ms `+0x8C`, markers `+0x90`. The radio station list is in `RadioInfo_EN.xml`.

## Menus

The front end is XAML pages in `media\UI.zip` (a plain zip), drawn by AVUI, a C++ take on WPF with `Binding`, styles, templates and converters. Pages are under `Scenes/Anthem/`, shared styles under `Resources/Anthem/`. Each page is a C++ class too, which binds the XAML's `PART_` elements and supplies the properties its bindings ask for. Which page opens when is decided by the state machine in `media\stateflow.zip` (`ui_screens.xml` and friends). Pause > Settings is `A_PauseOptions.xaml`, class `PauseOptionsMenu`, a 5 x 2 grid of tiles. Extra Options adds rows of its own to `A_HUDOptions.xaml` and `A_VideoOptions.xaml`, recognising them by their `x:Class`.

What the platform's menus (the MODS tab, each mod's page) use, all in the 24 November 2015 build:

- **Properties.** Bindings look properties up by name with `AVUI::Type::FindProperty(type, name)` at `0x157FF60`. It walks the list at `type+0x40` (nodes `{descriptor, next}`), comparing the descriptor's `char*` name at `+0x40` with the AVUI string's UTF-16 text (at `+0x28`), then tries the base type at `type+0x80`. A descriptor has its getter at `+0x28`, called `(out, desc, object)`, which boxes the value into `*out` (ints with `0x4702D0`, bools with `0x336BA0`); its setter at `+0x30`, `(desc, object, value)`, with an int value at `value+0x28`; and its value type at `+0x48` (bool is `0x15C9550(0)`). The HUD page's `EnableDrivatarLiveries` (`0xD58660` returns it) is a typical On/Off one. A page tells its bindings a property changed with `0xCF9C60(page, desc)`. Float properties, which the game's `Slider` binds its `Value` to (the audio volumes, the controller dead zones), have the same layout with a float at `value+0x28`; `0xED6E90` returns one, and `0x3F5990` boxes a float. The slider's look is `A_Style_Slider_Options` in `Resources/Anthem/Shared_OptionsScreens.xaml`, whose number is a `NumUnitConverter` (`FractionDigits`, `ShowUnit`, `Scale`; `ConverterParameter=fracpercentage` shows 0..1 as 0..100). Extra Options' drift camera sliders on the HUD page are built the same way.
- **The Settings page.** It's made once while the game boots and reused. `0xDC8A60` wires its tiles with `0xD9FC10(page, "PART_Button_X", &pmf)`, where `pmf` is `{handler, 0}` and a name that isn't on the page is skipped. Its controller grid is a fixed 5 x 2 (set at `0xDC873E`): the navigation manager is at `page+0x410`, with the column count at `+0x9C`, and the tile grid is rebuilt from that each time the page is wired. A tile's handler (HUD's is `0xDC6C40`) fires a state-machine event.
- **Opening a page.** A page is made from a scene record, `{id, name, L"Anthem\\A_X" (std::wstring at +0x28), factory at +0x48}`. The HUD page's is built by `0xB3A3E0` (id `0x49`). Opening it builds the record twice, once to look it up and once inside the factory (`0xB0B990`), and the factory's copy goes into the new page at `+0x228`; that's the one its XAML is loaded from. Changing its path makes the same page class load a different file, and new files in `mediaoverride\UI\` are found like replaced ones.
- **Message boxes.** `0xD09D00(page, &title, &message, items)` shows `A_MessageBox.xaml` over `page` and returns it. Title and message are references to `CLocString`s (vtable `0x2EB0E30`, text from the virtual at `+0x38`). The items are a collection made by `0xC65230`, holding items made by `0xC65AB0(&item, &id, &label)` (id at `+0x68`) and added through the collection's vtable `+0x40` (collection, index, item). The popup takes the collection's interface at `+0x38`. `0xDD8820(popup, 6)` sets its help bar. `0xDD86F0(popup, 2, std::function<void()>, true)` sets what happens when an item is chosen; the function is the MSVC layout (`{space[0x18], impl*}`) and taken by value. `0xE01A60(popup)` is the chosen item.

- **The MODS tab's pages.** Every page of the MODS tab, and every mod's page, is the HUD options page loading another file: the Marketplace tiles' Car Packs handler (`0xDBD720`) fires the `car_packs` event, which the platform's `pause.xml` sends to a node showing the HUD page, and the scene record is pointed at `A_ModPage_<nn>`. A page leaves only by `back`, to `pause_menu.enterback`. The files (platform 2.9.0): `A_ModPage_01` is YOUR MODS and holds every view at once (the list, each mod's page, each mod's own pages), switched with `FH2Mod_View_<k>` properties; `02` is UPDATES, `04` ABOUT, `06` HELP; `80` and up are mods' pages on game tiles. `A_ModOptions` is only the list that stands in for the tab when its hooks don't go in.
- **The Settings door.** Pause > Settings > MODS runs the HUD tile's handler (`0xDC6C40`) with the scene record pointed at `A_ModPage_01`, so the same YOUR MODS opens from the Settings page and B comes back there; the Settings page's wiring hook (`0xDC8A60`) asks about a restart as the player comes back, as the pause menu's (`0xDC6E30`) does for the tab.
- **Finding a tile.** The pause menu finds each tile to wire it with `0xCBDB20(String** name, page)` (called at `0xD9FA3A`), which returns the `Button` or null. The platform asks it for `PART_Button_FH2Mod_Installed` once a session: when that isn't there, the game isn't reading the platform's files, and the game's message box (above), whose text comes from code, says so.
- **The Achievements tiles.** `PART_Button_Achievements` (handler `0xDBD610`, wired at `0xDC7E5D`) and the PORSCHE tab's `PART_Button_Panama_Achievements` (`0xDBF150`, which first checks the tile has the focus with `0xDC17D0`) both call `0xCB47B0(page, &out, 0xFE)`, a UI command that opened the console's achievements app, at `0xDBD635` and `0xDBF185`. A page on that tile hooks those two calls.
- **Tile text.** The pause menu's icon tiles use `A_ControlTemplate_IconTile` (`Shared_TiledMenus.xaml`): the icon, the title, and a notification badge from `local:BindingProxy.A` (bound to the page's `NotificationBadge...` properties). A tile's `Tag` takes a binding too (the Marketplace tab's badge is one), so a copy of that template with one more `FZTextBlock` bound to the tile's `Tag` shows a line of its own; the pause menu's resources have `A_Style_FZTextBlock_TileClubMemberCount`, the club tile's orange count, before the menu items. Bindings on the pause menu page find the platform's properties through the same `FindProperty` hook as the options pages', from inside a template too (`RelativeSource FindAncestor, AncestorType={x:Type Page}`, as the game's own templates do). A `Color` property is typed and boxed as the game's `TeamColor` (its descriptor at `0x140C09BD0`): type `0x1415C8D80(0)`, value boxed by `0x140BEA4E0(out, const float rgba[4])`, red, green, blue and alpha from 0 to 1 (a packed `0xAARRGGBB` is unpacked that way at `0x14195B21A`). The wristbands' colours are in `media\GameTunableSettings.zip`'s `PlayerXPLevels.xml`, as signed ARGB ints: `ColourNum` for the band, `TextColour` for text on it.
- **The pad in the menus.** Pages get the pad as keys, WPF's numbering, from a table at `0x142254390` (game control pressed, released, key): A is Space (18), B is B (45), X is X (67), Y is Y (68), the d-pad the arrows (23-26) and the number pad's 8/2/4/6 together, LB PageUp (19) and RB PageDown (20). The table also has the shifts (116, 117) and the controls (118, 119), which aren't the bumpers. On the HUD options page they reach the button handler at vtable `+0x578` (`0xCEC3D0`).
- **The title screen.** It's `Scenes/Anthem/A_Splash.xaml`, class `Splash` (vtable `0x142E533C8`). "PRESS (A) TO START" is the `HelpButton`, and the platform's notices are its text. Its button handler is the same shared `0xCEC3D0`, in the vtable slot at `0x142E53940` (`+0x578`), and only A (18) is registered for it (at `0xE8F9A5`). A calls `Splash::OnStart` (`0xEA0D40`), which signs in, loads the profile and moves on. The page's state is at `page+0x448`: 0 waiting for A, 1 A pressed, 2 a popup over it, 3 and 4 moving on (`OnStart` checks 3 and 4 at `+0x27`). The platform's title-screen entries hook that slot: another button in state 0 runs the mod's code, and calling the original with A, pressed, starts the game as the player would. The entries' lines fade out with the `HelpButton` by copying its storyboards' animations onto them. A list entry shows the message box (see Message boxes) over `Splash` itself; picking an item presses A the same way.

## The free-roam HUD

The HUD you drive with is `Scenes/Anthem/A_Hud.xaml`, class `Hud` (vtable `0x2DEF9C8`, `.?AVHud@@`). The platform's "a mod didn't load" notice goes into it, and so does the HUD layer preview ([`hud-layer-preview.md`](hud-layer-preview.md)).

- **The page.** One `Hud` page was built per session in every test, while the game loads (its bindings were resolved about 15 s before it was first shown), and it stays while menus come and go. Its vtable's `+0x488` (`0xD38DD0`, slot `0x2DEFE50`) runs each time it's shown and sets `page+0x410` to 1 (at `0xD38DF2`); `+0x4F8` (`0xD41540`, slot `0x2DEFEC0`) runs when it's hidden and sets it back to 0 (at `0xD4156B`); slot 0 is its deleting destructor (`0xD244C0`). Its `DataContext` is the page itself, so the platform's `FindProperty` hook (see Menus) answers its bindings, and getters are called with the page as the object.
- **Other pages in its place.** The pause menu, the map, photo mode and the rewind each put a page of their own in its place: the `Hud` page is hidden, not destroyed, and shown again after. With the telemetry HUD turned on, `ui_screens.xml`'s `show_hud_and_continue` shows `UIPageTelemetry` (`A_Telemetry.xaml`, class `Telemetry`, vtable `0x2E555E8`) instead, so the `Hud` page isn't shown at all.
- **The LimitedHud pages.** `LimitedHud` (vtable `0x2DF3F28`) and `LimitedHudMinorNotification` (`0x2DF4798`) are cut-down HUD pages for the stateflow's limited HUD states (`UIPageLimitedHudMinorNotification`, `UIPageLimitedHudRadioIntro`, `UIPageLimitedHudANNAIntro`). The exe has only one `Anthem\A_Hud` page path, so they're expected to load the same file, and a getter for a property bound in it can be called with one of them as the object: check the object's vtable if something should only show on the free-roam HUD. None was seen in testing.
- **The suspend.** The speedometer, map, prompts and notifications are statuses of two of the world's modules, `HUDModule` (vtable `0x2CDC7E0`) and `HUDModuleAR` (`0x2CDC890`, the prompts and markers in the world), in the world's module list (WorldManager `*(game+0x34C0F40)`, world `[wm+8]`, list at `world+0xF8`). Each reaches a `HUDStatusPrioritiserModule` (vtable `0x2CDC5E8`) through a weak pointer (`module+0x68`, control block `+0x70`); in testing both shared one. `0x78B0F0(HUDModule*)` and `0x78B1A0(HUDModuleAR*)` suspend: each hides what its prioritiser shows (`0x77A430`) and sets the prioritiser's byte `+0xA8` to 1 (at `0x78B161` and `0x78B211`). `0x785CD0(module)` resumes, clearing it (at `0x785CE7`). The game does both itself (stateflow's `global.suspend_all_huds` and `resume_all_huds`: the rewind and `in_game_ui.xml`'s `prepare_for_driving` end with a resume), and the prioritiser's vtable `+0x10` (`0x7813D0`) clears the flag too (at `0x7813E5`), so a mod that keeps the HUD hidden has to suspend again whenever the flag reads 0. The Drivatars' name tags aren't statuses and stay.
- **The prioritiser.** While `+0xA8` is set, its update lets a new status through only if the status's byte `+0x110` is set (the check is at `0x77B9E2`); the others wait. In testing that held back small notifications and big banners (one for over two minutes) and the autosave spinner, which showed after the resume, while a Rivals status was dropped, the in-world prompts were gone, and waiting skill statuses flashed for about a frame each. A resume brings the speedometer and map back in one frame.

## HUD notifications

The small notification at the bottom of the driving view (a road discovered, an achievement) is a `HUDMinorNotificationComponent`. The platform's pop-ups and tips (API 10) use it as Achievements does: Show (`0x755330`) on a live component, with display strings from MakeDisplayString (`0x323310`), called from the AchievementsModule's tick (`0x3FCD84`).

- **The option.** The game's Notifications setting (Pause > Settings > HUD) is `Options/MinorNotifications` in the player's profile (`CForzaProfile`, at `CPlayer+0x328`), an `Int32` defaulting to 1. The minor-notification gate `0x754920` reads it when a notification is sent, not when it appears: above 0 it shows; otherwise only while the scene `LimitedHudMinorNotification` (`0x53`) is on top. It doesn't cover the big banners at the top.
- **The gate's callers.** Show calls the gate at `0x75542C`; a sibling of Show (`0x7554B0`, road discoveries, photo collection and rewards) at `0x7555A5`; the component's reflected `Show` method (`0x750E60`) at `0x750FB1`. When the gate lets a notification through it stores a new object at the component's `+0x198`; when it doesn't, that stays as it was. The platform's pop-ups hook all three calls to see every notification shown.
- **The chime.** The control plays `HZ2_SFX_HUD_Notification_GenericOn_01` itself each time it shows one, so a pop-up needs no sound of its own.
- **Text.** MakeDisplayString converts with the Windows code page (`MultiByteToWideChar` with code page 0), so text beyond ASCII can come out garbled; its wide sibling `0x1239840` is a constructor run in place, not a drop-in.

### Code released mods check in memory

A mod that compares game code in memory switches its feature off when another mod has patched it. These are the ones known, so new hooks keep clear of them (the platform's do, by at least `0xEE` bytes):

| Mod | Checks |
|---|---|
| Achievements 1.0.x | Show `0x755330` and MakeDisplayString `0x323310`, 15 bytes each, once, at its first pop-up, with no fallback to the exe on disk |
| FH2MP 1.2.0 | MakeDisplayString `0x323310`, 16 bytes, and about 85 other places, none near the platform's |
| WreckingBall 1.0.0 to 1.3.0 | the code it hooks (`0x12CC730`, `0x1377B50`, `0x384C00` among them) and six vtable slots; 1.3.3 falls back to the exe on disk |
| CCBR 1.0.0 | car code (`0x289320`, `0x28C590`, `0x28D930`, `0xF5B420`-`0xF73080`, `0x190BF90`, `0x190BFD0`) and six `lea rdx` sites, 7 bytes each, from `0x6A2AF7` to `0x6B992A` |
| Universal Radio 1.0.0 to 1.7.1 | operator new's prologue `0x3229C0`, the PE headers and the FMOD exports; its two radio call sites (`0x1018A13`, `0x10221F2`) only when it runs without the platform |
| WheresMyBuddy 1.7.0 | the `ChampionshipDrivatarsState` vtable slot `0x2D44F30` |

So Show's and MakeDisplayString's first 16 bytes must stay unpatched: call them, don't hook them. Achievements checks 15 bytes of each, and FH2MP all 16 of MakeDisplayString; a hook at either function's start would change all of them. The 2.9.0 platform's sites were checked against every range above offline, with all of them in at once and every released mod's own hooks too, in both orders: none is refused and every check still matches. CommunityGarage 1.2.1, which calls Show and hooks the same three notification sites as Achievements, couldn't be checked that way, since neither its source nor its DLL was at hand.

## Sites the platform hooks

Every site below is the platform's own: don't hook it, patch it or check its bytes in memory, and use the API instead (settings, pages, title-screen entries, pop-ups, the HUD and pad layers). `include/fh2game.h` has the same list. Each one is checked against the exe on disk before it goes in, and turned off with a log line when it doesn't match. Instruction hooks at the same address chain, so the three sites Achievements also hooks (the tick and the notification's constructor and destructor) are shared rather than refused.

| Site | What | Kind | In when | Since |
|---|---|---|---|---|
| `0x157FF60` | `AVUI::Type::FindProperty` | function | every session the menus' code matches | 2.3.0 |
| `0xDC8A60` | the Settings page wiring its tiles (calls `0xD9FC10`, and the HUD tile `0xDC6C40`) | function | the same | 2.3.0 |
| `0xB3A3E0` | the HUD options page's scene record (its factory `0xB0B990` makes the page from it) | function | the same | 2.3.0 |
| `0x2DF06C0`, `0x2DF07B0` | the HUD options page's vtable `+0x488` (loaded) and `+0x578` (buttons) | pointer | the MODS tab | 2.5.0 |
| `0xDC6E30` | the pause menu wiring its tiles (calls `0xD9F9F0`) | function | the MODS tab | 2.5.0 |
| `0xDBD720`, `0xDBDD00`, `0xDBDD60`, `0xDBFE40` | the Marketplace tab's tile handlers | function | the MODS tab | 2.5.0 |
| `0xDDACAD`, `0xDDAE1F`, `0xDDB2F5`, `0xDDB910`, `0xDDBB3E` | the code that hides the Marketplace tab | byte patch | the MODS tab | 2.5.0 |
| `0xDBD635`, `0xDBF185` | the Achievements tiles' calls | call | a mod's page takes the tile | 2.7.0 |
| `0x2E53940` | the title screen's buttons (`Splash` vtable `+0x578`) | pointer | a mod added a title-screen entry | 2.8.0 |
| `0x2DEFE50` | the free-roam HUD shown (`Hud` vtable `+0x488`) | pointer | the platform's notices on the HUD (the mods loaded, NEW MOD, a mod that didn't load) | 2.5.0 |
| `0x2E6EF28` | the file system's resolver slot, written directly | slot | Extra Options isn't loaded | 2.7.3 |
| `0x3FCD84` | the AchievementsModule's tick | instruction (shared) | a mod declared pop-ups or tips, or `Developer=1`; or a mod plays a UI sound from outside its callbacks; or a mod has a HUD layer | 2.9.0 |
| `0x73E0B1`, `0x73F046` | the HUD notification component's constructor and destructor | instruction (shared) | a mod declared pop-ups or tips, or `Developer=1` | 2.9.0 |
| `0x75542C`, `0x7555A5`, `0x750FB1` | the three calls of the minor-notification gate `0x754920`, in Show, its sibling and the reflected Show | call | the same | 2.9.0 |
| `0x2DEFE50`, `0x2DEFEC0`, `0x2DEF9C8` | the free-roam HUD's vtable `+0x488` (shown), `+0x4F8` (hidden) and slot 0 (its destructor) | pointer | the same, or a mod has a HUD layer | 2.9.0 |
| `0x10768D0` | the controller manager's "release taken controls", right after each frame's reading | function | a mod added a pad layer | 2.9.0 |
| `0xB99CA0`, `0xB99D74` | `DriveTrainConverter`'s two type checks (the database guard) | instruction | the game reads a database a car pack or mod changed | 2.9.0 |

"Since" is the release that first hooked the site. A 2.9.0 session without a mod that declares pop-ups or tips, plays UI sounds from outside its callbacks, or adds a HUD or pad layer, and without a database changed by a car pack or mod, hooks only what 2.8.0 hooked.

## Finding more

Most of this came from the exe's own strings and RTTI. The exe keeps MSVC type names like `.?AVChampionshipDrivatarsState@SaveStates@@`, which lead to vtables. SQL in the exe shows which tables of the game database it reads, and how. When you find something, a pull request adding it here helps the next person.

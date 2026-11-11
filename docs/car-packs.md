# Car packs

From 2.9.0 the platform loads car packs: new cars for the Autoshow, each pack in a folder of its own in `fh2-cars` next to `azimuth.exe`. There's no installer, nothing is written to the game folder, and two packs can't overwrite each other's files or database rows.

## What they need

Azimuth 1.0 or later. The platform then mounts its own folders ahead of the game's ([game-notes.md](game-notes.md#azimuths-drives-and-this-folders-layer)), which is what packs are read from. Packs are read with Extra Options loaded too ([With Extra Options](#with-extra-options) below). On an older Azimuth packs aren't read, and `fh2-mods\platform.log` says so. Mods work there as before. What the platform looks for, as it starts, is the `data\titles\1A96545B` folder Azimuth keeps the game's temporary drive in (Azimuth 0.9.10 has it too), with the `tmp\Turn10Temp.scratch` folder in it that the game makes on that drive, which shows the drive really is there. A fresh Azimuth folder only has them once the game has been started, so packs are read from the second start. The log says which one was missing ("media overrides: this Azimuth folder has no data\titles\1A96545B yet ...", or "... has data\titles\1A96545B, but no tmp\Turn10Temp.scratch in it ...").

## Installing and removing a pack

Put the pack's folder in `fh2-cars`, so that its `pack.ini` is at `fh2-cars\<pack>\pack.ini`, and start the game. Its cars are in the Autoshow under their make. `platform.log` lists each pack and the cars it adds, the Id each car got the first time ("FOR_MustangCL_15 is car 3000 from now on"), and why a pack was left out, if it was. The MODS tab's YOUR MODS lists the packs under the mods, with the cars each adds, and says in red when one isn't working; its About page has a row that opens `fh2-cars`.

To remove a pack, delete its folder. Its cars and files are gone from the next start. A car you bought is in your save by its Id, and what the game does with a save whose car has gone hasn't been tried yet, so keep a copy of your saves before removing a pack whose car you own. A car keeps its Id for good (see below), so putting the pack back gives it the same one.

### When a pack's cars are missing

A pack's cars can also be missing from the game's database without you removing anything: the pack is left out because a mod's SQL now takes one of its car's Ids, its own SQL fails after an update, the packs aren't read at all (with Extra Options, when one of the checks below fails), or the platform has gone back to an older version. The platform checks at every start, before the game reads your save, that every car it has given an Id to (`fh2-mods\platform-car-ids.txt`) is in the database the game reads. When one isn't:

- the first start it's missing, your saves are copied to `fh2-mods\save-snapshots\car-packs-<session>` (the newest 5 such copies are kept, apart from those mods ask for), and `fh2-mods\platform-cars-missing.txt` lists the cars;
- every start it's missing, the title screen says which pack's cars aren't in the game ("Car pack Mustang clone's cars aren't in the game this time."), with why on the line below, and the pack's row on the MODS tab says so in red. For a pack whose folder is gone only `platform.log` says so;
- when the database the platform built on can't be read, which passes on its own, the database from the last start is kept as it is, with the packs' cars, rather than one without them.

**Extracting an older mod zip takes car packs out.** Every mod zip ships the platform it was built with, and many (WreckingBall 1.3.3, Achievements 1.0.2 and others) still ship 2.8.0, which doesn't know car packs. Extract one of those and the next start has none of your packs' cars, with no warning, since 2.8.0 can't give one. Reinstall a mod that ships 2.9.0 (or the platform's own zip) before playing, and keep a copy of your saves if you own a pack's car.

## With Extra Options

Extra Options (`winmm.dll` next to `azimuth.exe`) takes over how the game finds its files, and reads `mediaoverride`, `mediapc` and its own mods' folders. The platform takes its turn after it: it adds the packs' folders, and one folder of its own, ahead of the ones Extra Options adds. You install packs the same way, in `fh2-cars`, and they get the same Ids as without Extra Options, so a save works either way.

What's different:

- **Where the files are.** The packs' database, their cars' names and the merged `zipmanifest.xml` are in `data\titles\1A96545B\tmp\fh2-platform\eo-media`, which only the platform writes to. Everything else the platform writes (mods' text and files, the MODS tab) goes to `mediaoverride`, as it always has with Extra Options. `mediaoverride`'s database gets mods' changes only, so other Azimuth folders on the same game folder don't see this folder's packs.
- **It needs Extra Options to be working.** Extra Options 0.6.0 is the version tested. Its `FH2_ExtraOptions.ini` has to be there with `[Global] Enabled` not 0, and either `EnableMediaOverrides=1` or a mod of its own turned on, since otherwise it adds no folders at all.
- **Everything is checked, every start.** That Extra Options took the file lookup, that the packs' folders went in before it set up its own, that they are ahead of its folders, and that the game read its list of zips and its database from the platform's folder. `platform.log` has a line for each, all starting "car packs with Extra Options:". If any check fails, the packs are switched off for that session and the game reads what it would without them, so it never plays with half of a pack. The title screen and the packs' rows on the MODS tab say why, and `fh2-mods\platform-eo-packs.txt` has the outcome ("in" or "lost").
- **A file in two places leaves the pack out.** If one of a pack's files (a car's zip, say) is also in `mediaoverride`, `mediapc` or one of Extra Options' own mods, that pack is left out for the session, with the file named in the log and on its row. Install a car as a pack or as an Extra Options mod, not both. An Extra Options mod with a `zipmanifest.xml` of its own leaves every pack out.
- **Extra Options' own car mods keep their Ids.** A new pack car is never given an Id that a turned-on Extra Options mod's `database.json` inserts into `Data_Car`. If such a mod arrives later with the Id a pack car already has, the pack car keeps it and Extra Options moves or refuses its own (its log says which); only a mod that shares rows by Id (`id_conflicts: last_wins`) leaves the pack out, as the car's row would become the mod's.
- **FH2MP.** Started from the FH2MP launcher (online play), the packs are off for that session: online, the game has to read the server's database, which FH2MP puts in `mediaoverride`. Started normally, they're read. If FH2MP asks to go online while a game with the packs is running, it's told to have you start from its launcher instead.

Moving `winmm.dll` away (or back) changes nothing for the packs or your save: the cars and their Ids are the same, only the folder the database is read from differs.

## This Azimuth folder's own files

On Azimuth 1.0 or later without Extra Options, everything the platform writes for the game goes to the Azimuth folder itself, from 2.9.0: mods' text and files (`set_string`, `set_file`), the MODS tab's pages, the database, and the packs' merged `zipmanifest.xml`. They're in `data\titles\1A96545B\tmp\fh2-platform\media`, part of the temporary drive Azimuth keeps for FH2 in its own folder, which the game reads ahead of `mediaoverride`. So Azimuth folders that share one game folder no longer see each other's mods. `mediaoverride` is still read, after this: files other mods copy there, a traffic car mod's database. The first time the layer is used, the files and string tables the platform wrote to `mediaoverride` before (as 2.8.0 did) are moved to `fh2-mods\platform-old-overrides\mediaoverride`, so they can't show through from under it; `moved.txt` there lists them.

With FH2MP in the Azimuth folder (an `fh2mp` folder there), the database is the exception: online play needs exactly the server's database, which FH2MP puts in `mediaoverride`, so the platform builds the database there as before ([api.md](api.md#add_sql-clear_sql-and-apply_database-api-2)). Packs' other files are still read.

## Making a pack

```
fh2-cars\<pack>\
  pack.ini                                        the pack's name and the cars it adds
  <name>.sql                                      database changes (any name; run in name order)
  <car>.part-ids.txt                              where a car's carbin names its own part rows (see The model)
  media\cars\<car>.zip                            each car's model and data, as the game's own
  media\Stripped\RC1\Thumbnail_<car>_Big.swatchbin    its thumbnails
  media\Stripped\RC1\Thumbnail_<car>_Small.swatchbin
```

`pack.ini` names the pack and lists the cars it adds, one section each, named after the car's media name (its `Data_Car.MediaName`, and the name of its zip):

```ini
[pack]
name = Mustang clone
version = 1.0
author = Trinculo54

[car FOR_MustangCL_15]
name = Mustang GT Clone
id = 3000
```

The platform reads `[pack]`'s `name` and `version`, the `[car ...]` section names, and each car's `name` (the MODS tab lists it) and `id`, the Id the car asks for ([Database rows](#database-rows)). Anything else in a car's section is for the tool that made the pack. Save `pack.ini` as UTF-8 without a byte order mark, so names with letters beyond ASCII ("Huracán EVO") show as written; with a byte order mark Windows doesn't read the file's first line. A pack without `pack.ini` adds no car, but its SQL and files are still used, so it can change stock cars too.

### Database rows

The pack's `.sql` files run on the platform's copy of the game database, in file-name order and before mods' statements, so mods that change every car change the pack's too. Each pack runs in a transaction of its own: if a statement fails, none of the pack's changes are made, and the log has the SQLite error.

Write `{car:<media name>}` wherever one of the pack's cars' Id goes, and never a number. The platform gives each car an Id the first time it sees it, and keeps it for good, since a bought car's Id is in the save. The Ids are recorded in `fh2-mods\platform-car-ids.txt`, which is each Azimuth folder's own, with a copy next to the folder's database (`data\titles\1A96545B\tmp\fh2-platform\media\Stripped\platform-car-ids.txt`), so emptying `fh2-mods` to reinstall doesn't lose them. A car that neither lists keeps the Id it has in the database the platform built last time, and the log says so ("TEST_Pack01 is car 3003 again").

A save shows the same cars on another install only if their cars have the same Ids there, so give each car an `id` in `pack.ini`, from 3000 to 8999 (the game's own cars go up to 2501, and FM5's to 2169), one no other pack's car has. A car new to an install gets the Id it asks for whenever that one is free there, so every install with the pack gives the car the same Id, whatever order its packs were added in. When it isn't free (another car has it, or a mod's or the database's rows take it), the car gets another, and the MODS tab and the log say why, and that saves from other installs don't show it. A car without an `id` gets the lowest free one from 3000 up that no other installed pack's new car asks for, so its Id can differ between installs. A car that already has an Id keeps it, even if `pack.ini` asks for another later; the log says so. An `id` that isn't a number from 3000 to 8999 is ignored, with a warning.

A car takes more than its `Data_Car` row: its Id `N` keys its rows in `Data_Car`, `Data_Engine`, `Data_Drivetrain`, `Data_Car_Buckets`, `CameraOverrides` and `CarExceptions`, and `N * 1000` to `N * 1000 + 999` its rows in `Data_CarBody` and the `List_*` tables. A new car gets an Id all of whose rows are free: in the database being built on, in the rows the mods' SQL adds, and in those packs add by numbers of their own. The platform runs that SQL on a copy in memory first to see them. So a mod that adds car 3014 itself, or engine 3014 or rows 3014000 up, doesn't clash with the next pack car: that one gets 3015. (The game's own drivetrain 5000 and its rows are skipped the same way.)

Car packs' cars get Ids from 3000 to 8999, never 9000 or above. A mod (or a pack without `pack.ini`) that has to add a car by an Id of its own should use one from 9000 up, and that car's rows from `Id * 1000`. One that adds rows by an Id from 3000 to 8999 works, but the MODS tab and the log warn about it: a pack car recorded with that Id before is left out (below). So is a pack whose SQL never says `{car:X}` for one of the cars its `pack.ini` lists, since that car doesn't get the Id the platform gives it (its carbin's part Ids are set for that one).

A new car needs, as the game's own cars have:

- a `Data_Car` row, with its `MediaName`, a `Thumbnail`, and `ContentId` 0 (no content licence to check);
- a `Data_Car_Buckets` row: its group in the Autoshow (`CarBucket`);
- its rows in the tables keyed by `Ordinal` or `CarId`: upgrades (`List_Upgrade*`), suspension and anti-roll bar physics, `Combo_Colors`, `CameraOverrides`, `CarPartPositions`. Their own ids are `<car Id> * 1000 + n` by the game's convention, so write them as `{car:X} * 1000 + n`. `Combo_Colors` ids aren't the car's: use ones after the highest there is.

Engines, drivetrains and bodies can be shared with a stock car. The game's notes on the database are in [game-notes.md](game-notes.md#cars).

The game trusts every row it reads, so a row a car needs that isn't there usually stops the game rather than showing an error ([game-notes.md](game-notes.md#rows-the-game-trusts)). Two to watch:

- **`Data_Car.PowertrainID` has to be an Id in `Powertrains`** (0 to 7 in the stock game: the drive type and where the engine sits, with the icon the menus show). Without it the game stops as soon as the Autoshow or a garage lists the car (issue #23). From 2.9.0 the platform catches that one: when the game reads a database a car pack or a mod changed, a car whose powertrain isn't in the table is listed without its drivetrain. `platform.log` names those cars as the game starts ("[Database guards] these cars' powertrains (in brackets) aren't in its Powertrains table, ...: PACK_Car (12)") and says so again the first time a menu lists one, and YOUR MODS ends with a "Game database" row, with a note in red, for the rest of the session. Fix the pack: the guard only keeps the game running.
- **A car's upgrades in a save.** A save keeps the part Ids of the upgrades fitted to each car. If a pack's upgrade rows change Id or go (a new version of the pack, or the pack removed and its car still in the garage), the game stops at `0x105BEEC` when it loads that car's physics. The platform can't catch this one safely yet. Keep upgrade rows' Ids (`{car:X} * 1000 + n`) the same from one version of a pack to the next, and keep a copy of your saves before updating or removing a pack whose car you've upgraded.

Give `Data_Car.DisplayName` and `ModelShort` as text. The game's own are string-table references (`_&<id>`), and the top bar only shows names from the string tables, so the platform gives each name a new key in `Data_Car.str` (from `0xFF00`) and writes it into every language's table ([string-tables.md](string-tables.md#adding-keys)). `DisplayName` is what the Autoshow and the top bar show.

A pack is left out, and the log says why, when the database already has one of its cars (by media name), when another pack adds the same car (the first pack by folder name keeps it), when one of the rows of the Id recorded for one of its cars is in the database now or is added by a mod's or another pack's SQL (its car keeps that Id: remove what has it and the pack is back), or when no Id below 9000 is free.

### The model

`media\cars\<car>.zip` is laid out as the game's own car zips ([game-notes.md](game-notes.md#cars)). The platform reads its directory and adds its line to the game's `zipmanifest.xml`, so it can be any size. While it writes that merged `zipmanifest.xml`, a mod can't replace it with `set_file`, which would take the packs' zips out. The car's files name it in their paths, `game:\media\cars\<car in lower case>\...`, so a copy of a stock car has to have those changed too. A zip with the name of one of the game's replaces it for that Azimuth folder.

A carbin names the models of the car's upgradeable parts (rear wing, bumpers, side skirts, hood, roll cage) by the Ids of their database rows. The bumpers', side skirts', hood's and roll cage's are its body's rows, so a car sharing a stock car's body names them as that car does. The rear wing's rows are the car's own, `{car:X} * 1000 + n`, so its carbin can only name them once the car has its Id: `<car>.part-ids.txt` says where they are, and at each start the platform sets them in its copy of the zip on the temporary drive.

```
zip cars\<car>.zip        the zip, under media
entry <offset>            where the carbin's local header is in it
at <offset> <n>           a 4-byte Id there is set to <car Id> * 1000 + n (a line for each)
```

The carbin has to be stored in the zip so its bytes stay where the list says: stored, or deflated in stored blocks only. The platform makes the entry's CRC right after setting them.

### The thumbnail

With `Thumbnail` set to `game:\Media\Stripped\RC1\Thumbnail_<car>.png`, the game reads `Stripped\RC1\Thumbnail_<car>_Big.swatchbin` (816 x 459, the Autoshow's large picture) and `_Small.swatchbin` (192 x 108, the car lists) from the pack, where it would read them in `RC1.zip`. The format is in [game-notes.md](game-notes.md#thumbnails). Keep the large one's top left empty, as the game's are: the Autoshow writes the make and model over it.

### Other files

Any file under the pack's `media` is read where the game would read the same path, as with `mediaoverride`: `media\X\path\file` stands for `media\X.zip`'s `path\file`. Where two packs have the same file, the first pack by folder name wins, for zips and their manifest lines too.

## How it works

At each start, before the game runs, the platform links each pack's `media` folder into `data\titles\1A96545B\tmp\fh2-platform\packs\<pack>` (hard links, so no space is used; copies when the pack is on another drive), writes the merged `zipmanifest.xml` into its layer, and builds the database with the packs' and the mods' statements. A car zip with a `part-ids.txt` is a copy there, with the car's Ids set in its carbin; it's copied again only when the pack's zip changes. When the game first looks up a file, it mounts its layer and then each pack's folder ahead of `mediaoverride`. The game reads only through Azimuth's drives, so that's where its folders have to be ([game-notes.md](game-notes.md#azimuths-drives-and-this-folders-layer)).

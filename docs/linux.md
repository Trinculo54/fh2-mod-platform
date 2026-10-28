# FH2 mods on Linux (Wine and Proton)

FH2 mods run on Linux wherever Azimuth runs, under Wine or Proton. The platform needs nothing extra. Extra Options needs one setting, and a few mods have limits there. What's here was tested with Wine 9.0 or seen in Linux players' logs; Proton itself hasn't been tested directly.

## Setting up

**1. Let Extra Options load.** Extra Options is a `winmm.dll`, and Wine uses its own `winmm.dll` instead unless told otherwise. Set this DLL override for Azimuth:

```
WINEDLLOVERRIDES="winmm=n,b"
```

- Steam, including Azimuth added as a non-Steam game: put `WINEDLLOVERRIDES="winmm=n,b" %command%` in its launch options.
- Lutris, Heroic or Bottles: add `winmm` as a DLL override, native then builtin, in the game's settings (or set the environment variable above there).
- Wine on its own: start Azimuth with `WINEDLLOVERRIDES="winmm=n,b" wine azimuth.exe`, or add `winmm` (native, builtin) under Libraries in `winecfg`.

Without it Extra Options doesn't run. The platform's text, file and database changes reach the game through the `mediaoverride` folder: up to 2.7.2 only Extra Options made the game read it, so none of them showed up. From 2.7.3 the platform mounts the folder itself when Extra Options isn't loaded, so mods' changes still show up; Extra Options' own features (and the menus that come from it) still need the override. From 2.7.1, `fh2-mods\platform.log` says so ("Extra Options isn't running: its winmm.dll is next to azimuth.exe, but Wine is using its own winmm.dll instead"); older versions said nothing, and waited 20 seconds for Extra Options before mods could hook the game.

**2. The platform loads as it is.** With Wine 9.0, the `d3dcompiler_47.dll` next to `azimuth.exe` is used without an override. If `fh2-mods\platform.log` never appears, your Wine or Proton build is using its own copy: add it to the override, `WINEDLLOVERRIDES="winmm,d3dcompiler_47=n,b"`. Azimuth 1.0.3 no longer loads `d3dcompiler_47.dll`; there the platform is the `DWrite.dll` next to `azimuth.exe` (platform 2.7.4 and newer), and Wine has its own `dwrite.dll` too. If `platform.log` never appears on 1.0.3, use `WINEDLLOVERRIDES="winmm,dwrite=n,b"`.

**3. Use platform 2.7.1 or newer.** Up to 2.7.0 the platform changed the game database with Windows' own SQLite (`winsqlite3.dll`), which Wine and Proton don't have. Mods that change the database did nothing on Linux, and `platform.log` said "Windows' SQLite (winsqlite3.dll) isn't available, so database changes are off". Every mod's zip includes the platform, so installing one that ships 2.7.1 or newer updates it. If you're stuck with an older one, this works as well: download the 64-bit DLL zip from [sqlite.org](https://sqlite.org/download.html) ("Precompiled Binaries for Windows"), rename its `sqlite3.dll` to `winsqlite3.dll` and put it in your prefix's `drive_c/windows/system32` folder (for Proton, `steamapps/compatdata/<id>/pfx/drive_c/windows/system32`). Next to `azimuth.exe` doesn't work.

## The mods

| Mod | On Linux |
|---|---|
| [Achievements](https://github.com/Trinculo54/fh2-achievements) | From 1.0.1 it works as on Windows. 1.0.0 read your save with `winsqlite3.dll` too, so it never unlocked anything (its log said "winsqlite3.dll isn't available"); the `winsqlite3.dll` workaround above fixes 1.0.0 as well. Achievements you earned meanwhile are in your save and unlock on the next launch that can read it. |
| [WreckingBall](https://github.com/Trinculo54/WreckingBall) | Nothing Linux-specific: a player's log shows it loading and hooking the game as on Windows. |
| [WheresMyBuddy](https://github.com/Trinculo54/WheresMyBuddy) | The Xbox sign-in opens your Linux browser through Wine. If no browser appears, set `browser_sign_in=0` under `[xbox]` in `wheresmybuddy\WheresMyBuddy.ini` to sign in with a code instead; the address and the code are in `wheresmybuddy\SIGN-IN.txt`. Wine protects the saved sign-in much more weakly than Windows does, so keep your prefix to yourself, and moving between Wine and Proton means signing in again. |
| [FH2 Universal Radio](https://github.com/Trinculo54/FH2-Universal-Radio) | Not tested on Linux yet. Known limits: capturing another app's audio doesn't work, because Wine doesn't support WASAPI loopback capture; and skipping tracks from the game sends media keys, which a Linux Spotify app doesn't get, so skip in Spotify itself. Your Linux files are under `Z:\` (`Z:\home\you\Music`), and the dashboard opens in your Linux browser at `http://localhost:8420`. |
| [fh2wheel](https://github.com/iamAzoDx/fh2wheel) | Not tested on Linux. |

## When something doesn't work

- **There's no `fh2-mods\platform.log`.** The platform didn't load: add `d3dcompiler_47` (on Azimuth 1.0.3, `dwrite`) to the override (step 2).
- **`platform.log` is there, but there's no MODS tile and no changed text.** With platform 2.7.2 or older, Extra Options isn't running: check the `winmm` override (step 1), or update the platform. From 2.7.3 look for "media overrides:" lines in `platform.log`. From 2.7.1 `platform.log` says "Extra Options isn't running" when Wine is the reason. `FH2_ExtraOptions.log` next to `azimuth.exe` is rewritten every time Extra Options starts, so an old date on it means it didn't.
- **`platform.log` says database changes are off.** The platform is older than 2.7.1 (step 3).
- Crash reports go to `fh2-mods\crashes` as on Windows. Paths in logs start with `Z:\`, which is your Linux root, `/`.

## For mod makers

- **Only use DLLs Wine has.** `winsqlite3.dll` isn't one: build SQLite into your mod, as the platform does (`third_party/sqlite`), or use the platform's database functions. Check what you load with `llvm-objdump -p` and against the DLLs in your Wine install.
- **Wine ignores a proxy DLL named after one of its own** (`winmm.dll`, `dxgi.dll`, `version.dll`, `winhttp.dll` and many more) unless the player sets an override. `d3dcompiler_47.dll` is used as it is with Wine 9.0; whether `DWrite.dll` is hasn't been checked yet.
- **When Wine uses its own copy, it still reports the path of the file in the game folder** (`GetModuleFileName`, `GetModuleHandleEx` with that path). To tell whether a proxy is really loaded, look for one of its exports, not at its path: Extra Options' is `Fh2TweaksStatus`, which is how the platform recognises it from 2.7.1.
- **Other things that differ:** most of WinRT (`Windows.Media.Control` and the like) isn't there; WASAPI loopback capture isn't supported; `SendInput` only reaches Wine's own windows; `ShellExecute` on a URL opens the Linux browser; `CryptProtectData` works, but its protection is weak.
- **Test in a fresh prefix**, such as `WINEPREFIX=$(mktemp -d) wineboot -i`. A DLL you added to your own prefix while testing hides the problem from you: the platform's tests once had a `winsqlite3.dll` put in theirs, which is how this one went unnoticed.

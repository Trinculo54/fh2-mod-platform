# FH2 Mod Platform 2.9.0: release notes (draft)

The language and UI release. Mod settings get one home, the platform's menus follow the game's language, and mods can show pop-ups, tips and message boxes in the game's own look, in your language too. It also brings car packs, guards against two kinds of crash, and fixes for online play and Proton. API 10, which only adds to API 9: mods made for 2.8.0 or older keep working.

## For players

### What's new

- **One home for mod settings.** Every mod has its own page under **Pause > MODS > YOUR MODS**, with its switches, sliders, choices, keys, links and pages in one place, and **Pause > Settings > MODS** opens the same list. The list shows each mod's version and state ("Off", "Off online", "New", a yellow "Update x.y.z", or a red "! Not working" saying why). Settings marked \* take effect the next time you start the game, and when you leave, the game offers to restart for you. HELP takes the place of ALL SETTINGS on the MODS tab.
- **Your language.** The MODS tab, every mod's page and the restart prompt are in the language the game shows: Brazilian Portuguese, Dutch, French, German, Italian, Japanese, Korean, Polish, Russian, Spanish and Traditional Chinese, as well as English. Mods can now ship translations too, and theirs follow the game's language in the same way. The first time, before the game has said which language it uses, the platform guesses it from Windows' regional format, as the game does.
- **Pop-ups and tips from mods**, on the game's own notification at the bottom of the screen, one queue for every mod and spaced around the game's own. They wait while you're in a menu, photo mode, a replay or a cutscene. You decide whether they show, on the platform's own page, **FH2 Mod Platform**, at the end of YOUR MODS: **Mod pop-ups** (follows the game's Notifications setting, or Off), **Tips**, **Show tips again**, and **Recent pop-ups**, which lists the last 10 with the mod that sent each. A mod that shows pop-ups also gets a **Pop-ups** row of its own, so you can quieten one mod without the others.
- **Car packs.** New cars for the Autoshow, each pack a folder in `fh2-cars` next to `azimuth.exe`, with nothing written to the game folder. They need Azimuth 1.0 or later without Extra Options. On such an install, everything the platform writes for the game now goes to the Azimuth folder itself, so Azimuth folders that share one game folder no longer see each other's mods. See [car-packs.md](car-packs.md).
- **Fewer crashes.**
  - A car whose drivetrain isn't in the game's database (from a car pack or a mod that changes the database) no longer stops the game when the Autoshow or a garage lists it: it's listed without its drivetrain, and `platform.log` names the car ([issue #23](https://github.com/Trinculo54/fh2-mod-platform/issues/23)).
  - A mod's page that would crash the game as it opens is checked first, and shows "This page couldn't be shown" instead. A page that crashed the game anyway is left out until the mod is updated or reinstalled.
  - When another program installs a crash handler after the platform's, a crash now goes through it once, after the platform's report, instead of back and forth between the two until the stack overflowed; Azimuth's and Windows' own handling is reached again.
- **If the MODS tab is missing** because the game isn't reading the platform's menu files, a message box now says so once a session, and `fh2-mods\platform.log` says why.

### Online play (FH2MP)

- **Mods that shouldn't run online are stopped, even when FH2MP turns out to be online late.** Up to 2.8.0 the platform only looked once, before any mod loaded, so a nitro mod could stay on online when FH2MP was started without its launcher (`azimuth.exe --skip-launcher`) or its session was written late ([issue #22](https://github.com/Trinculo54/fh2-mod-platform/issues/22)). The platform now looks again as FH2MP starts, and when FH2MP tells it it's going online, and stops such mods for the rest of the session: their hooks, text, files and database changes come out, and the MODS tab shows them as "Off online". The **Load when playing online** switch on each mod's page still decides.
- **FH2MP players: the FH2MP launcher must ship 2.9.0 too.** The launcher puts back the platform it was built with every time you press Play, so until an FH2MP release ships 2.9.0, playing through it takes your install back to 2.8.0.

### Linux (Wine and Proton)

- Under Proton, mods' text, files and database changes reach the game without Extra Options, even though Proton maps the game somewhere other than Windows does ([PR #17](https://github.com/Trinculo54/fh2-mod-platform/pull/17)). [linux.md](linux.md) has the details.
- With Extra Options running under Wine or Proton, the platform now finds it among every loaded DLL and leaves the override folder to it. That should stop Extra Options refusing its patches in some launches ("REFUSED: unexpected loaded bytes at media_override_native_resolver" in `FH2_ExtraOptions.log`); if it still happens, please report it with both logs.

### What to do

- **To get 2.9.0**, install (or reinstall) any mod that ships it, or copy `d3dcompiler_47.dll` and `DWrite.dll` from the platform's release zip next to `azimuth.exe`, replacing both. Line 1 of `fh2-mods\platform.log` says which version runs.
- **Keep it in place.** Every mod zip ships the platform it was built with. If you unpack one that ships 2.8.0 or older afterwards, it puts that back and you lose what's above (an older platform also brings back the old Settings > MODS page): reinstall a mod that ships 2.9.0.
- **Your settings carry over.** Mods keep their own settings, and the platform's files in `fh2-mods` stay. To see the first-run hints and every mod's tips again, delete `fh2-mods\platform-seen.txt` (or use Show tips again for the tips).
- **Achievements players** on Azimuth 1.0 or later without Extra Options: Achievements 1.0.2 looks for the game database in `mediaoverride`, and 2.9.0 keeps the one with mods' changes in the Azimuth folder's own layer there, so until Achievements' next release it reads the game's own database (or another mod's in `mediaoverride`). Counts that depend on other mods' database changes may differ meanwhile.
- **If something goes wrong**, attach `fh2-mods\platform.log` and any `fh2-mods\crashes\crash-*.txt` to your report. `platform.log` now also lists every pop-up and tip shown, with the mod that sent it.

## For mod makers

### New in API 10

- **Text in the game's language.** `game_language`, `set_setting_text` and `set_mod_description` change your rows and description once the game has picked its language (from PR #24).
- **Translations.** `language`, `add_translation`, `tr`, `tr_plural`, `set_string_tr`, `format_number` and `format_date`: gettext PO files built into your DLL (`fh2-mods\lang\<Mod>\<lang>.po` overrides them, for translators), looked up in the game's language as it changes, with each language's plural forms and its numbers and dates. Text you got from `tr()` and pass on as it is follows the game's language by itself. `include/fh2mod_l10n.h` has `FH2_TR` and friends, which give the English on older platforms, and `tools/l10n` has `extract.py` and `po2inc.py`. `test=1` in `fh2-mods\platform-language.ini` turns on a padded test language. See [translating.md](translating.md).
- **UI templates.** `ui_status`, `ui_uses`, `play_ui_sound`, `show_toast`, `clear_toast`, `show_tip`, `tip_seen`, `set_tip_seen`, `show_message_box`, `set_page_help`, `set_page_hints` and `show_page_hint`: the game's own notification, message box and UI sounds, and your pages' help bar and hint line, filled with your text. The platform draws them, decides when they show, logs what they showed, and never takes XAML from you. `include/fh2mod_ui.h` wraps every one so it does nothing on an older platform, with no version checks of your own. `fh2mod Platform ui` says what's on screen and waiting, and `Developer=1` under `[popups]` in `fh2-mods\platform-popups.ini` adds a test page and test commands.
- **For FH2MP:** the `FH2Platform.Online` interface, with `going_online` to call before connecting.
- **Previews:** `FH2Platform.HudLayer` (version 2, adding a shared feed over the minimap) and `FH2Platform.PadLayer` (version 1). See [hud-layer-preview.md](hud-layer-preview.md) and [pad-layer-preview.md](pad-layer-preview.md).

[api.md](api.md) covers every member, and [writing-a-mod.md](writing-a-mod.md#showing-things-to-the-player) has a table of which template to use for what.

### What changed for existing mods

API 10 only adds members at the end of `FH2ModApi`, so a mod built against API 9 or older loads and runs without a rebuild. A few things behave differently, though:

- **`set_file` refuses the platform's own UI files** (`A_Hud`, `A_Splash`, `A_PauseMenu`, `A_PauseOptions`, `A_ModOptions` and `A_ModPage_NN` in `UI\Scenes\Anthem`, `stateflow\pause.xml` and `ui_screens.xml`), in any spelling, and returns 0. Before, the first mod to set one won and took the MODS tab, the title screen's entries or the HUD notice away from every player. For a page of your own, use pages (API 8).
- **`set_page_xaml` checks your XAML** before the game sees it: every resource key has to be one every page has or one of your own, and every image one of the game's textures. A fragment that fails returns 0, and the page keeps the last XAML that passed (or shows "This page couldn't be shown"). A page the game crashed opening is left out until your DLL changes. Text beyond ASCII on pages now reaches the game as it is, except characters beyond U+FFFF (emoji, which the game's fonts don't have), which show as "?", and control characters other than line breaks and tabs, which are left out.
- **Online play can start late.** `playing_online` can turn from 0 to 1 during a session, never back, so check it when it matters, not once. A mod with `offline_only` (or with Load when playing online turned off) that had already started is stopped when FH2MP turns out to be online: its hooks and byte patches go off, its text, SQL and files come out, and its hotkeys, commands and title-screen entries do nothing. What you do from your own threads is yours to stop when `playing_online` turns 1.
- **Pop-ups and tips need declaring.** Call `ui_uses(FH2_UI_USES_TOASTS)` (and `_TIPS`) from `fh2mod_init`: the platform hooks the game's notification only in a session where a mod did, or with `Developer=1`. Without it, `show_toast` and `show_tip` return 0 when no other mod declared them, and your mod gets no Pop-ups row when another did.
- **Your rows have moved.** They're on your mod's own page under MODS > YOUR MODS, with your links and pages, and Settings > MODS opens the same list. Descriptions no longer end in "(Mod)". Update the README lines that send players to "Settings > MODS" or "MODS > INSTALLED MODS".
- **Where the platform writes.** On Azimuth 1.0 or later without Extra Options, string tables, `set_file` files and the database go to `data\titles\1A96545B\tmp\fh2-platform\media` in the Azimuth folder, which the game reads ahead of `mediaoverride` (the database stays in `mediaoverride` while FH2MP is in the folder). A mod that reads one of those files itself has to look there first; never hard-code `mediaoverride`.
- **The update notice** on the title screen reads "MyMod 1.1.0 is out: Pause > MODS > UPDATES".
- **Title-screen entry labels** are cut at 32 characters rather than 32 bytes, and shown in capitals the way the game's language writes them, not only a to z.
- **The game sites the platform hooks** grew; [game-notes.md](game-notes.md#sites-the-platform-hooks) and `include/fh2game.h` list them all, with when each goes in. Don't hook `Show` or `MakeDisplayString`: Achievements and FH2MP check their first bytes in memory.

When you release a mod built on 2.9.0, bump your `platform` submodule to the v2.9.0 tag so your zip ships it, and say in your README that it needs platform 2.9.0 for the new calls.

## Known limitations

- **Some of the platform's text stays in English.** Its notices on the HUD (the mods loaded, NEW MOD, a mod that didn't load) are written before the game picks its language. The title-screen entry lines are written as the game starts, in the language it had last time, so after you change the game's language they follow from the next launch. The platform's own title-screen notices ("... is out", "Old FH2 Mod Platform: reinstall ...") are English on purpose, and so are the `Developer=1` test page and the `fh2mod Platform ui` output.
- **Pop-up text is folded to plain letters** until the platform's wide text has been seen working in game: "ł" shows as "l" and "ß" as "ss", and letters with no plain form (Cyrillic, Japanese, Korean, Chinese) as "?". Tips go the same way. Message boxes and pages show every letter the game's fonts have. `WideText=1` under `[popups]` in `fh2-mods\platform-popups.ini` turns the wide text on, for testing it.
- **The translations of the platform's own text are drafts** waiting for review by volunteers, Polish and Brazilian Portuguese first. Corrections are welcome as issues or pull requests.
- **The game's Czech and Hungarian text is English**, and so is the platform's when the game is in either.
- **Pop-ups are dropped, not held, while the game's Notifications setting is Off**, since the game then draws no notification at all.
- **Message boxes' help bar** reads "(A) Select (B) Cancel" while the box's last item says Back. Both do the same.
- **Car packs aren't read with Extra Options loaded.** They need Azimuth 1.0 or later without it.
- **A saved upgrade whose part no longer exists** (a car pack or mod that changed the database, then removed) can still stop the game as the car loads. That one isn't guarded yet; game-notes.md says why.
- **The HUD layer and pad layer are previews.** They aren't part of an API version, and their names, limits and behaviour may change before they are.
- **Under Wine and Proton**, hotkeys and commands (from 2.8.0) and the new pop-ups, tips and message boxes haven't been tried in game yet.

## Credits

- **qiangqiang101**, for PR #24: text in the game's language (`game_language`, `set_setting_text`, `set_mod_description`), the platform's own text in 12 languages, and the MODS tab's tiles in the game's language.
- **consultoria-deta**, for PR #17: the platform's override folder under Proton, and finding Extra Options when Wine loads it under another name.
- Everyone who tested the HUD layer, car packs and the new menus, and the players who reported issues #22 and #23.

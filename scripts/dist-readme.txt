FH2 Mod Platform
================

The shared base for Forza Horizon 2 (Azimuth) mods. It's one DLL under
two names: d3dcompiler_47.dll, which Azimuth up to 1.0.1 loads, and
DWrite.dll, which Azimuth 1.0.1 and later load (1.0.3 loads only this
one). It loads every mod in the fh2-mods folder next to azimuth.exe and
gives them game-text edits, game file and database changes, title-screen
notices, update checks, logging, a way to talk to each other, access to
the game, and shared hooks into its code. From 2.9.0 it also shows mods'
text in the game's language, gives them pop-ups, tips and message boxes
in the game's own look, and loads car packs from the fh2-cars folder.

Players: you usually don't need this zip. Every mod built on the platform
ships it in its own zip. On Azimuth 1.0.3, if your mods' zips only have
d3dcompiler_47.dll (platform 2.7.3 or older), copy DWrite.dll from here
next to azimuth.exe. Mod settings are under Pause > MODS > YOUR MODS
(Pause > Settings > MODS opens the same list), and the platform's own
page at the end of that list says whether mods' pop-ups and tips show.
A mod zip that ships an older platform puts it back over this one:
reinstall a mod that ships 2.9.0 afterwards. FH2MP players: the FH2MP
launcher puts back the platform it was built with every time you press
Play, so 2.9.0 stays only once FH2MP's launcher ships it.

Mod makers: this zip has
  d3dcompiler_47.dll        the platform, to test with
  DWrite.dll                the same file, for Azimuth 1.0.1 and later
  include\fh2mod.h          the API (C/C++)
  include\fh2game.h         names for the game addresses mods use, and the
                            sites the platform hooks
  examples\HelloMod.dll     an example mod; put it in fh2-mods to try it
  tools\fh2mod.exe          runs mods' commands in the running game:
                            fh2mod list, fh2mod <mod> <command> [args]
Docs, the example's source and build setup, the helper headers for
translations and pop-ups (fh2mod_l10n.h, fh2mod_ui.h), and the 2.9.0
release notes:
  https://github.com/Trinculo54/FH2-Mod-Platform

Everything the platform does is logged to fh2-mods\platform.log. If the
game crashes, a report naming the mod involved goes to fh2-mods\crashes.

MIT License. The platform includes MinHook (BSD 2-Clause), so ship
LICENSE-MinHook.txt along with the platform's DLLs. Unofficial fan-made
software; not affiliated with Playground Games, Turn 10, Microsoft or the
Azimuth developers.

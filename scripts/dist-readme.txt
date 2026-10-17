FH2 Mod Platform
================

The shared base for Forza Horizon 2 (Azimuth) mods: d3dcompiler_47.dll
loads every mod in the fh2-mods folder next to azimuth.exe and gives them
game-text edits, game file and database changes, title-screen notices,
update checks, logging, a way to talk to each other, and access to the
game.

Players: you don't need this zip. Every mod built on the platform ships
d3dcompiler_47.dll in its own zip.

Mod makers: this zip has
  d3dcompiler_47.dll        the platform, to test with
  include\fh2mod.h          the API (C/C++)
  examples\HelloMod.dll     an example mod; put it in fh2-mods to try it
Docs, the example's source and build setup:
  https://github.com/Trinculo54/FH2-Mod-Platform

Everything the platform does is logged to fh2-mods\platform.log. If the
game crashes, a report naming the mod involved goes to fh2-mods\crashes.

MIT License. Unofficial fan-made software; not affiliated with Playground
Games, Turn 10, Microsoft or the Azimuth developers.

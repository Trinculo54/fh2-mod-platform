// play_ui_sound (API 10, platform 2.9.0): the game's own UI sounds for every mod, with no HUD layer needed. See
// ui_sounds.cpp; the rules are ui_rules.cpp's UiSoundRules.
#pragma once

#include "fh2mod.h"

#include <cstdint>

namespace fh2platform {

// The API member (ui_templates.cpp), from any thread.
int ui_sounds_play(FH2Mod* mod, uint32_t sound);
// Plays FH2_SOUND_ `sound` now, on the game's thread (the pop-ups' tick plays a pop-up's sound this way): false if it
// can't (the game's sound code isn't as expected, the player isn't up, or it crashed: `code` is the exception then).
bool ui_sound_play_direct(uint32_t sound, uint32_t* code);
// fill_ui_api: the API the sounds' own hook is made through.
void ui_sounds_prepare(const FH2ModApi* api);

// settings.cpp: the MODS tab's own way of playing a sound by its FMOD name (the GameString to the player at
// game+0x365A298, its vtable +0x08), without the MODS tab's checks, which ui_sounds.cpp makes its own. False if not
// played: `code` is 0 when the game's code isn't known yet, else the exception it raised.
bool settings_play_sound_unchecked(const char* name, uint32_t* code);

} // namespace fh2platform

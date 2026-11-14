// The HUD layer preview's glue (hud_layer.cpp): the FH2Platform.HudLayer interface, the page, the code
// checks, the hooks, the pump, the getter, the hold and the sounds. Without a mod that adds a layer,
// none of it does anything.
#pragma once

#include "fh2mod.h"

#include <cstdint>
#include <string>

namespace fh2platform {

// hud_layer.cpp
const void* hud_interface();                     // the FH2HudInterface table
FH2Mod* hud_layer_owner();                       // the HUD layer's own FH2Mod: its hooks, notice and log lines
void hud_layer_prepare(const FH2ModApi* api);    // load_mods, after every fh2mod_init, before the first apply_all
bool hud_layer_wants_page();                     // write_hud_notice, settings_start
bool hud_layer_splice(std::string& page);        // write_hud_notice: false = the layer is left out (logged)
void hud_layer_page_written(bool ok, const std::string& page);
void hud_layer_start();                          // the end of settings_start (and its no-game-folder return)
uint8_t* hud_layer_find(const wchar_t* name);    // d_find_property, for FH2Mod_Hud_ names
bool hud_layer_active();                         // a layer was added this session
void hud_layer_mod_stopped(FH2Mod* mod);         // go_online: its layer lapses at the next frame (HudEngine::stop_layer)
const char* hud_layer_crash_text();              // crash.cpp: a fixed buffer, "" without a layer
std::string hud_layer_session_text();            // session.cpp: "" without a layer
int hud_layer_choice();                          // the Button prompts row: 0 Automatic, 1 Controller, 2 Keyboard and mouse
void hud_layer_refresh_text();                   // the Button prompts row's text in the game's language, before the pages are written
double hud_layer_now();                          // the layer's clock, ms
void hud_layer_sweep_check();                    // the device sweep's round: says once if the pump stopped

// hotkeys.cpp: the device sweep, sharing the hotkeys' poll thread
void device_sweep_start();
int device_auto_kbd();                           // -1 not known yet, 0 pad, 1 keyboard and mouse

// settings.cpp: wrappers over its own code
uint8_t* settings_hud_descriptor(const char* name, void* getter, int kind);   // 0 int, 1 bool, 2 float, 3 String
bool settings_notify(void* obj, void* desc, uint32_t* code);                  // PropertyChanged; false (with the exception) if it faulted
bool settings_sounds_ok();
bool settings_play_sound(const char* name, uint32_t* code);   // false if not played (code 0: the player isn't ready)
bool settings_balanced(const std::string& xml, std::string& why);
std::string settings_read_ui(const char* name);
int settings_properties_state();                 // 0 not yet, 1 FindProperty hooked ("ready"), -1 it failed

} // namespace fh2platform

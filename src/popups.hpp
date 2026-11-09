// Pop-ups and tips on the game's own HUD notification (the UI templates' renderer G, platform 2.9.0): the glue
// between the queue's rules (popup_queue.cpp) and the game. See popups.cpp.
#pragma once

#include "fh2mod.h"
#include "popup_queue.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fh2platform {

// The API's members (ui_templates.cpp), from any thread.
uint32_t popups_status(FH2Mod* mod);
int popups_show_toast(FH2Mod* mod, const FH2Toast* toast);
void popups_clear_toast(FH2Mod* mod, const char* key);
int popups_show_tip(FH2Mod* mod, const FH2Tip* tip);
int popups_tip_seen(FH2Mod* mod, const char* key);
void popups_set_tip_seen(FH2Mod* mod, const char* key, int seen);

// load_mods, once every fh2mod_init has returned: reads fh2-mods\platform-popups.ini and starts the hooks' thread,
// in every session: it waits for the game's code, and hooks only if the game is FH2. A mod's first pop-up may come
// mid-session, after the notification component was made and the HUD page shown, which only these hooks see. The
// controller poll for tips starts here when a mod declared tips, otherwise with the first show_tip.
void popups_prepare(const FH2ModApi* api);
// Whether the pop-ups' page tracking hooks the free-roam HUD's vtable slots this session (settings.cpp: the
// notice's own hook then accepts the slot as the exe on disk has it, S5's F1). True from popups_prepare on, before
// the hooks go in.
bool popups_track_page();
// Whether the free-roam HUD page is on screen now: 1 or 0 while the pop-ups' page tracking is in, -1 when the platform
// doesn't know (no mod declared pop-ups or tips, or the tracking didn't go in). play_ui_sound's world-only sounds.
int popups_hud_on_screen();

// The platform is about to call one of the mod's callbacks (REPLY's 1 s window), or caught a crash in one (its
// waiting pop-ups and tip go). Any thread.
void popups_note_callback(FH2Mod* mod);
void popups_mod_faulted(FH2Mod* mod);

// For the player's rows (the settings restructure): "Mod pop-ups", a mod's own "Pop-ups", "Tips", "Show tips again"
// and "Recent pop-ups". Every switch is on until one of these says otherwise.
void popups_set_platform_toasts(bool on);
void popups_set_mod_toasts(FH2Mod* mod, bool on);
void popups_set_tips(bool on);
int popups_tips_again();                  // how many tips were forgotten
std::vector<PopupRecent> popups_recent(); // the last 10 shown, newest first
std::string popups_describe();            // what's on screen and waiting, and why ("fh2mod Platform ui")

// The UI templates' developer tools (ui_player.cpp, Developer=1): buttons in tips written for the keyboard (1), the
// pad (0) or as normal (-1: the HUD layer's Button prompts row when the player chose a device there, else the device
// used last); and that device, as tips write it now (-1 not known yet).
void popups_force_device(int keyboard);
int popups_device();
// Whether the hooks for pop-ups and tips are on their way this session (a mod declared them, or Developer=1).
bool popups_started();

} // namespace fh2platform

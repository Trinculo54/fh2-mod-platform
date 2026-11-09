// The player's controls for pop-ups and tips, and the UI templates' developer tools (platform 2.9.0). See
// ui_player.cpp.
#pragma once

#include "fh2mod.h"

#include <string>
#include <vector>

namespace fh2platform {

// The platform itself as a mod, "Platform": its own command ("fh2mod Platform ui"), the test page's pop-ups and its
// own pages under YOUR MODS (Recent pop-ups, UI templates). Never one of the MODS tab's mods.
FH2Mod* platform_self();

// Once every fh2mod_init has returned, before popups_prepare: reads fh2-mods\platform-popups.ini (the player's
// choices, and Developer=1), gives the pop-up queue the player's choices, and adds the platform's command.
void ui_player_start(const FH2ModApi* api);
// settings_start, before the pages are first written: the platform's own pages (Recent pop-ups, and with Developer=1
// UI templates).
void ui_player_pages();
// Developer=1 in fh2-mods\platform-popups.ini's [popups]: the UI templates test page, and the ui command's tests.
bool ui_developer();

// The text of one of the platform's rows, in the game's language now.
struct PlayerText {
    std::string label, description, hint;
    std::vector<std::string> choices;   // a choice row's, else none (an On/Off row)
};

// The platform's two rows on its page: "Mod pop-ups" (a choice: Same as Notifications, Off) and "Tips" (On/Off).
enum class PlayerRow { mod_popups, tips };
PlayerText player_row_text(PlayerRow row);
int player_row_value(PlayerRow row);                  // as the row shows it: the choice's index, or 1 for On
void player_row_changed(PlayerRow row, int value);   // the player changed it (UI thread): at once, and kept

// A mod's own "Pop-ups" row (On/Off), on its page: for a mod that declared FH2_UI_USES_TOASTS without
// FH2_UI_USES_OWN_SWITCH. mod_popups_start gives the queue the player's choice from last time and returns the row's
// value; mod_popups_changed is the player changing it (UI thread).
bool mod_gets_popups_row(const FH2Mod* mod);
PlayerText mod_popups_text(const std::string& mod_name);
int mod_popups_start(FH2Mod* mod, const std::wstring& dll);
void mod_popups_changed(FH2Mod* mod, const std::wstring& dll, int value);

// The platform's page under YOUR MODS, as settings.cpp lays it out: its line in the list of mods, then its rows in
// order. A heading; one of the two rows above; an action, A on which calls player_select(key); or one of the
// platform's own pages (key: its page key), A on which shows it.
PlayerText platform_line();
struct PlatformLine {
    enum Kind { heading, mod_popups, tips, action, page } kind;
    std::string key;
    PlayerText text;
};
std::vector<PlatformLine> platform_page_lines();
// A on an action row of the platform's page (UI thread). Returns whether it was one.
bool player_select(const std::string& key);
// A page of the platform's own (by key): its title, and its row's label and description, in the game's language now.
bool platform_page_text(const std::string& key, std::string& title, std::string& label, std::string& description);

} // namespace fh2platform

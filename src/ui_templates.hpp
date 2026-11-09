// The UI templates (API 10, platform 2.9.0): pop-ups, tips, message boxes, UI sounds, and the help bar and hint
// line of the mods' pages. See ui_templates.cpp, and the design in docs/api.md's "UI templates" section.
#pragma once

#include "fh2mod.h"

#include <cstdint>

namespace fh2platform {

// Puts the UI templates' members into the API (platform.cpp, with the rest of g_api).
void fill_ui_api(FH2ModApi& api);

// What a mod said it uses with ui_uses (FH2_UI_USES_ bits), or 0: whether its page gets a "Pop-ups" row, and
// whether the controller poll for the device used last has to run. Any thread.
uint32_t ui_uses_of(const FH2Mod* mod);
// Whether any loaded mod said it uses `bits` (any of them).
bool any_mod_uses(uint32_t bits);

// The platform is about to call one of a mod's own callbacks: a hotkey's on_press or on_rebind, a command, a page,
// setting, title-screen or message box callback. A pop-up sent within 1 s of it may be a reply
// (FH2_TOAST_FLAG_REPLY). Any thread; call it just before the callback.
void ui_note_callback(FH2Mod* mod);
// The platform caught a crash in one of a mod's callbacks: its waiting pop-ups and tip are dropped (and, as the
// other templates are built, what else of its the design takes down). Any thread.
void ui_mod_faulted(FH2Mod* mod);
// The mod was stopped for online play (platform.cpp's go_online): its waiting pop-ups, tips and UI sounds are
// dropped, with a line in its name. Any thread.
void ui_mod_stopped(FH2Mod* mod);
// How many times the platform has caught a crash in the mod's callbacks so far (a message box doesn't call the
// on_choose of a mod that faulted after it was shown).
uint32_t ui_mod_faults(const FH2Mod* mod);

// Which of a mod's callbacks the platform is in, on the game's UI thread (settings.cpp sets it around each one):
// show_message_box, play_ui_sound and show_page_hint ask, since a box shows only from these, a sound from them
// plays at once, and a hint changed from the page's own shows at once.
enum class UiCb {
    page,           // a page's on_select, on_button or on_slot, with the page up
    page_open_all,  // a page's on_open as YOUR MODS opens: every mod's pages are asked at once, none of them shown
    page_open,      // a page's on_open as its tile opens it: the page is on its way, not loaded yet
    setting,        // a setting's on_change or on_slide
    title,          // a title-screen entry's on_press or on_choose
    box,            // a message box's on_choose
};
struct UiCallbackState {
    FH2Mod* mod = nullptr;
    UiCb kind   = UiCb::page;
    void* page  = nullptr;       // the game's page the callback came from (what a box shows over), or null
    int page_index = -1;         // the mod's page (settings.cpp's g_pages), or -1
    bool box_shown = false;      // a message box was shown from it, or from a callback inside it
    UiCallbackState* outer = nullptr;
};
// Set for as long as it lives (callbacks can nest: a slot set from on_button comes back through on_slot). Keep it
// out of functions with __try.
class UiCallback {
public:
    UiCallback(FH2Mod* mod, UiCb kind, void* page = nullptr, int page_index = -1);
    ~UiCallback();
    UiCallback(const UiCallback&)            = delete;
    UiCallback& operator=(const UiCallback&) = delete;
    UiCallbackState state;
};
// The innermost callback this thread is in, or null.
UiCallbackState* ui_callback_now();

} // namespace fh2platform

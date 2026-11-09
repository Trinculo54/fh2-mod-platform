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

} // namespace fh2platform

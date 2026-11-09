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

} // namespace fh2platform

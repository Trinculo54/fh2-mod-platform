// The UI templates (API 10, platform 2.9.0): the members of fh2mod.h's "UI templates" block, each passed on to the
// file that does it:
//  - ui_status, the pop-ups (show_toast, clear_toast) and the tips (show_tip, tip_seen, set_tip_seen): popups.cpp,
//    with their rules in popup_queue.cpp;
//  - play_ui_sound: ui_sounds.cpp;
//  - show_message_box, set_page_help, set_page_hints and show_page_hint: settings.cpp, where the pages and the game's
//    message box are; their rules are ui_rules.cpp's.
// ui_uses is here: it records what each mod said it uses, for the "Pop-ups" rows and the controller poll. So is the
// callback scope (UiCallback) that tells the others which of a mod's callbacks the platform is in.
#include "ui_templates.hpp"

#include "platform.hpp"
#include "popups.hpp"
#include "ui_sounds.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace fh2platform {
namespace {

// FH2Prompt is frozen: it's embedded in FH2Tip and passed in arrays, so its size and layout never change.
static_assert(sizeof(FH2Prompt) == 56 && offsetof(FH2Prompt, hotkey) == 16 && offsetof(FH2Prompt, reserved) == 36,
              "FH2Prompt is frozen");
static_assert(offsetof(FH2Tip, button) == 24, "FH2Tip's members before the prompt");

// ---- ui_uses -------------------------------------------------------------------------------------------------

constexpr uint32_t kKnownUses = FH2_UI_USES_TOASTS | FH2_UI_USES_TIPS | FH2_UI_USES_OWN_SWITCH;

std::mutex g_mu;
std::map<const FH2Mod*, uint32_t> g_uses;   // by mod, what ui_uses said (every call's bits together)

std::string uses_text(uint32_t uses) {
    std::string s;
    auto add = [&](const char* what) { s += (s.empty() ? "" : ", ") + std::string(what); };
    if (uses & FH2_UI_USES_TOASTS) add("pop-ups");
    if (uses & FH2_UI_USES_TIPS) add("tips");
    if (uses & FH2_UI_USES_OWN_SWITCH) add("a pop-ups row of its own");
    return s.empty() ? "nothing" : s;
}

int api_ui_uses(FH2Mod* m, uint32_t uses) {
    if (!m) return 0;
    if (!in_mod_init()) {
        log_line(m->name, "ui_uses wasn't called from fh2mod_init, so it's ignored: call it there, before fh2mod_init returns");
        return 0;
    }
    if (const uint32_t unknown = uses & ~kKnownUses) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%X", unknown);
        log_line(m->name, std::string("ui_uses: ") + hex + " isn't a bit this platform knows, so it's ignored");
    }
    uint32_t all;
    {
        std::scoped_lock lk{g_mu};
        all = g_uses[m] |= uses & kKnownUses;
    }
    log_line(m->name, "UI templates: it uses " + uses_text(all));
    return 1;
}

// ---- pop-ups and tips (popups.cpp) ---------------------------------------------------------------------------

uint32_t api_ui_status(FH2Mod* m) { return m ? popups_status(m) : 0; }
int api_show_toast(FH2Mod* m, const FH2Toast* t) { return popups_show_toast(m, t); }
void api_clear_toast(FH2Mod* m, const char* key) { popups_clear_toast(m, key); }
int api_show_tip(FH2Mod* m, const FH2Tip* t) { return popups_show_tip(m, t); }
int api_tip_seen(FH2Mod* m, const char* key) { return popups_tip_seen(m, key); }
void api_set_tip_seen(FH2Mod* m, const char* key, int seen) { popups_set_tip_seen(m, key, seen); }

// ---- play_ui_sound (ui_sounds.cpp), message boxes, page help and hint lines (settings.cpp) -------------------

int api_play_ui_sound(FH2Mod* m, uint32_t sound) { return ui_sounds_play(m, sound); }
int api_show_message_box(FH2Mod* m, const FH2MessageBox* box) { return settings_show_box(m, box); }
int api_set_page_help(FH2Mod* m, const char* key, const FH2Prompt* prompts, uint32_t count) { return pages_set_help(m, key, prompts, count); }
int api_set_page_hints(FH2Mod* m, const char* key, const char* const* texts, uint32_t count) { return pages_set_hints(m, key, texts, count); }
int api_show_page_hint(FH2Mod* m, const char* key, int index, uint32_t kind) { return pages_show_hint(m, key, index, kind); }

// ---- the callback scope and faults -------------------------------------------------------------------------------

thread_local UiCallbackState* g_callback = nullptr;
std::mutex g_faults_mu;
std::map<const FH2Mod*, uint32_t> g_faults;

} // namespace

void fill_ui_api(FH2ModApi& api) {
    api.ui_status        = api_ui_status;
    api.ui_uses          = api_ui_uses;
    api.play_ui_sound    = api_play_ui_sound;
    api.show_toast       = api_show_toast;
    api.clear_toast      = api_clear_toast;
    api.show_tip         = api_show_tip;
    api.tip_seen         = api_tip_seen;
    api.set_tip_seen     = api_set_tip_seen;
    api.show_message_box = api_show_message_box;
    api.set_page_help    = api_set_page_help;
    api.set_page_hints   = api_set_page_hints;
    api.show_page_hint   = api_show_page_hint;
    ui_sounds_prepare(&api);
}

uint32_t ui_uses_of(const FH2Mod* mod) {
    std::scoped_lock lk{g_mu};
    const auto it = g_uses.find(mod);
    return it == g_uses.end() ? 0 : it->second;
}

bool any_mod_uses(uint32_t bits) {
    std::scoped_lock lk{g_mu};
    for (const auto& [m, uses] : g_uses)
        if (uses & bits) return true;
    return false;
}

void ui_note_callback(FH2Mod* mod) { popups_note_callback(mod); }
void ui_mod_faulted(FH2Mod* mod) {
    {
        std::scoped_lock lk{g_faults_mu};
        ++g_faults[mod];
    }
    popups_mod_faulted(mod);
}

uint32_t ui_mod_faults(const FH2Mod* mod) {
    std::scoped_lock lk{g_faults_mu};
    const auto it = g_faults.find(mod);
    return it == g_faults.end() ? 0 : it->second;
}

UiCallback::UiCallback(FH2Mod* mod, UiCb kind, void* page, int page_index) {
    state.mod        = mod;
    state.kind       = kind;
    state.page       = page;
    state.page_index = page_index;
    state.outer      = g_callback;
    g_callback       = &state;
}
UiCallback::~UiCallback() { g_callback = state.outer; }

UiCallbackState* ui_callback_now() { return g_callback; }

} // namespace fh2platform

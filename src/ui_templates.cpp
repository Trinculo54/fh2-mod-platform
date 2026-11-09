// The UI templates (API 10, platform 2.9.0): the members of fh2mod.h's "UI templates" block.
//
// Being built. Each member is in g_api from the start, so the header, docs/api.md and the platform test agree on
// them while the templates are written. Until a member is done its body here does nothing: it returns 0 and says
// once in platform.log, in the name of the first mod to call it, that it isn't in this build yet. Replace a stub
// with the real call (in a file of its own if it's big: the pop-up queue, tips, boxes, page help) and take its line
// out of kStubs; docs/api.md drops its "(being built)" at the same time.
//
// ui_uses is real: it records what each mod said it uses, for the "Pop-ups" rows and the controller poll. So are
// ui_status, the pop-ups (show_toast, clear_toast) and the tips (show_tip, tip_seen, set_tip_seen): popups.cpp; and
// play_ui_sound: ui_sounds.cpp. So is the callback scope (UiCallback) that tells the others which of a mod's callbacks
// the platform is in.
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

// The members still being built, and what the log says each does meanwhile.
enum Stub {
    kShowMessageBox,
    kSetPageHelp,
    kSetPageHints,
    kShowPageHint,
    kStubCount
};
struct StubInfo {
    const char* name;
    const char* meanwhile;
};
constexpr StubInfo kStubs[kStubCount] = {
    {"show_message_box", "so no box shows and it returns 0"},
    {"set_page_help", "so the help bar stays as it was and it returns 0"},
    {"set_page_hints", "so it returns 0"},
    {"show_page_hint", "so it returns 0"},
};
std::atomic<bool> g_said[kStubCount];

void not_built(FH2Mod* m, Stub s) {
    if (g_said[s].exchange(true)) return;
    log_line(m ? m->name : std::string(), std::string(kStubs[s].name) + " isn't in this build yet, " + kStubs[s].meanwhile);
}

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

int api_play_ui_sound(FH2Mod* m, uint32_t sound) { return ui_sounds_play(m, sound); }

// ---- the callback scope and faults -------------------------------------------------------------------------------

thread_local UiCallbackState* g_callback = nullptr;
std::mutex g_faults_mu;
std::map<const FH2Mod*, uint32_t> g_faults;

// ---- the stubs -----------------------------------------------------------------------------------------------

int api_show_message_box(FH2Mod* m, const FH2MessageBox*) {
    not_built(m, kShowMessageBox);
    return 0;
}
int api_set_page_help(FH2Mod* m, const char*, const FH2Prompt*, uint32_t) {
    not_built(m, kSetPageHelp);
    return 0;
}
int api_set_page_hints(FH2Mod* m, const char*, const char* const*, uint32_t) {
    not_built(m, kSetPageHints);
    return 0;
}
int api_show_page_hint(FH2Mod* m, const char*, int, uint32_t) {
    not_built(m, kShowPageHint);
    return 0;
}

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
UiCallback::~UiCallback() { g_callback = const_cast<UiCallbackState*>(state.outer); }

UiCallbackState* ui_callback_now() { return g_callback; }


} // namespace fh2platform

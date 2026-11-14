// PadMod.dll, for tests/pad_hook_test.cpp: one pad layer (LB with the d-pad) that logs each press with
// what buttons() says, and a layer the platform has to refuse. Its first press also sends a pop-up it never
// declared (API 10), which must find the platform's pop-up hooks in.
#include "fh2mod.h"
#include "fh2mod_pad.h"

#include <cstdio>

namespace {

const FH2ModApi* g_api       = nullptr;
FH2Mod* g_mod                = nullptr;
const FH2PadInterface* g_pad = nullptr;

void on_press(void*, const char* key, uint32_t button) {
    uint32_t held = 0;
    const int ok  = g_pad->buttons(&held);
    char line[96];
    std::snprintf(line, sizeof line, "press %s 0x%04X (buttons %d 0x%04X)", key, button, ok, held);
    g_api->log(g_mod, line);
    // API 10: a pop-up from a mod that declared tips but not pop-ups, sent mid-session: the platform's hooks are in
    // (a mod declared tips), so it's queued with a line asking for the declaration, and ui_status has FH2_UI_TOASTS
    static bool toasted = false;
    if (toasted || g_api->api_version < 10) return;
    toasted    = true;
    FH2Toast t{};
    t.size     = sizeof t;
    t.key      = "Press";
    t.title    = "Pad layer";
    t.subtitle = "A press";
    const int queued = g_api->show_toast(g_mod, &t);
    std::snprintf(line, sizeof line, "show_toast %d, then ui_status has pop-ups: %s", queued, g_api->ui_status(g_mod) & FH2_UI_TOASTS ? "yes" : "no");
    g_api->log(g_mod, line);
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "PadMod";
    info.version          = "1.0.0";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;
    if (api->api_version >= 10) api->ui_uses(g_mod, FH2_UI_USES_TIPS);   // so the pop-up hooks go in (against the real exe)
    g_pad = static_cast<const FH2PadInterface*>(api->get_interface(FH2_PAD_INTERFACE, 1));
    if (!g_pad) {
        api->log(g_mod, "no pad interface");
        return 0;
    }
    FH2PadLayer l{};
    l.size     = sizeof l;
    l.key      = "Chat";
    l.modifier = FH2_PAD_LB;
    l.buttons  = FH2_PAD_DPAD_UP | FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT | FH2_PAD_DPAD_RIGHT;
    l.on_press = on_press;
    api->log(g_mod, g_pad->add_layer(g_mod, &l) ? "layer Chat added" : "layer Chat refused");
    FH2PadLayer both = l;   // LB+RB with Up: would be open with Chat, so it must be refused
    both.key         = "Both";
    both.modifier    = FH2_PAD_LB | FH2_PAD_RB;
    both.buttons     = FH2_PAD_DPAD_UP;
    api->log(g_mod, g_pad->add_layer(g_mod, &both) ? "layer Both added" : "layer Both refused");
    return 0;
}

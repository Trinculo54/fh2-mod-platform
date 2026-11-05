// A mod for the platform test's "hud" phase (the HUD layer preview, include/fh2mod_hud.h): it gets the
// interface, adds a layer with every part, tries to add a second one, sets a time-trial editor's frame,
// and, from a thread of its own once every mod has started, tries to add a layer again (too late: the HUD
// page is written once every fh2mod_init has returned).
#include "fh2mod.h"
#include "fh2mod_hud.h"
#include <windows.h>

#include <cstdio>

static const FH2ModApi* g_api;
static FH2Mod* g_mod;
static const FH2HudInterface* g_hud;

static void say(const char* what, int v) {
    char b[160];
    std::snprintf(b, sizeof b, "%s: %d", what, v);
    g_api->log(g_mod, b);
}

static DWORD WINAPI later(LPVOID) {
    g_api->wait_for_mods(30000);
    FH2HudLayer l{};
    l.size  = sizeof l;
    l.parts = FH2_HUD_PART_BAR;
    say("after init", g_hud->add_hud_layer(g_mod, &l));
    return 0;
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    g_api = api;
    FH2ModInfo info{sizeof info, "HudTestMod", "1.0.0", nullptr, FH2MOD_PLATFORM_VERSION, 0};
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    g_hud = static_cast<const FH2HudInterface*>(api->get_interface(FH2_HUD_INTERFACE, FH2_HUD_INTERFACE_VERSION));
    if (!g_hud || g_hud->size < sizeof(FH2HudInterface)) {
        api->log(g_mod, "no interface");
        return 0;
    }
    api->log(g_mod, "interface found");
    FH2HudLayer l{};
    l.size     = sizeof l;
    l.parts    = FH2_HUD_PART_ALL;
    l.lease_ms = 2000;
    say("add_hud_layer", g_hud->add_hud_layer(g_mod, &l));
    say("add_hud_layer again", g_hud->add_hud_layer(g_mod, &l));

    FH2HudState s{};
    s.size              = sizeof s;
    s.tabs[0]           = "Route";
    s.tabs[1]           = "Checkpoints";
    s.tabs[2]           = "Test";
    s.tab               = 1;
    s.line1             = "Checkpoint 2 of 3";
    s.line1_after       = "Moved";
    s.line1_after_style = FH2_HUD_TEXT_WARN;
    s.line2             = "Drag it along the road";
    s.strip             = FH2_HUD_STRIP_RAIL;
    s.strip_left        = "0:00";
    s.strip_right       = "1:30";
    s.mark_count        = 3;
    for (int k = 0; k < 3; ++k) {
        s.marks[k].at     = 0.25f * static_cast<float>(k + 1);
        s.marks[k].shape  = FH2_HUD_MARK_DIAMOND;
        s.marks[k].style  = k == 1 ? FH2_HUD_MARK_CURRENT : FH2_HUD_MARK_NORMAL;
        s.marks[k].number = static_cast<uint16_t>(k + 1);
    }
    s.playhead        = 1;
    s.prompt_count    = 3;
    s.prompts[0]      = {FH2_GLYPH_B, 0, "Backspace", "Leave", 0, {0, 0, 0}};
    s.prompts[1]      = {FH2_GLYPH_X, 0, "Ctrl+Z", "Undo", 0, {0, 0, 0}};
    s.prompts[2]      = {FH2_GLYPH_A, 0, "Space", "Place", 1, {0, 0, 0}};
    s.chips[0].text   = "Editing";
    s.corner.text     = "Trial 1";
    s.countdown_label = "Test run";
    say("set_hud_state", g_hud->set_hud_state(g_mod, &s));
    if (HANDLE t = CreateThread(nullptr, 0, later, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

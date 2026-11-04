// Offline test of the HUD layer preview's template and state (src/hud_template.cpp, src/hud_state.cpp), with
// no game: a fake game (Strings of 0x40 bytes with their count at +0x10, and a binding that reads each told
// String through the getter, as the game's do) and a fake clock drive the engine frame by frame. Run by
// tests/run_hud_layer_test.sh:
//   hud_layer_test <out folder>
// With FH2_MEDIA set to a folder holding UI.zip, the template is checked against UI.zip's own dictionaries
// and spliced into its A_Hud.xaml, and the pages are written to the out folder for hud_template_check.py;
// without it those checks print "skipped" lines and the check runs on a stand-in dictionary that holds just
// the keys the template uses.
//   T1 the template and its check      T6 the screen's owner
//   T2 staging and coalescing          T7 text and key caps
//   T3 Strings and their references    T8 the device judge
//   T4 leases                          T9 pulses
//   T5 parts and CLEAN                 T10 sounds
//   T0 no layer: the engine does nothing
#include "hud_state.hpp"
#include "hud_template.hpp"
#include "strings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace fh2platform;
namespace fs = std::filesystem;

static int fails = 0;
static void check(bool ok, const char* f, ...) __attribute__((format(printf, 2, 3)));
static void check(bool ok, const char* f, ...) {
    char b[1200];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", b);
    if (!ok) ++fails;
}
#define CHECK(x) check((x), "%s", #x)

static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static std::size_t count_of(const std::string& s, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}
static float as_float(uint32_t b) {
    float f = 0;
    std::memcpy(&f, &b, 4);
    return f;
}

// ---- the fake game ------------------------------------------------------------------------------------

struct StrInfo {
    int serial = 0;
    std::wstring text;
    bool freed = false, pinned = false;
};

struct Told {
    uint64_t frame;
    double t;
    std::string name;
    uint32_t bits;
    std::wstring text;
};

struct FakeGame : HudGame {
    HudEngine* e        = nullptr;
    const uint64_t* frm = nullptr;
    const double* now   = nullptr;
    std::map<void*, StrInfo> strs;
    int serial = 0;
    uint64_t made = 0, freed = 0, engine_releases = 0, game_releases = 0, double_releases = 0, used_freed = 0;
    std::vector<int> free_order;   // serials, in the order they were freed
    bool fault_make = false, fault_release = false, no_memory = false, fault_notify = false;
    bool keep = false;                    // the bindings never let go of the Strings they read
    int lag   = 0;                        // the bindings let go of the old String this many frames after the notify
    std::deque<std::pair<uint64_t, void*>> late;   // (the frame it's let go before, the String)
    std::map<int, void*> held;            // what each String binding holds (the last String it read)
    std::vector<Told> told;
    std::function<void(int)> during_notify;   // called from inside notify, after the binding's read

    static volatile LONG* count(void* s) { return reinterpret_cast<volatile LONG*>(static_cast<char*>(s) + 0x10); }
    bool make_string(const wchar_t* t, int n, void** out) override {
        if (fault_make) return false;
        if (no_memory) {
            *out = nullptr;
            return true;
        }
        char* s   = new char[0x40]();   // never deleted, so a release too many shows as a count below 0
        *count(s) = 1;
        StrInfo& i = strs[s];
        i.serial   = ++serial;
        i.text.assign(t, static_cast<std::size_t>(n));
        ++made;
        *out = s;
        return true;
    }
    void add_ref(void* s) override {
        if (strs[s].freed) ++used_freed;
        InterlockedIncrement(count(s));
    }
    void drop(void* s, bool by_engine) {
        const LONG c = InterlockedDecrement(count(s));
        if (c < 0) ++double_releases;
        if (c == 0) {
            StrInfo& i = strs[s];
            i.freed    = true;
            ++freed;
            free_order.push_back(i.serial);
        }
        ++(by_engine ? engine_releases : game_releases);
    }
    bool release(void* s, bool* was_freed) override {
        if (fault_release) return false;
        const bool last = *count(s) == 1;
        drop(s, true);
        *was_freed = last;
        return true;
    }
    int refs(void* s) override { return *count(s); }
    void pin(void* s) override {
        InterlockedExchangeAdd(count(s), 1 << 20);
        strs[s].pinned = true;
    }
    bool notify(int prop) override {
        if (fault_notify) return false;
        const HudProp& p = e->props()[static_cast<std::size_t>(prop)];
        Told t{*frm, *now, p.short_name, 0, {}};
        if (p.type == 's') {
            void* s = e->string(prop, false);   // inside PropertyChanged, on the same thread, as the game does
            if (s) t.text = strs[s].text;
            void*& h = held[prop];
            if (h && !keep) {
                if (lag > 0) late.push_back({*frm + static_cast<uint64_t>(lag), h});
                else drop(h, false);
            }
            h = s;
        } else {
            t.bits = e->value(prop, false);
        }
        told.push_back(t);
        if (during_notify) during_notify(prop);
        return true;
    }
    int held_refs() {   // the references the bindings hold on Strings that aren't the shared empty one
        int n = 0;
        for (auto& [prop, s] : held)
            if (s && !strs[s].pinned) ++n;
        return n;
    }
};

// ---- a rig: one engine, its layers, the fake game and clock ---------------------------------------------

struct StatusCall {
    uint64_t frame;
    double t;
    int layer;
    uint32_t bits;
};

static void on_status_noop(void*, uint32_t) {}

struct Rig {
    double t   = 1000;   // ms; each step is a frame at t, then t moves on
    uint64_t f = 0;
    int page   = 0;
    uint64_t gen = 1;
    bool kbd     = false;
    int crash_status = -1;   // this layer's on_status "crashes"
    std::vector<std::string> lines;
    std::vector<StatusCall> st;
    std::map<uint64_t, uint64_t> carried;   // frame -> values carried over (the stats)
    FakeGame g;
    std::unique_ptr<HudEngine> e;
    int mods[4]      = {};
    std::size_t mark = 0;   // g.told's size before the last step

    Rig() {
        HudEnv env;
        env.game        = &g;
        env.clock       = [this] { return t; };
        env.log         = [this](const std::string& who, const std::string& text) { lines.push_back("[" + who + "] " + text); };
        env.call_status = [this](FH2HudStatusFn fn, void* user, uint32_t bits, uint32_t* code) {
            const int layer = static_cast<int>(reinterpret_cast<intptr_t>(user));
            st.push_back({f, t, layer, bits});
            if (layer == crash_status) {
                *code = 0xC0000005;
                return false;
            }
            fn(user, bits);
            return true;
        };
        e     = std::make_unique<HudEngine>(env);
        g.e   = e.get();
        g.frm = &f;
        g.now = &t;
    }
    int add(const char* name, uint32_t parts, uint32_t lease = 0, bool status = true) {
        const int n = e->layer_count();
        FH2HudLayer l{};
        l.size      = sizeof l;
        l.parts     = parts;
        l.lease_ms  = lease;
        l.on_status = status ? on_status_noop : nullptr;
        l.user      = reinterpret_cast<void*>(static_cast<intptr_t>(n));
        return e->add_layer(&mods[n < 4 ? n : 3], name, &l);
    }
    void build() { e->build(hud_build_props(e->parts())); }
    HudEngine::FrameOut step(bool live = true, double dt = 20) {
        HudEngine::FrameIn in;
        in.now       = t;
        in.frame     = ++f;
        // the game's own work between ticks: lagging bindings let go of what they were due to
        while (!g.late.empty() && g.late.front().first <= f) {
            g.drop(g.late.front().second, false);
            g.late.pop_front();
        }
        in.live      = live;
        in.page      = &page;
        in.page_gen  = gen;
        in.kbd       = kbd;
        in.available = true;
        mark         = g.told.size();
        const HudEngine::FrameOut out = e->frame(in);
        carried[f] = e->take_stats().carried;
        t += dt;
        return out;
    }
    void steps(int n, bool live = true, double dt = 20) {
        for (int i = 0; i < n; ++i) step(live, dt);
    }
    std::vector<Told> last() const { return {g.told.begin() + static_cast<long>(mark), g.told.end()}; }
    int ix(const std::string& n) const { return e->prop_index(n); }
    const HudProp& prop(const std::string& n) const { return e->props()[static_cast<std::size_t>(ix(n))]; }
    // The newest bits on_status was given (hud_status itself would be a call, which renews the lease).
    uint32_t bits(int layer) const {
        for (auto it = st.rbegin(); it != st.rend(); ++it)
            if (it->layer == layer) return it->bits;
        return 0;
    }
    int lines_with(const std::string& s) const {
        int n = 0;
        for (const std::string& l : lines) n += has(l, s);
        return n;
    }
    // The published text of a String property (through the getter, the reference given back).
    std::wstring text(const std::string& n) {
        void* s = e->string(ix(n), false);
        if (!s) return L"<null>";
        std::wstring w = g.strs[s].text;
        g.drop(s, false);
        return w;
    }
    float fval(const std::string& n) const { return as_float(e->value(ix(n), false)); }
    uint32_t val(const std::string& n) const { return e->value(ix(n), false); }
};

static bool told_in(const std::vector<Told>& v, const std::string& name, uint32_t bits) {
    for (const Told& t : v)
        if (t.name == name && t.bits == bits) return true;
    return false;
}
static bool told_name(const std::vector<Told>& v, const std::string& name) {
    for (const Told& t : v)
        if (t.name == name) return true;
    return false;
}
static int index_of(const std::vector<Told>& v, const std::string& name, uint32_t bits) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name && v[i].bits == bits) return static_cast<int>(i);
    return -1;
}
static bool budgeted(const HudProp& p) { return p.role == HudRole::content || p.role == HudRole::kbd; }

// The time-trial editor's bar, about 90 values from nothing.
static FH2HudState bar_frame() {
    FH2HudState s{};
    s.size              = sizeof s;
    s.tabs[0]           = "Route";
    s.tabs[1]           = "Checkpoints";
    s.tabs[2]           = "Props";
    s.tabs[3]           = "Test";
    s.tabs[4]           = "Save";
    s.tab               = 1;
    s.tab_keys[0]       = "Q";
    s.tab_keys[1]       = "E";
    s.line1             = "Checkpoint 2 of 3";
    s.line1_after       = "Moved";
    s.line1_after_style = FH2_HUD_TEXT_WARN;
    s.line2             = "Drag it along the road";
    s.strip             = FH2_HUD_STRIP_RAIL;
    s.strip_left        = "0:00";
    s.strip_right       = "1:30";
    s.strip_after       = "Best 1:24.6";
    s.mark_count        = 8;
    for (int k = 0; k < 8; ++k) {
        s.marks[k].at     = 0.1f + 0.1f * static_cast<float>(k);
        s.marks[k].shape  = FH2_HUD_MARK_DIAMOND;
        s.marks[k].style  = k == 1 ? FH2_HUD_MARK_CURRENT : k > 5 ? FH2_HUD_MARK_DIM : FH2_HUD_MARK_NORMAL;
        s.marks[k].number = static_cast<uint16_t>(k + 1);
    }
    s.playhead     = 1;
    s.prompt_count = 5;
    s.prompts[0]   = {FH2_GLYPH_B, 0, "Backspace", "Leave", 0, {0, 0, 0}};
    s.prompts[1]   = {FH2_GLYPH_Y, 0, "Enter", "Test run", 0, {0, 0, 0}};
    s.prompts[2]   = {FH2_GLYPH_X, 0, "Ctrl+Z", "Undo", 0, {0, 0, 0}};
    s.prompts[3]   = {FH2_GLYPH_DPAD_LEFTRIGHT, 0, "Left/Right", "Move", 0, {0, 0, 0}};
    s.prompts[4]   = {FH2_GLYPH_A, 0, "Space", "Place", 1, {0, 0, 0}};
    return s;
}

// The same bar with almost every value different (all eight tabs, all sixteen marks, other prompts).
static FH2HudState bar_frame_b() {
    static std::vector<std::string> keep;
    keep.clear();
    keep.reserve(64);
    FH2HudState s = bar_frame();
    for (int i = 0; i < 8; ++i) s.tabs[i] = keep.emplace_back("Page " + std::to_string(i + 1)).c_str();
    s.tab               = 6;
    s.tab_keys[0]       = "Z";
    s.tab_keys[1]       = "C";
    s.line1             = "Checkpoint 3 of 3";
    s.line1_after       = "Placed";
    s.line1_after_style = FH2_HUD_TEXT_ACCENT;
    s.line2             = "Drive through it to test";
    s.strip_left        = "0:05";
    s.strip_right       = "2:10";
    s.strip_after       = "Best 1:20.1";
    s.mark_count        = 16;
    for (int k = 0; k < 16; ++k) {
        s.marks[k].at     = 0.03f + 0.06f * static_cast<float>(k);
        s.marks[k].shape  = k % 2 ? FH2_HUD_MARK_DOT : FH2_HUD_MARK_DIAMOND;
        s.marks[k].style  = static_cast<uint8_t>(k % 3);
        s.marks[k].number = static_cast<uint16_t>(100 + k);
    }
    s.prompts[0] = {FH2_GLYPH_MENU, 1, "Esc", "Menu", 0, {0, 0, 0}};
    s.prompts[1] = {FH2_GLYPH_VIEW, 0, "Tab", "Map", 1, {0, 0, 0}};
    s.prompts[2] = {FH2_GLYPH_LT, 0, "Shift+R", "Reset", 0, {0, 0, 0}};
    s.prompts[3] = {FH2_GLYPH_RT, 0, "Up/Down", "Raise", 0, {0, 0, 0}};
    s.prompts[4] = {FH2_GLYPH_LB, 1, "F5", "Ghost", 0, {0, 0, 0}};
    return s;
}

// Every part filled, with as many of the bar's values set as it has (about 160; about 250 in all).
static FH2HudState full_frame() {
    static std::vector<std::string> keep;
    keep.clear();
    keep.reserve(64);
    FH2HudState s{};
    s.size = sizeof s;
    for (int i = 0; i < 8; ++i) s.tabs[i] = keep.emplace_back("Section " + std::to_string(i + 1)).c_str();
    s.tab               = 6;
    s.tab_keys[0]       = "Z";
    s.tab_keys[1]       = "C";
    s.line1             = "Checkpoint 3 of 3";
    s.line1_after       = "Placed";
    s.line1_after_style = FH2_HUD_TEXT_ACCENT;
    s.line2             = "Drive through it to test";
    s.centre            = FH2_HUD_CENTRE_ALERT;
    s.strip             = FH2_HUD_STRIP_RAIL;
    s.strip_left        = "0:05";
    s.strip_right       = "2:10";
    s.strip_after       = "Best 1:20.1";
    s.mark_count        = 16;
    for (int k = 0; k < 16; ++k) {
        s.marks[k].at     = 0.03f + 0.06f * static_cast<float>(k);
        s.marks[k].shape  = FH2_HUD_MARK_DIAMOND;
        s.marks[k].style  = k % 2 ? FH2_HUD_MARK_DIM : FH2_HUD_MARK_CURRENT;
        s.marks[k].number = static_cast<uint16_t>(100 + k);
    }
    s.playhead     = 1;
    s.prompt_count = 5;
    s.prompts[0]   = {FH2_GLYPH_MENU, 1, "Ctrl+Left", "Back", 1, {0, 0, 0}};
    s.prompts[1]   = {FH2_GLYPH_VIEW, 1, "Ctrl+Right", "Next", 1, {0, 0, 0}};
    s.prompts[2]   = {FH2_GLYPH_LT, 1, "Shift+Up", "Raise", 1, {0, 0, 0}};
    s.prompts[3]   = {FH2_GLYPH_RT, 1, "Shift+Down", "Lower", 1, {0, 0, 0}};
    s.prompts[4]   = {FH2_GLYPH_LB, 1, "Alt+Left", "Ghost", 1, {0, 0, 0}};
    for (int i = 0; i < 4; ++i) s.chips[i] = {keep.emplace_back("Chip " + std::to_string(i + 1)).c_str(), 5, static_cast<uint8_t>(i + 1), {}};
    s.corner     = {"Trial 1", 5, 3, {}};
    s.tip_before = "Press";
    s.tip_button = {FH2_GLYPH_A, 1, "Ctrl+S", nullptr, 0, {0, 0, 0}};
    s.tip_after  = "to drop a checkpoint";
    s.row_count  = 8;
    static const char* const labels[8] = {"Route", "Surface", "Weather", "Time", "Traffic", "Laps", "Ghost", "Rewind"};
    static const char* const values[8] = {"Coast road", "Tarmac", "Clear", "Dusk", "Off", "3", "Best", "On"};
    for (int r = 0; r < 8; ++r) s.rows[r] = {labels[r], values[r], r % 2 ? FH2_ROW_DIM : FH2_ROW_NORMAL, {0, 0, 0}};
    s.row_selected    = 2;
    s.panel_footer    = "A to change, B to go back";
    s.countdown       = 3;
    s.countdown_label = "Test run";
    return s;
}

// Next to nothing: one line.
static FH2HudState small_frame() {
    FH2HudState s{};
    s.size  = sizeof s;
    s.line1 = "Ready";
    return s;
}

// ---- T1: the template and its check ---------------------------------------------------------------------

struct Ui {
    bool media = false;
    fs::path zip;
    std::map<std::string, std::string> cache;
};

// A stand-in for AnthemApp.xaml's dictionary with the keys the template uses, of the kinds UI.zip has them
// (Global_Converters, Global_Material, Global_Textures and Shared_Hud.xaml), for a run without FH2_MEDIA.
static std::string stand_in_app() {
    std::string s =
        "<Application x:Class=\"ForzaUI.AnthemApp\" xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
        "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\" xmlns:local=\"clr-namespace:ForzaUI;assembly=\">\r\n"
        "<Application.Resources>\r\n<ResourceDictionary>\r\n"
        "<local:FZBooleanToVisibilityConverter x:Key=\"A_FZBooleanToVisibilityConverter\" InvertResult=\"false\" />\r\n"
        "<local:FZBooleanToVisibilityConverter x:Key=\"A_FZBooleanToVisibilityConverter_Inverse\" InvertResult=\"true\" />\r\n"
        "<local:FZMaterialPresetRoundtangle x:Key=\"A_FZMaterialPresetRoundtangle_HudItem\" CornerRadius=\"5\" />\r\n"
        "<local:FZMaterialPresetGlassRoundtangle x:Key=\"A_FZMaterialPresetGlassRoundtangle_HudItem\" CornerRadius=\"5\" />\r\n";
    for (const char* n : {"HudSmall", "HudMedium", "HudVeryLarge"})
        s += std::string("<Style x:Key=\"A_Style_FZTextBlock_") + n + "\" TargetType=\"{x:Type local:FZTextBlock}\" />\r\n";
    for (const char* n : {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Menu", "View", "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
                          "DPadUpDown", "DPadLeftRight", "L3", "R3", "LeftThumb", "LeftThumb_UpDown", "LeftThumb_LeftRight", "RightThumb",
                          "RightThumb_UpDown", "RightThumb_LeftRight"})
        s += std::string("<ImageSource x:Key=\"A_BitmapImage_ControllerButton_") + n + "\">x</ImageSource>\r\n";
    return s + "</ResourceDictionary>\r\n</Application.Resources>\r\n</Application>\r\n";
}

static HudUi make_ui(Ui& u) {
    HudUi ui;
    ui.read = [&u](const char* name) -> std::string {
        if (!u.media) return std::strcmp(name, "AnthemApp.xaml") == 0 ? stand_in_app() : std::string();
        auto it = u.cache.find(name);
        if (it == u.cache.end()) {
            const std::vector<char> d = unzip_file(u.zip, name);
            it = u.cache.emplace(name, std::string(d.begin(), d.end())).first;
        }
        return it->second;
    };
    return ui;   // no balanced_xml here (that's settings.cpp's): the check's own scanner balances the tags
}

// The test's own tag balance, apart from the check's: start and end tags pair up, comments and
// declarations skipped.
static bool balanced(const std::string& x, std::string& why) {
    std::vector<std::string> open;
    std::size_t i = 0;
    while ((i = x.find('<', i)) != std::string::npos) {
        if (x.compare(i, 4, "<!--") == 0) {
            i = x.find("-->", i);
            if (i == std::string::npos) return why = "a comment isn't closed", false;
            continue;
        }
        if (x.compare(i, 2, "<?") == 0 || x.compare(i, 2, "<!") == 0) {
            i = x.find('>', i);
            if (i == std::string::npos) return why = "a declaration isn't closed", false;
            continue;
        }
        std::size_t e = i + 1;
        char q        = 0;
        for (; e < x.size(); ++e) {
            if (q) {
                if (x[e] == q) q = 0;
            } else if (x[e] == '"' || x[e] == '\'') {
                q = x[e];
            } else if (x[e] == '>') {
                break;
            }
        }
        if (e >= x.size()) return why = "a tag isn't closed", false;
        const bool close = x[i + 1] == '/', self = x[e - 1] == '/';
        std::size_t n0 = i + (close ? 2 : 1), n1 = n0;
        while (n1 < e && !std::isspace(static_cast<unsigned char>(x[n1])) && x[n1] != '/' && x[n1] != '>') ++n1;
        const std::string name = x.substr(n0, n1 - n0);
        if (close) {
            if (open.empty() || open.back() != name) return why = "</" + name + "> closes " + (open.empty() ? "nothing" : "<" + open.back() + ">"), false;
            open.pop_back();
        } else if (!self) {
            open.push_back(name);
        }
        i = e + 1;
    }
    if (!open.empty()) return why = "<" + open.back() + "> isn't closed", false;
    return true;
}

// Which part of 2.9's tally a property's bindings count under.
static const char* section_of(const std::string& n) {
    const auto digit_after = [&](char c) { return n.size() > 1 && n[0] == c && std::isdigit(static_cast<unsigned char>(n[1])); };
    if (n == "Shown") return "root";
    if (n == "Shutter") return "shutter";
    for (const char* p : {"Bar", "BarVis", "Chips", "ChipsVis", "Tip", "TipVis", "Panel", "PanelVis", "Count", "CountVis"})
        if (n == p) return "fades";
    if (digit_after('P') || n == "HoldWord" || n == "HoldAngle" || n == "HoldFill") return "prompts";
    if (n.rfind("Mark", 0) == 0 || n == "StripRow" || n == "Rail" || n == "RailLeft" || n == "RailRight" || n == "RowAfter" ||
        n == "Playhead" || n == "PlayheadX")
        return "strip";
    if (n.rfind("Tab", 0) == 0) return "tabs";
    if (n == "Alert" || n.rfind("L1", 0) == 0 || n == "L2" || n.rfind("Dot", 0) == 0 || n == "InlineAfter") return "middle";
    if (digit_after('C')) return "chips";
    if (n.rfind("Tip", 0) == 0) return "tip";
    if (digit_after('R') || n == "PanelFooter") return "panel";
    if (n.rfind("Count", 0) == 0) return "countdown";
    return "?";
}

// Every {Binding FH2Mod_Hud_<name>...} in the markup, by section.
static std::map<std::string, int> tally(const std::string& markup, int* total) {
    std::map<std::string, int> t;
    *total = 0;
    const std::string b = "{Binding FH2Mod_Hud_";
    for (std::size_t at = markup.find(b); at != std::string::npos; at = markup.find(b, at + 1)) {
        std::size_t e = at + b.size();
        while (e < markup.size() && (std::isalnum(static_cast<unsigned char>(markup[e])) || markup[e] == '_')) ++e;
        ++t[section_of(markup.substr(at + b.size(), e - at - b.size()))];
        ++*total;
    }
    return t;
}

static std::string replace_first(std::string s, const std::string& from, const std::string& to) {
    const std::size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

// The page minus what the splice put in: the two blocks (whole lines, marker to marker) and xmlns:ed's line.
static std::string unspliced(std::string p) {
    const std::pair<const char*, const char*> blocks[] = {
        {"<!-- FH2 Mod Platform: HUD layer resources -->", "<!-- FH2 Mod Platform: end of the HUD layer resources -->"},
        {"<!-- FH2 Mod Platform: HUD layer -->", "<!-- FH2 Mod Platform: end of the HUD layer -->"}};
    for (const auto& [a, b] : blocks) {
        const std::size_t i = p.find(a), j = p.find(b);
        if (i == std::string::npos || j == std::string::npos) return "<markers missing>";
        const std::size_t from = p.rfind('\n', i) + 1, to = p.find('\n', j) + 1;
        p.erase(from, to - from);
    }
    const std::size_t ed = p.find("xmlns:ed=\"http://schemas.microsoft.com/expression/2010/drawing\"");
    if (ed != std::string::npos) {
        const std::size_t from = p.rfind('\n', ed) + 1, to = p.find('\n', ed) + 1;
        p.erase(from, to - from);
    }
    return p;
}

static void write_file(const fs::path& f, const std::string& s) { std::ofstream(f, std::ios::binary) << s; }

static const char* const kEd = "xmlns:ed=\"http://schemas.microsoft.com/expression/2010/drawing\"";

// The splice's own checks on a page: anchors once, where they belong, the page balanced, nothing else changed.
static void check_spliced(const std::string& name, const std::string& before, const std::string& page, uint32_t parts) {
    const bool ring = (parts & (FH2_HUD_PART_BAR | FH2_HUD_PART_COUNTDOWN)) != 0;
    check(count_of(page, "<!-- FH2 Mod Platform: HUD layer resources -->") == 1 && count_of(page, "<!-- FH2 Mod Platform: HUD layer -->") == 1 &&
              count_of(page, "<!-- FH2 Mod Platform: end of the HUD layer -->") == 1,
          "T1 %s: the splice put each block in once", name.c_str());
    const std::size_t res_end = page.find("<!-- FH2 Mod Platform: end of the HUD layer resources -->");
    const std::size_t dict    = page.find("</ResourceDictionary>");
    check(count_of(page, "</ResourceDictionary>") == 1 && res_end < dict && page.find_first_not_of(" \r\n", page.find('\n', res_end)) == dict,
          "T1 %s: the resources end on the line before the one </ResourceDictionary>", name.c_str());
    const std::size_t zp = page.rfind("</local:ZPanel>"), g = zp == std::string::npos ? zp : page.rfind("</Grid>", zp);
    const std::size_t lay_end = page.find("<!-- FH2 Mod Platform: end of the HUD layer -->");
    check(count_of(page, "</local:ZPanel>") == 1 && lay_end < g && page.find_first_not_of(" \r\n", page.find('\n', lay_end)) == g,
          "T1 %s: the layer ends on the line before the last </Grid> before </local:ZPanel>", name.c_str());
    check(count_of(page, kEd) == (ring ? 1u : 0u), "T1 %s: xmlns:ed is in the page %s", name.c_str(), ring ? "once (its parts have rings)" : "not at all");
    std::string why;
    check(balanced(page, why), "T1 %s: the page as spliced balances%s%s", name.c_str(), why.empty() ? "" : ": ", why.c_str());
    check(unspliced(page) == before, "T1 %s: the page minus the two blocks and xmlns:ed is the page as it was", name.c_str());
}

static void t1_template(const fs::path& out) {
    std::printf("\n== T1 the template and its check\n");
    Ui u;
    if (const char* m = std::getenv("FH2_MEDIA"); m && *m && fs::exists(fs::path(m) / "UI.zip")) {
        u.media = true;
        u.zip   = fs::path(m) / "UI.zip";
    }
    std::printf(u.media ? "UI.zip: %s\n" : "%s\n", u.media ? u.zip.string().c_str() : "no FH2_MEDIA: a stand-in dictionary with the template's keys");
    const HudUi ui = make_ui(u);
    const std::string page0 = u.media ? ui.read("Scenes/Anthem/A_Hud.xaml") : std::string();
    if (u.media) check(!page0.empty(), "T1 UI.zip's Scenes/Anthem/A_Hud.xaml read (%zu bytes)", page0.size());

    // 2.9's tally, per part: [content bindings, properties]
    struct PartCount {
        int bindings, props;
    };
    const PartCount per[kHudParts] = {{405, 205}, {60, 37}, {37, 15}, {65, 51}, {4, 6}};
    const std::map<std::string, int> sections = {{"prompts", 215}, {"strip", 103}, {"tabs", 55}, {"middle", 32}, {"chips", 60},
                                                 {"tip", 37},      {"panel", 65},   {"countdown", 4}};
    const char* const section_part[] = {"prompts", "strip", "tabs", "middle", "chips", "tip", "panel", "countdown"};
    const int section_bits[]          = {1, 1, 1, 1, 2, 4, 8, 16};
    std::string pages;
    for (const uint32_t parts : {0x1Fu, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x11u, 0x0Du}) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "%02X (%s)", parts, hud_parts_text(parts).c_str());
        const std::string name = nm;
        const std::vector<HudProp> props = hud_build_props(parts);
        int want_b = 2, want_p = 2;
        for (int k = 0; k < kHudParts; ++k)
            if (parts & (1u << k)) want_b += per[k].bindings + 3, want_p += per[k].props;
        int types[4] = {};
        for (const HudProp& p : props) ++types[p.type == 'b' ? 0 : p.type == 'i' ? 1 : p.type == 'f' ? 2 : 3];
        check(static_cast<int>(props.size()) == want_p, "T1 %s: %zu properties (want %d)", name.c_str(), props.size(), want_p);
        const std::string res = hud_resources_markup(parts), layer = hud_layer_markup(parts);
        HudCheck c;
        std::string why;
        const bool ok = hud_check_markup(res, layer, parts, props, ui, c, why);
        check(ok, "T1 %s: the check passes%s%s", name.c_str(), ok ? "" : ": ", why.c_str());
        int total = 0;
        std::map<std::string, int> t = tally(res + layer, &total);
        check(static_cast<int>(c.bindings) == want_b && total == want_b, "T1 %s: %zu bindings by the check, %d in the markup (want %d)", name.c_str(),
              c.bindings, total, want_b);
        check(c.bindings <= kHudMaxBindings, "T1 %s: %zu bindings, at most 640", name.c_str(), c.bindings);
        // the tally, section by section, so a difference can be traced
        for (int s = 0; s < 8; ++s) {
            const int want = (parts & static_cast<uint32_t>(section_bits[s])) ? sections.at(section_part[s]) : 0;
            check(t[section_part[s]] == want, "T1 %s: %s %d bindings (want %d)", name.c_str(), section_part[s], t[section_part[s]], want);
        }
        int nparts = 0;
        for (int k = 0; k < kHudParts; ++k) nparts += (parts >> k) & 1;
        check(t["fades"] == 3 * nparts && t["root"] == 1 && t["shutter"] == 1 && t["?"] == 0,
              "T1 %s: fades %d (want %d), root %d, shutter %d, unknown %d", name.c_str(), t["fades"], 3 * nparts, t["root"], t["shutter"], t["?"]);
        // every property bound, every int value with its trigger (read from the markup, apart from the check)
        std::string unbound, untriggered;
        for (const HudProp& p : props) {
            if (!has(res + layer, "{Binding " + p.name + "}") && !has(res + layer, "{Binding " + p.name + ","))
                unbound += (unbound.empty() ? "" : ", ") + p.short_name;
            for (int v : p.values)
                if (!has(res + layer, "Binding=\"{Binding " + p.name + "}\" Value=\"" + std::to_string(v) + "\""))
                    untriggered += (untriggered.empty() ? "" : ", ") + p.short_name + "=" + std::to_string(v);
        }
        check(unbound.empty(), "T1 %s: every property of the table is bound%s%s", name.c_str(), unbound.empty() ? "" : "; not: ", unbound.c_str());
        check(untriggered.empty(), "T1 %s: every int value has its trigger%s%s", name.c_str(), untriggered.empty() ? "" : "; not: ",
              untriggered.c_str());
        check(c.distinct == props.size(), "T1 %s: the check saw %zu of %zu properties bound", name.c_str(), c.distinct, props.size());
        if (parts == 0x1F) {
            check(types[0] == 122 && types[1] == 64 && types[2] == 47 && types[3] == 83, "T1 ALL: %d bool, %d int, %d float, %d String (want 122, 64, 47, 83)",
                  types[0], types[1], types[2], types[3]);
            check(c.triggers == 290, "T1 ALL: %zu DataTriggers on the properties (want 290)", c.triggers);
            check(c.used == 40, "T1 ALL: %zu resource keys used (want 40)", c.used);
            if (u.media) check(c.dicts == 43 && c.known == 1096, "T1 ALL: UI.zip's %d dictionaries and %zu keys (want 43 and 1096)", c.dicts, c.known);
        }
        const std::string table = hud_props_text(props);
        check(count_of(table, "\r\n") == props.size() && table.rfind("FH2Mod_Hud_Shown\tbool\tfalse\t\r\n", 0) == 0,
              "T1 %s: the props table has one line a property", name.c_str());
        char file[64];
        std::snprintf(file, sizeof file, "props-%02X.txt", parts);
        write_file(out / file, table);
        if (!u.media) continue;
        std::string page = page0;
        HudCheck c2;
        why.clear();
        const bool spliced = hud_splice(page, parts, props, ui, c2, why);
        check(spliced, "T1 %s: spliced into UI.zip's A_Hud.xaml%s%s", name.c_str(), spliced ? "" : ": ", why.c_str());
        if (!spliced) continue;
        check_spliced(name, page0, page, parts);
        char pf[64];
        std::snprintf(pf, sizeof pf, "A_Hud-%02X.xaml", parts);
        write_file(out / pf, page);
        pages += std::string(pf) + "\t" + file + "\t";
        for (int k = 0; k < kHudParts; ++k)
            if (parts & (1u << k)) pages += std::string(pages.back() == '\t' ? "" : ",") + kHudPartNames[k];
        pages += "\n";
    }
    write_file(out / "pages.txt", pages);
    if (!u.media) {
        std::printf("skipped T1: the check against UI.zip's own dictionaries (FH2_MEDIA isn't set)\n");
        std::printf("skipped T1: the splice into UI.zip's A_Hud.xaml (FH2_MEDIA isn't set)\n");
    }

    // the splice on a page of the same shape, with and without its anchors (the stand-in's keys, or UI.zip's)
    {
        const std::string mini =
            "<Page x:Class=\"ForzaUI.A_Hud\"\r\n      xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"\r\n"
            "      xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"\r\n      xmlns:local=\"clr-namespace:ForzaUI;assembly=\">\r\n"
            "    <Page.Resources>\r\n        <ResourceDictionary>\r\n        </ResourceDictionary>\r\n    </Page.Resources>\r\n"
            "    <local:ZPanel x:Name=\"LayoutRoot\">\r\n        <Grid>\r\n            <Grid />\r\n        </Grid>\r\n    </local:ZPanel>\r\n</Page>\r\n";
        const std::vector<HudProp> all = hud_build_props(FH2_HUD_PART_ALL);
        std::string p = mini, why;
        HudCheck c;
        const bool ok = hud_splice(p, FH2_HUD_PART_ALL, all, ui, c, why);
        check(ok, "T1 a page of A_Hud.xaml's shape: spliced%s%s", ok ? "" : ": ", why.c_str());
        if (ok) check_spliced("the small page", mini, p, FH2_HUD_PART_ALL);
        const auto refused = [&](const char* what, std::string page, uint32_t parts, const std::string& want) {
            const std::string before = page;
            std::string w;
            HudCheck cc;
            const bool r = !hud_splice(page, parts, hud_build_props(parts), ui, cc, w);
            check(r && has(w, want) && page == before, "T1 splice refused, page untouched: %s (\"%s\")", what, w.c_str());
        };
        refused("an empty page", "", FH2_HUD_PART_ALL, "A_Hud.xaml couldn't be read from the game's UI.zip");
        refused("two </ResourceDictionary>", replace_first(mini, "</Page.Resources>", "<ResourceDictionary></ResourceDictionary></Page.Resources>"),
                FH2_HUD_PART_ALL, "insertion points aren't each there once (</ResourceDictionary> 2, </local:ZPanel> 1)");
        refused("no </local:ZPanel>", replace_first(replace_first(mini, "<local:ZPanel x:Name=\"LayoutRoot\">", "<Grid>"), "</local:ZPanel>", "</Grid>"),
                FH2_HUD_PART_ALL, "</local:ZPanel> 0");
        refused("rings but no xmlns:local", replace_first(mini, "xmlns:local=\"clr-namespace:ForzaUI;assembly=\"", "xmlns:l=\"x\""), FH2_HUD_PART_BAR,
                "xmlns:local wasn't found once");
        std::string q = replace_first(mini, "xmlns:local=\"clr-namespace:ForzaUI;assembly=\"", "xmlns:l=\"x\"");
        why.clear();
        check(hud_splice(q, FH2_HUD_PART_CHIPS, hud_build_props(FH2_HUD_PART_CHIPS), ui, c, why) && !has(q, kEd),
              "T1 the chips alone need no xmlns:local and get no xmlns:ed%s%s", why.empty() ? "" : ": ", why.c_str());
    }

    // broken markups: each refused with its own first problem
    {
        const uint32_t all = FH2_HUD_PART_ALL;
        const std::vector<HudProp> props = hud_build_props(all);
        const std::string res = hud_resources_markup(all), layer = hud_layer_markup(all);
        const std::string root_tag = "<Grid x:Name=\"FH2HL_Root\" Style=\"{StaticResource FH2HL_Root}\">";
        const std::string plus     = "<local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_HudSmall}\" Text=\"+\"";
        const std::string l1s2     = "Binding=\"{Binding FH2Mod_Hud_L1Style}\" Value=\"2\"";
        const std::string end      = "<!-- FH2 Mod Platform: end of the HUD layer -->";
        struct Broken {
            const char* what;
            std::string res, layer;
            uint32_t parts;
            std::size_t cap;
            const char* want;
        };
        const Broken cases[] = {
            {"an unknown resource key", res, replace_first(layer, "A_Style_FZTextBlock_HudSmall", "A_Style_FZTextBlock_HudTiny"), all, 640,
             "the resource key A_Style_FZTextBlock_HudTiny isn't in UI.zip's application dictionaries"},
            {"an unbalanced tag", res, replace_first(layer, "</StackPanel>", ""), all, 640, "the layer's tags: "},
            {"a Style on an element of another type", res, replace_first(layer, plus, "<local:FZMaterial Style=\"{StaticResource A_Style_FZTextBlock_HudSmall}\" Text=\"+\""),
             all, 640, "the Style A_Style_FZTextBlock_HudSmall (for FZTextBlock) is on a <local:FZMaterial>"},
            {"an inline Style for another type", res,
             replace_first(layer, "<Style TargetType=\"local:FZMaterial\">", "<Style TargetType=\"local:FZTextBlock\">"), all, 640,
             "the Style in a <local:FZMaterial.Style> targets FZTextBlock, not FZMaterial"},
            {"a binding to a name not in the table", res, replace_first(layer, "{Binding FH2Mod_Hud_C0}", "{Binding FH2Mod_Hud_C9}"), all, 640,
             "the binding FH2Mod_Hud_C9 isn't one of this page's properties"},
            {"a bool bound as Text", res, replace_first(layer, "Text=\"{Binding FH2Mod_Hud_C0}\"", "Text=\"{Binding FH2Mod_Hud_C0Shown}\""), all, 640,
             "FH2Mod_Hud_C0Shown is a bool, bound where a String goes"},
            {"an int trigger value not in the list", res, replace_first(layer, l1s2, "Binding=\"{Binding FH2Mod_Hud_L1Style}\" Value=\"7\""), all, 640,
             "a trigger shows FH2Mod_Hud_L1Style = 7, which isn't one of its values"},
            {"a listed value without a trigger", res, replace_first(layer, l1s2, "Binding=\"{Binding FH2Mod_Hud_L1Style}\" Value=\"1\""), all, 640,
             "no trigger shows FH2Mod_Hud_L1Style = 2, one of its values"},
            {"Text=\"HELLO\"", res, replace_first(layer, "Text=\"+\"", "Text=\"HELLO\""), all, 640,
             "Text=\"HELLO\" is a literal text, and only the platform's +, < and > may be"},
            {"two top elements", res, replace_first(layer, end, "<Grid x:Name=\"FH2HL_Root\" Style=\"{StaticResource FH2HL_Root}\" />\r\n" + end), all, 640,
             "the layer block holds 2 top elements, not one (FH2HL_Root)"},
            {"a second top element of another kind", res, replace_first(layer, end, "<Grid />\r\n" + end), all, 640,
             "the layer's top element isn't the FH2HL_Root Grid with its Style and no Visibility of its own"},
            {"a Visibility on the root", res,
             replace_first(layer, root_tag, "<Grid x:Name=\"FH2HL_Root\" Style=\"{StaticResource FH2HL_Root}\" Visibility=\"Visible\">"), all, 640,
             "the layer's top element isn't the FH2HL_Root Grid with its Style and no Visibility of its own"},
            {"the root style changed", replace_first(res, "<Setter Property=\"Visibility\" Value=\"Collapsed\" />", "<Setter Property=\"Visibility\" Value=\"Visible\" />"),
             layer, all, 640, "the FH2HL_Root style isn't exactly collapsed unless FH2Mod_Hud_Shown is true"},
            {"641 bindings (the cap lowered to 587 for the 588)", res, layer, all, 587, "588 bindings, more than the 587 the layer may have"},
            {"ed:Arc with the parts saying no ring (CHIPS)", hud_resources_markup(FH2_HUD_PART_CHIPS),
             replace_first(hud_layer_markup(FH2_HUD_PART_CHIPS), "<local:FZMaterial Preset=\"{DynamicResource A_FZMaterialPresetRoundtangle_HudItem}\" Color=\"#FFFFFFFF\"",
                           "<ed:Arc Width=\"10\" Height=\"10\" />\r\n<local:FZMaterial Preset=\"{DynamicResource A_FZMaterialPresetRoundtangle_HudItem}\" Color=\"#FFFFFFFF\""),
             FH2_HUD_PART_CHIPS, 640, "the markup has an ed:Arc, but its parts (CHIPS) have no ring"},
        };
        for (const Broken& b : cases) {
            HudCheck c;
            std::string why;
            const bool changed = b.res != res || b.layer != layer || b.cap != 640;
            const bool refused = !hud_check_markup(b.res, b.layer, b.parts, b.parts == all ? props : hud_build_props(b.parts), ui, c, why, b.cap);
            check(changed && refused && why.rfind(b.want, 0) == 0, "T1 refused: %s (\"%s\")", b.what, why.c_str());
        }
        HudCheck c;
        std::string why;
        check(hud_check_markup(res, layer, all, props, ui, c, why, 588), "T1 588 bindings pass a cap of 588%s%s", why.empty() ? "" : ": ", why.c_str());
        // and the countdown alone without its ring
        std::string cl = hud_layer_markup(FH2_HUD_PART_COUNTDOWN);
        const std::size_t a0 = cl.find("<ed:Arc "), a1 = cl.find("</ed:Arc>");
        if (a0 != std::string::npos && a1 != std::string::npos) cl.erase(cl.rfind('\n', a0) + 1, cl.find('\n', a1) - cl.rfind('\n', a0));
        why.clear();
        const bool r = !hud_check_markup(hud_resources_markup(FH2_HUD_PART_COUNTDOWN), cl, FH2_HUD_PART_COUNTDOWN, hud_build_props(FH2_HUD_PART_COUNTDOWN),
                                         ui, c, why);
        check(r && why == "the markup has no ed:Arc, but its parts (COUNTDOWN) have rings", "T1 refused: the countdown without its ring (\"%s\")", why.c_str());
    }
}

// ---- T0: no layer ---------------------------------------------------------------------------------------

static void t0_no_layer() {
    std::printf("\n== T0 no layer: the engine does nothing\n");
    Rig r;
    CHECK(r.e->layer_count() == 0 && r.e->parts() == 0);
    uint32_t notified = 0;
    for (int i = 0; i < 50; ++i) notified += r.step(i % 2 == 0).notified;
    check(notified == 0 && r.g.told.empty() && r.lines.empty() && r.st.empty() && r.g.made == 0,
          "T0 50 frames with no layer: %u notifies, %zu log lines, %zu status calls, %llu Strings", notified, r.lines.size(), r.st.size(),
          static_cast<unsigned long long>(r.g.made));
    CHECK(r.e->owner() == -1 && !r.e->hold_wanted() && r.e->holders().empty() && r.e->shown_parts() == 0 && r.e->clean());
    CHECK(hud_build_props(0).size() == 2);   // Shown and Shutter only
}

// ---- T2: staging and coalescing -------------------------------------------------------------------------

static void t2_staging() {
    std::printf("\n== T2 staging and coalescing\n");
    {
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
        r.build();
        r.step();   // the page's first frame: the switches told, hides first
        const std::vector<Told> first = r.last();
        check(first.size() == 13 && told_in(first, "Shown", 0) && told_in(first, "BarVis", 0) && told_in(first, "Shutter", 0),
              "T2 a new page's first frame tells the 13 switches false (%zu told)", first.size());
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        const uint64_t f0 = r.f + 1;
        uint64_t vis_frame = 0;
        int most = 0;
        std::set<std::string> content;
        for (int i = 0; i < 20 && !vis_frame; ++i) {
            r.step();
            int b = 0;
            for (const Told& t : r.last()) {
                const HudProp& p = r.prop(t.name);
                if (budgeted(p)) ++b;
                if (budgeted(p) && p.part == 0) content.insert(t.name);
                if (t.name == "BarVis" && t.bits == 1) vis_frame = r.f;
            }
            most = std::max(most, b);
        }
        const int n      = static_cast<int>(content.size());
        const int frames = static_cast<int>(vis_frame - f0 + 1);
        std::printf("the bar from nothing: %d values, BarVis at frame %d of the show, at most %d budgeted notifies a frame\n", n, frames, most);
        check(n >= 80 && n <= 110, "T2 a whole bar frame from nothing is about 90 values (%d)", n);
        check(most <= 24, "T2 at most 24 budgeted notifies a frame (%d)", most);
        check(vis_frame && frames <= (n + 23) / 24 + 1, "T2 all told within ceil(n / 24) + 1 = %d frames (%d)", (n + 23) / 24 + 1, frames);
        const std::vector<Told> v = r.last();
        const int shown = index_of(v, "Shown", 1), vis = index_of(v, "BarVis", 1), fade = index_of(v, "Bar", 1);
        int last_content = -1;
        for (int i = 0; i < static_cast<int>(v.size()); ++i)
            if (budgeted(r.prop(v[i].name)) && r.prop(v[i].name).part == 0) last_content = i;
        check(shown >= 0 && vis > shown && fade > vis && last_content < shown, "T2 Shown, BarVis and Bar told after the last of the bar's content, in that order, "
                                                                               "in one frame (%d, %d, %d after %d)", shown, vis, fade, last_content);
        const std::size_t after = r.g.told.size();
        r.steps(10);
        bool late = false;   // no content of the bar told after it came in (all of it was told first)
        for (std::size_t i = after; i < r.g.told.size(); ++i) late = late || (budgeted(r.prop(r.g.told[i].name)) && r.prop(r.g.told[i].name).part == 0);
        CHECK(!late);

        r.e->set_state(a, &s);
        r.step();
        check(r.last().empty(), "T2 the same state twice: nothing told the second frame (%zu)", r.last().size());
        s.line2 = "Changed";
        r.e->set_state(a, &s);
        r.step();
        check(r.last().size() == 1 && r.last()[0].name == "L2" && r.last()[0].text == L"Changed", "T2 one text changed: exactly one notify (%zu)",
              r.last().size());
        r.steps(10);
        s.line2 = "Other";
        r.e->set_state(a, &s);
        s.line2 = "Changed";
        r.e->set_state(a, &s);
        r.step();
        check(r.last().empty(), "T2 A -> B -> A before a frame: nothing told (%zu)", r.last().size());
        // published but not told: B, then back to A, while the page isn't live
        s.line2 = "Other";
        r.e->set_state(a, &s);
        r.steps(10, false);
        check(r.text("L2") == L"Other", "T2 while the page isn't live the text is published (the getter gives it)");
        s.line2 = "Changed";
        r.e->set_state(a, &s);
        r.steps(10, false);
        r.step();
        check(r.last().empty(), "T2 B published, then A again, before the page was live: nothing told (%zu)", r.last().size());

        // one slot's text every frame for a second
        r.steps(10);
        const double t0 = r.t;
        double t_last   = t0;
        std::string last_text;
        for (int i = 0; i < 50; ++i) {
            last_text = "Lap time " + std::to_string(i);
            s.line2   = last_text.c_str();
            r.e->set_state(a, &s);
            t_last = r.t;
            r.step();
        }
        r.steps(10);
        int during = 0;
        double told_at = -1;
        std::wstring final_text;
        for (const Told& t : r.g.told)
            if (t.name == "L2" && t.t >= t0) {
                if (t.t <= t_last) ++during;
                told_at    = t.t;
                final_text = t.text;
            }
        check(during <= 10, "T2 one text changed every frame for a second: told %d times in it (at most 10)", during);
        check(final_text == std::wstring(last_text.begin(), last_text.end()) && told_at - t_last <= 100,
              "T2 its last value told %.0f ms after its last change (within 100)", told_at - t_last);
    }
    {
        // the playhead every frame, with 200 values carried over
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
        r.build();
        r.step();
        FH2HudState s = full_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_ALL);
        uint64_t most = 0;
        int frames = 0, with = 0;
        for (int i = 0; i < 16; ++i) {
            r.e->set_value(a, FH2_HUD_PLAYHEAD, 0.05f * static_cast<float>(i + 1));
            r.step();
            if (!r.carried[r.f]) break;
            ++frames;
            most = std::max(most, r.carried[r.f]);
            const float want = 0.05f * static_cast<float>(i + 1) * 1104.0f - 1.5f;
            for (const Told& t : r.last())
                if (t.name == "PlayheadX" && std::fabs(as_float(t.bits) - want) < 0.01f) {
                    ++with;
                    break;
                }
        }
        check(most >= 200, "T2 a full frame of every part from nothing: %llu values carried over at most", static_cast<unsigned long long>(most));
        check(frames > 0 && with == frames, "T2 PlayheadX told every frame while values are carried over (%d of %d)", with, frames);
    }
    {
        // show(INSTANT) with 100 values carried over
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        r.step();
        FH2HudState s = small_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        FH2HudState b = full_frame();
        r.e->set_state(a, &b);
        r.step();
        const uint64_t carried = r.carried[r.f];
        r.e->show(a, FH2_HUD_PART_INSTANT);
        r.step();
        const std::vector<Told> v = r.last();
        check(carried >= 100 && told_in(v, "BarVis", 0) && told_in(v, "Bar", 0) && told_in(v, "Shown", 0),
              "T2 show(INSTANT) with %llu values carried over: BarVis, Bar and Shown false all told the next frame", static_cast<unsigned long long>(carried));
        bool content = false;
        for (const Told& t : v) content = content || budgeted(r.prop(t.name));
        r.steps(5);
        for (const Told& t : r.last()) content = content || budgeted(r.prop(t.name));
        CHECK(!content);   // and the carried content isn't told while the bar is off
    }
}

// ---- T3: Strings ----------------------------------------------------------------------------------------

struct StringsLine {
    unsigned long long in_use = 0, waiting = 0, made = 0, freed = 0, left = 0, off = 0;
    long long refs = 0;
    int most       = 0;
    bool ok        = false;
};
static StringsLine strings_line(const std::string& s) {
    StringsLine l;
    l.ok = std::sscanf(s.c_str(),
                       "Strings: %llu in use (one per text shown), %llu waiting to be let go; made %llu, freed %llu, left with the game %llu, left with "
                       "the page while off screen %llu; the game holds %lld references on the ones in use (at most %d on one)",
                       &l.in_use, &l.waiting, &l.made, &l.freed, &l.left, &l.off, &l.refs, &l.most) == 8;
    return l;
}

// One frame on a thread of its own: false if it didn't finish (a lock held across a notify would never let
// the getter in).
static bool step_within(Rig& r, DWORD ms) {
    struct Arg {
        Rig* r;
    } arg{&r};
    HANDLE h = CreateThread(
        nullptr, 0, [](LPVOID p) -> DWORD {
            static_cast<Arg*>(p)->r->step();
            return 0;
        },
        &arg, 0, nullptr);
    if (!h) return false;
    const bool done = WaitForSingleObject(h, ms) == WAIT_OBJECT_0;
    CloseHandle(h);
    return done;
}

static void t3_strings() {
    std::printf("\n== T3 Strings\n");
    {
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        // the binding reads the getter from inside the notify, and also reads two more properties there
        int inner = 0;
        r.g.during_notify = [&](int) {
            void* s = r.e->string(r.ix("L1"), false);
            if (s) r.g.drop(s, false);
            r.e->value(r.ix("BarVis"), false);
            ++inner;
        };
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        const bool done = step_within(r, 10000);
        check(done && inner > 0, "T3 a notify that calls the getter from inside it completes: no lock is held across it (%d notifies)", inner);
        if (!done) {
            std::printf("\nthe frame didn't finish: stopping here\n%d FAILED\n", fails);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(fails));
        }
        r.g.during_notify = nullptr;
        r.steps(20);
        const StringsLine l0 = strings_line(r.e->strings_line());
        void* empty = r.e->string(-1, false);   // no property: the shared empty String
        r.g.drop(empty, false);
        StringsLine l100;
        for (int i = 1; i <= 1000; ++i) {
            const std::string t = "Lap time " + std::to_string(i);
            s.line2             = t.c_str();
            r.e->set_state(a, &s);
            r.steps(5);   // 100 ms: each change published and told
            if (i == 100) l100 = strings_line(r.e->strings_line());
        }
        r.steps(150);   // 3 s
        const std::string line = r.e->strings_line();
        const StringsLine l    = strings_line(line);
        std::printf("%s\n", line.c_str());
        // the slots with text, from the getter
        std::size_t with_text = 0;
        for (int i = 0; i < static_cast<int>(r.e->props().size()); ++i)
            if (r.e->props()[static_cast<std::size_t>(i)].type == 's') {
                void* p = r.e->string(i, false);
                if (p != empty) ++with_text;
                r.g.drop(p, false);
            }
        check(l.ok && l0.ok && l100.ok, "T3 the Strings line reads as the spec has it");
        check(l.made - l0.made == 1000 && l.freed - l0.freed == 1000, "T3 1000 changes: 1000 Strings made and 1000 freed (%llu, %llu)", l.made - l0.made,
              l.freed - l0.freed);
        check(l.made == l.freed + l.in_use + 1 && l.waiting == 0, "T3 made %llu == freed %llu + in use %llu + the shared empty String", l.made, l.freed,
              l.in_use);
        check(l.left == 0 && l.off == 0, "T3 left with the game: %llu, left with the page while off screen: %llu", l.left, l.off);
        check(l.in_use == with_text && l.in_use == r.e->strings_in_use(), "T3 in use %llu == the slots with text %zu", l.in_use, with_text);
        check(!r.g.strs[empty].freed && r.g.strs[empty].pinned && r.g.refs(empty) >= (1 << 20), "T3 the shared empty String is never freed");
        check(l.refs == l100.refs && l.refs == static_cast<long long>(l.in_use) && l.most == 1,
              "T3 the game's references flat: %lld after 100 changes, %lld after 1000 (one binding on each String in use)", l100.refs, l.refs);
        check(r.g.made == l.made && r.g.freed == l.freed && r.g.double_releases == 0 && r.g.used_freed == 0,
              "T3 the fake game agrees: %llu made, %llu freed, no release too many, no freed String handed out", static_cast<unsigned long long>(r.g.made),
              static_cast<unsigned long long>(r.g.freed));

        // the page not live for 3 s: a retired String goes after 2 s
        const StringsLine p0 = strings_line(r.e->strings_line());
        s.line2              = "Off screen";
        r.e->set_state(a, &s);
        r.step(false);
        const StringsLine p1 = strings_line(r.e->strings_line());
        r.steps(90, false);   // 1.8 s more
        const StringsLine p2 = strings_line(r.e->strings_line());
        r.steps(10, false);   // past 2 s
        const StringsLine p3 = strings_line(r.e->strings_line());
        r.steps(50, false);
        check(p1.waiting == p0.waiting + 1 && p2.waiting == p1.waiting && p3.waiting == p0.waiting && p3.off == p0.off + 1 && p3.left == p0.left,
              "T3 page not live for 3 s: the retired String waits (%llu, %llu at 1.8 s) and is let go after 2 s (%llu; the binding of the page "
              "off screen still holds it: left with the page while off screen %llu, left with the game %llu)",
              p1.waiting, p2.waiting, p3.waiting, p3.off, p3.left);
        {
            // the page back: told the new text, the binding lets go of the old String and the game frees it
            const uint64_t freed0 = r.g.freed;
            r.steps(5);
            check(r.g.freed == freed0 + 1 && r.text("L2") == L"Off screen", "T3 the page back: the String it kept while off screen is freed (%llu)",
                  static_cast<unsigned long long>(r.g.freed - freed0));
        }

        // more than 256 retired: the oldest let go first. 32 texts change every frame with the page not live
        // (each published every 100 ms, so 32 Strings retired every 100 ms); a first round, and 2.2 s for
        // what it retired to go, so the burst's Strings are made and retired in the same order.
        std::vector<std::string> texts;
        const auto round = [&](int i) {
            FH2HudState b = bar_frame_b();
            texts.clear();
            texts.reserve(40);
            for (int k = 0; k < 8; ++k) b.tabs[k] = texts.emplace_back("T" + std::to_string(i) + "-" + std::to_string(k)).c_str();
            b.line1       = texts.emplace_back("Line " + std::to_string(i)).c_str();
            b.line2       = texts.emplace_back("Two " + std::to_string(i)).c_str();
            b.strip_after = texts.emplace_back("After " + std::to_string(i)).c_str();
            for (int k = 0; k < 16; ++k) b.marks[k].number = static_cast<uint16_t>(200 + i * 16 + k);
            for (int k = 0; k < 5; ++k) b.prompts[k].label = texts.emplace_back("Label " + std::to_string(i * 5 + k)).c_str();
            r.e->set_state(a, &b);
            r.step(false);
        };
        round(0);
        r.steps(110, false);
        const std::size_t f0 = r.g.free_order.size();
        unsigned long long most = 0;
        for (int i = 1; i <= 60; ++i) {   // 1.2 s
            round(i);
            most = std::max(most, strings_line(r.e->strings_line()).waiting);
        }
        bool in_order = true;
        for (std::size_t i = f0 + 1; i < r.g.free_order.size(); ++i) in_order = in_order && r.g.free_order[i] > r.g.free_order[i - 1];
        check(most == 256 && r.g.free_order.size() > f0 && in_order,
              "T3 more than 256 retired: at most %llu wait, and the %zu let go meanwhile went oldest first", most, r.g.free_order.size() - f0);

        // teardown: everything released once; then only the shared empty String
        const StringsLine tb = strings_line(r.e->strings_line());
        const uint64_t rel0  = r.g.engine_releases;
        r.e->set_off(HudOff::left_out);
        r.step(false);
        const uint64_t rel1 = r.g.engine_releases;
        r.steps(10);
        check(rel1 - rel0 == tb.in_use + tb.waiting && r.g.engine_releases == rel1 && r.g.double_releases == 0 && r.e->strings_in_use() == 0,
              "T3 teardown releases the %llu in use and the %llu waiting once (%llu releases), and nothing after", tb.in_use, tb.waiting,
              static_cast<unsigned long long>(rel1 - rel0));
        bool only_empty = true;
        for (int i = 0; i < static_cast<int>(r.e->props().size()); ++i)
            if (r.e->props()[static_cast<std::size_t>(i)].type == 's') {
                void* p    = r.e->string(i, false);
                only_empty = only_empty && p == empty;
                if (p) r.g.drop(p, false);
            }
        check(only_empty && r.e->string(r.ix("HoldWord"), false) == empty, "T3 after the teardown the getter hands out only the shared empty String "
                                                                           "(HoldWord included)");
        r.g.drop(empty, false);
        bool live_left = true;   // what's left is held by the bindings only
        for (auto& [p, i] : r.g.strs)
            if (!i.pinned && !i.freed) {
                bool held = false;
                for (auto& [prop, h] : r.g.held) held = held || h == p;
                live_left = live_left && held && r.g.refs(p) == 1;
            }
        CHECK(live_left && r.g.double_releases == 0);
        CHECK(r.e->value(r.ix("BarVis"), false) == 0 && r.e->show(a, FH2_HUD_PART_BAR) == 0);
    }
    {
        // a binding that never lets go: "left with the game" grows by one a change
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        r.g.keep      = true;
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        const StringsLine l0 = strings_line(r.e->strings_line());
        for (int i = 1; i <= 50; ++i) {
            const std::string t = "Kept " + std::to_string(i);
            s.line2             = t.c_str();
            r.e->set_state(a, &s);
            r.steps(5);
        }
        r.steps(110);
        const std::string line = r.e->strings_line();
        const StringsLine l    = strings_line(line);
        check(l.left - l0.left == 50 && l.freed == l0.freed && has(line, "left with the game " + std::to_string(l.left)),
              "T3 a binding that never lets go: left with the game %llu -> %llu over 50 changes, and the line says so", l0.left, l.left);
    }
    {
        // a binding that lets go a few frames after the notify (demo run 1): the platform waits for it, so
        // every old String is freed, none left with the game
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        r.g.lag       = 3;
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        const StringsLine l0 = strings_line(r.e->strings_line());
        for (int i = 1; i <= 50; ++i) {
            const std::string t = "Late " + std::to_string(i);
            s.line2             = t.c_str();
            r.e->set_state(a, &s);
            r.steps(5);
        }
        r.steps(20);
        const StringsLine l = strings_line(r.e->strings_line());
        check(l.freed - l0.freed == 50 && l.left == l0.left && l.waiting == 0 && r.g.double_releases == 0,
              "T3 a binding that lets go 3 frames late: freed %llu -> %llu over 50 changes, left with the game %llu -> %llu, "
              "%llu waiting", l0.freed, l.freed, l0.left, l.left, l.waiting);
    }
    {
        // a String fault: nothing more released, and no Strings from the getter; the bar hidden once
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        r.g.fault_make = true;
        s.line2        = "Fault";
        r.e->set_state(a, &s);
        const uint64_t rel0 = r.g.engine_releases;
        r.step();
        r.steps(5);
        int shown_false = 0;
        for (std::size_t i = r.g.told.size() - std::min<std::size_t>(r.g.told.size(), 40); i < r.g.told.size(); ++i)
            shown_false += r.g.told[i].name == "Shown" && r.g.told[i].bits == 0;
        bool nulls = true;
        for (int i = 0; i < static_cast<int>(r.e->props().size()); ++i)
            if (r.e->props()[static_cast<std::size_t>(i)].type == 's') nulls = nulls && r.e->string(i, false) == nullptr;
        check(r.e->off() && r.e->off_reason() == HudOff::strings_fault && r.g.engine_releases == rel0 && nulls,
              "T3 after a String fault: the layer off, nothing more released, the getter gives nullptr for Strings");
        check(shown_false == 1, "T3 after a String fault the bar is hidden once (the last hide: Shown false told %d time(s))", shown_false);
    }
    {
        // no memory: the slot keeps its old String and tries again next frame
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        r.g.no_memory = true;
        s.line2       = "Later";
        r.e->set_state(a, &s);
        r.steps(3);
        const std::wstring kept = r.text("L2");
        r.g.no_memory = false;
        r.steps(2);
        check(kept == L"Drag it along the road" && r.text("L2") == L"Later" && !r.e->off() &&
                  r.lines_with("[HUD layer] the game gave no memory for a String; trying again next frame") == 1,
              "T3 no memory for a String: the old one kept, the new one made a frame later, logged once");
    }
}

// ---- T4: leases -----------------------------------------------------------------------------------------

// The time on_status first saw a bit for this layer at or after `from`, or -1.
static double first_bit(const Rig& r, int layer, uint32_t bit, double from = 0) {
    for (const StatusCall& c : r.st)
        if (c.layer == layer && c.t >= from && (c.bits & bit)) return c.t;
    return -1;
}

static void t4_lease() {
    std::printf("\n== T4 leases\n");
    {
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 2000);
        r.build();
        r.step();
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.e->hold(a, 1);
        const double last_call = r.t;
        CHECK(r.e->hold_wanted());
        for (int i = 0; i < 160 && !(r.bits(a) & FH2_HUD_LAPSED); ++i) r.step();
        const double at = first_bit(r, a, FH2_HUD_LAPSED);
        const uint64_t lapse_frame = r.f;
        const std::vector<Told> v  = r.last();
        check(at > 0 && at - last_call >= 2000 && at - last_call <= 2020, "T4 shown on a live page with no calls: LAPSED %.0f ms after the last call (2000, never early)",
              at - last_call);
        check(told_in(v, "Bar", 0) && !told_in(v, "BarVis", 0), "T4 the lapse hides the bar with a fade (Bar false, BarVis still true)");
        check(!r.e->hold_wanted() && r.e->holders().empty(), "T4 the lapse removes the hold wish");
        r.steps(30);
        double vis_off = -1;
        for (const Told& t : r.g.told)
            if (t.name == "BarVis" && t.bits == 0 && t.frame > lapse_frame) vis_off = t.t;
        check(vis_off - at >= 240 && vis_off - at <= 270, "T4 ... and BarVis false %.0f ms later", vis_off - at);
        int into = 0;
        uint32_t prev = 0;
        for (const StatusCall& c : r.st)
            if (c.layer == a) {
                into += (c.bits & FH2_HUD_LAPSED) && !(prev & FH2_HUD_LAPSED);
                prev = c.bits;
            }
        check(into == 1 && (r.bits(a) & FH2_HUD_LAPSED) && !(r.bits(a) & FH2_HUD_SHOWN), "T4 on_status called once with LAPSED (%d)", into);
        check(r.lines_with("[TrialEditor] HUD layer: no call for 2.0 s while on screen, so it's hidden and its hold on the game's HUD is let go") == 1,
              "T4 the lapse is logged once");
        check(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 0 && r.lines_with("[TrialEditor] pulse_hud: the layer isn't shown, so there's nothing to blink") == 1,
              "T4 after the lapse pulse_hud says 0, logged: the layer isn't shown");
        // show_hud_layer clears LAPSED
        r.e->show(a, FH2_HUD_PART_BAR);
        r.step();
        CHECK(!(r.bits(a) & FH2_HUD_LAPSED) && (r.bits(a) & FH2_HUD_SHOWN));
        // neither shown nor holding: never lapses
        r.e->show(a, 0);
        r.e->hold(a, 0);
        const double quiet = r.t;
        r.steps(500);   // 10 s
        check(first_bit(r, a, FH2_HUD_LAPSED, quiet) < 0, "T4 a layer neither shown nor holding doesn't lapse in 10 s");
    }
    {
        // 1500 ms live + 5000 ms not live + 400 ms live: not lapsed, then lapsed 100 ms later
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 2000);
        r.build();
        r.step();
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(75);
        r.steps(250, false);
        r.steps(20);
        const bool before = (r.bits(a) & FH2_HUD_LAPSED) != 0;
        const double t0   = r.t;
        r.steps(10);
        const double at = first_bit(r, a, FH2_HUD_LAPSED);
        check(!before && at >= t0 + 80 && at <= t0 + 100, "T4 1500 ms live, 5000 ms off screen, 400 ms live: not lapsed, then lapsed %.0f ms later (100)",
              at - t0 + 20);
    }
    {
        // any call renews the lease, hud_status included
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 2000);
        r.build();
        r.step();
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        const char* const names[] = {"hud_status", "set_hud_value", "set_hud_state", "pulse_hud", "play_ui_sound", "hold_game_hud", "show_hud_layer"};
        for (int k = 0; k < 7; ++k) {
            r.steps(90);   // 1.8 s
            switch (k) {
                case 0: r.e->status(a); break;
                case 1: r.e->set_value(a, FH2_HUD_HOLD, 0.5f); break;
                case 2: r.e->set_state(a, &s); break;
                case 3: r.e->pulse(a, FH2_HUD_PULSE_SHUTTER); break;
                case 4: r.e->play_sound(a, FH2_SOUND_INFO); break;
                case 5: r.e->hold(a, 1); break;
                default: r.e->show(a, FH2_HUD_PART_BAR); break;
            }
            check(first_bit(r, a, FH2_HUD_LAPSED) < 0, "T4 %s renews the lease (no lapse after 1.8 s, then the call)", names[k]);
        }
        r.steps(90);
        CHECK(first_bit(r, a, FH2_HUD_LAPSED) < 0);
        r.steps(20);
        CHECK(first_bit(r, a, FH2_HUD_LAPSED) > 0);   // and with no call it does lapse
    }
    {
        // lease_ms 0 is 5000 ms; out of range is clamped
        Rig r;
        const int a = r.add("Default", FH2_HUD_PART_BAR, 0);
        const int b = r.add("Short", FH2_HUD_PART_BAR, 100);
        const int c = r.add("Long", FH2_HUD_PART_CHIPS, 70000);
        r.build();
        check(r.e->layer_lease(a) == 5000 && r.e->layer_lease(b) == 250 && r.e->layer_lease(c) == 60000,
              "T4 lease_ms 0 is 5000 ms; 100 and 70000 are clamped to 250 and 60000 (%u, %u, %u)", r.e->layer_lease(a), r.e->layer_lease(b),
              r.e->layer_lease(c));
        check(r.lines_with("[Short] add_hud_layer: lease_ms 100 is out of range; using 250") == 1 &&
                  r.lines_with("[Long] add_hud_layer: lease_ms 70000 is out of range; using 60000") == 1 && r.lines_with("[Default]") == 0,
              "T4 the clamps are logged; the default isn't");
        r.step();
        r.e->show(a, FH2_HUD_PART_BAR);
        const double t0 = r.t;
        r.steps(300);
        const double at = first_bit(r, a, FH2_HUD_LAPSED);
        check(at - t0 >= 5000 && at - t0 <= 5020, "T4 a layer added with lease_ms 0 lapses at %.0f ms (5000, never early)", at - t0);
        check(first_bit(r, b, FH2_HUD_LAPSED) < 0 && first_bit(r, c, FH2_HUD_LAPSED) < 0, "T4 the layers never shown nor holding never lapse");
    }
    {
        // a layer that only holds
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 2000);
        r.build();
        r.step();
        CHECK(r.e->hold(a, 1) == 1);
        r.step();
        CHECK(r.e->hold_wanted() && r.e->holders() == "TrialEditor");
        r.steps(110);
        check((r.bits(a) & FH2_HUD_LAPSED) && !r.e->hold_wanted(), "T4 a layer that only holds lapses, and its hold is let go");
        CHECK(r.e->hold(a, 1) == 1);
        r.step();
        check(!(r.bits(a) & FH2_HUD_LAPSED) && r.e->hold_wanted(), "T4 hold_game_hud(1) then clears LAPSED and the hold is wanted again");
        check(r.lines_with("[TrialEditor] hold_game_hud: held") == 2 && r.lines_with("[TrialEditor] hold_game_hud: let go") == 0,
              "T4 each hold logged (\"held\" twice; the lapse logs its own line)");
    }
}

// ---- T5: parts and CLEAN --------------------------------------------------------------------------------

static void t5_parts() {
    std::printf("\n== T5 parts and CLEAN\n");
    {
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS, 60000);
        r.build();
        r.step();
        FH2HudState s = full_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        // fading out: contents kept until the clear time, then BarVis false
        r.e->show(a, 0);
        r.step();
        const double out_at = r.t - 20;
        check(told_in(r.last(), "Bar", 0) && !told_in(r.last(), "BarVis", 0), "T5 the bar fading out: Bar false, BarVis still true");
        s.line2 = "Changed while out";
        r.e->set_state(a, &s);
        double vis_at = -1;
        bool content = false;
        for (int i = 0; i < 30; ++i) {
            r.step();
            for (const Told& t : r.last()) {
                if (t.name == "BarVis" && t.bits == 0) vis_at = t.t;
                content = content || budgeted(r.prop(t.name));
            }
        }
        check(vis_at - out_at >= 250 && vis_at - out_at < 270, "T5 BarVis false at its clear time, %.0f ms after the fade began (250)", vis_at - out_at);
        check(!content && r.text("L2") == L"Drive through it to test", "T5 a content change while OUT isn't told (or published) while the part is OUT or OFF");
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(3);
        const int l2 = [&] {
            for (std::size_t i = r.g.told.size(); i-- > 0;)
                if (r.g.told[i].name == "L2") return static_cast<int>(i);
            return -1;
        }();
        const int vis = [&] {
            for (std::size_t i = r.g.told.size(); i-- > 0;)
                if (r.g.told[i].name == "BarVis" && r.g.told[i].bits == 1) return static_cast<int>(i);
            return -1;
        }();
        check(l2 >= 0 && vis > l2 && r.g.told[static_cast<std::size_t>(l2)].text == L"Changed while out",
              "T5 shown again: the new text told before BarVis true");
        // INSTANT: collapsed in the same frame, Shown with it
        r.e->show(a, FH2_HUD_PART_INSTANT);
        r.step();
        const uint64_t hide = r.f;
        check(told_in(r.last(), "Bar", 0) && told_in(r.last(), "BarVis", 0) && told_in(r.last(), "Shown", 0), "T5 INSTANT collapses the bar in the same frame");
        r.steps(5);
        uint64_t clean = 0;
        for (const StatusCall& c : r.st)
            if (c.layer == a && c.frame >= hide && (c.bits & FH2_HUD_CLEAN) && !clean) clean = c.frame;
        check(clean == hide + 3, "T5 CLEAN exactly 3 frames after Shown is told false (%llu after)", static_cast<unsigned long long>(clean - hide));
        // two parts going out 100 ms apart; Shown false with the last
        r.e->show(a, FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS);
        r.steps(20);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(5);
        r.e->show(a, 0);
        const uint64_t from = r.f;
        r.steps(30);
        double chips_off = -1, bar_off = -1, shown_off = -1;
        int shown_index = -1, bar_index = -1;
        for (std::size_t i = 0; i < r.g.told.size(); ++i) {
            const Told& t = r.g.told[i];
            if (t.frame <= from) continue;
            if (t.name == "ChipsVis" && t.bits == 0) chips_off = t.t;
            if (t.name == "BarVis" && t.bits == 0) bar_off = t.t, bar_index = static_cast<int>(i);
            if (t.name == "Shown" && t.bits == 0) shown_off = t.t, shown_index = static_cast<int>(i);
        }
        check(chips_off > 0 && bar_off - chips_off == 100, "T5 two parts going out 100 ms apart clear %.0f ms apart", bar_off - chips_off);
        check(shown_off == bar_off && shown_index < bar_index, "T5 Shown false goes with the last part (in its frame, hides first)");
    }
    {
        // run 2's case: BAR and CHIPS shown; show(INSTANT, 0); show(COUNTDOWN)
        for (int gap = 0; gap < 2; ++gap) {
            Rig r;
            const int a = r.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
            r.build();
            r.step();
            FH2HudState s = full_frame();
            r.e->set_state(a, &s);
            r.e->show(a, FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS);
            r.steps(30);
            const uint64_t from = r.f;
            r.e->show(a, FH2_HUD_PART_INSTANT);
            if (gap) r.step();
            r.e->show(a, FH2_HUD_PART_COUNTDOWN);
            r.steps(30);
            bool bad = false, count = false;
            for (const Told& t : r.g.told)
                if (t.frame > from) {
                    bad   = bad || ((t.name == "BarVis" || t.name == "ChipsVis") && t.bits == 1);
                    count = count || (t.name == "CountVis" && t.bits == 1);
                }
            check(!bad && count, "T5 run 2's case (%s): no BarVis or ChipsVis true told, the countdown in",
                  gap ? "a frame between the two shows" : "both shows before one frame");
        }
    }
    {
        // the page not live: the states wait, then catch up in order
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS, 60000);
        r.build();
        r.step();
        FH2HudState s = full_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(20);
        r.e->show(a, 0);   // the bar starts out
        r.step();
        CHECK(told_in(r.last(), "Bar", 0));
        const std::size_t before = r.g.told.size();
        r.step(false);
        r.e->show(a, FH2_HUD_PART_CHIPS);   // the chips wanted while the page is off screen
        r.steps(25, false);                 // the bar's clear time passes off screen
        const bool quiet = r.g.told.size() == before;
        const std::size_t from = r.g.told.size();
        r.step();
        const uint64_t live = r.f;
        r.steps(5);
        const std::vector<Told> v(r.g.told.begin() + static_cast<long>(from), r.g.told.end());
        const int bar = index_of(v, "BarVis", 0), chips = index_of(v, "ChipsVis", 1);
        int first_chip = -1, last_chip = -1;
        for (int i = 0; i < static_cast<int>(v.size()); ++i)
            if (r.prop(v[i].name).part == 1 && budgeted(r.prop(v[i].name))) {
                if (first_chip < 0) first_chip = i;
                last_chip = i;
            }
        check(quiet && bar >= 0 && v[static_cast<std::size_t>(bar)].frame == live && first_chip > bar &&
                  v[static_cast<std::size_t>(first_chip)].frame == live && chips > last_chip,
              "T5 off screen the states wait; the first live frame tells BarVis false, then the chips' content, and ChipsVis true follows all of it "
              "(%d, %d, %d, %d)",
              bar, first_chip, last_chip, chips);
        std::string seq;
        for (const Told& t : v)
            if (t.name == "Shown") seq += std::string(t.bits ? " true" : " false") + " (frame +" + std::to_string(t.frame - live) + ")";
        std::printf("(Shown told:%s)\n", seq.empty() ? " nothing" : seq.c_str());
    }
    // A tip or panel fading out keeps the bar's height: the strip row's switch, the bar coming in and the bar
    // collapsing all wait for its clear time.
    const auto first_told = [](const Rig& r, std::size_t from, const std::string& name, uint32_t bits) {
        for (std::size_t i = from; i < r.g.told.size(); ++i)
            if (r.g.told[i].name == name && r.g.told[i].bits == bits) return static_cast<int>(i);
        return -1;
    };
    {
        // the tip goes out while the strip's row comes (NONE -> RAIL): StripRow waits for TipVis false
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_TIP, 60000);
        r.build();
        r.step();
        FH2HudState s = small_frame();
        s.tip_before  = "Press";
        s.tip_button  = {FH2_GLYPH_A, 0, "Space", nullptr, 0, {0, 0, 0}};
        s.tip_after   = "to drop a checkpoint";
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR | FH2_HUD_PART_TIP);
        r.steps(20);
        CHECK(r.val("StripRow") == 0 && r.e->shown_parts() == (FH2_HUD_PART_BAR | FH2_HUD_PART_TIP));
        const std::size_t from = r.g.told.size();
        r.e->show(a, FH2_HUD_PART_BAR);   // the hide first, then the new state (with no tip text)
        FH2HudState t     = small_frame();
        t.strip           = FH2_HUD_STRIP_RAIL;
        t.strip_left      = "0:00";
        t.strip_right     = "1:30";
        r.e->set_state(a, &t);
        r.steps(30);
        const int tip_off = first_told(r, from, "TipVis", 0), row_on = first_told(r, from, "StripRow", 1),
                  tip_fade = first_told(r, from, "Tip", 0);
        check(tip_fade >= 0 && tip_off > tip_fade && row_on > tip_off &&
                  r.g.told[static_cast<std::size_t>(row_on)].t - r.g.told[static_cast<std::size_t>(tip_fade)].t >= 250,
              "T5 the tip fading out keeps the strip's row as it was: StripRow true told after TipVis false (%d, %d, %d)", tip_fade, tip_off, row_on);
        check(r.g.told[static_cast<std::size_t>(row_on)].frame == r.g.told[static_cast<std::size_t>(tip_off)].frame &&
                  r.val("StripRow") == 1 && r.text("RailRight") == L"1:30",
              "T5 ... and StripRow true comes in the frame the tip clears, the timeline's labels already told");
        bool tip_kept = true;   // the tip's words weren't emptied while it faded
        for (std::size_t i = from; i < static_cast<std::size_t>(tip_off); ++i)
            if (r.g.told[i].name == "TipAfter" || r.g.told[i].name == "TipBefore") tip_kept = false;
        CHECK(tip_kept);
    }
    {
        // the bar comes in while the panel goes out: BarVis true waits for PanelVis false
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_PANEL, 60000);
        r.build();
        r.step();
        FH2HudState s = full_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_PANEL);
        r.steps(20);
        const std::size_t from = r.g.told.size();
        const double t0        = r.t;
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(30);
        const int panel_off = first_told(r, from, "PanelVis", 0), bar_on = first_told(r, from, "BarVis", 1);
        check(panel_off >= 0 && bar_on > panel_off && r.g.told[static_cast<std::size_t>(bar_on)].frame == r.g.told[static_cast<std::size_t>(panel_off)].frame &&
                  r.g.told[static_cast<std::size_t>(panel_off)].t - t0 >= 250,
              "T5 the bar coming in waits while the panel fades out, and comes in the frame the panel clears (%d, %d)", panel_off, bar_on);
    }
    {
        // the bar going out first, then the tip: the bar's collapse waits for the tip's
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_TIP, 60000);
        r.build();
        r.step();
        FH2HudState s = full_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR | FH2_HUD_PART_TIP);
        r.steps(20);
        const std::size_t from = r.g.told.size();
        r.e->show(a, FH2_HUD_PART_TIP);   // the bar out
        r.steps(5);                       // 100 ms
        r.e->show(a, 0);                  // the tip out too
        r.steps(30);
        const int bar_off = first_told(r, from, "BarVis", 0), tip_off = first_told(r, from, "TipVis", 0), shown_off = first_told(r, from, "Shown", 0);
        check(bar_off >= 0 && tip_off >= 0 && r.g.told[static_cast<std::size_t>(bar_off)].frame == r.g.told[static_cast<std::size_t>(tip_off)].frame &&
                  shown_off >= 0 && r.g.told[static_cast<std::size_t>(shown_off)].frame == r.g.told[static_cast<std::size_t>(tip_off)].frame,
              "T5 the bar's collapse waits for the tip fading out above it: BarVis and TipVis false in the same frame, Shown with them (%d, %d, %d)",
              bar_off, tip_off, shown_off);
    }
}

// ---- T6: the screen's owner -----------------------------------------------------------------------------

static void t6_owner() {
    std::printf("\n== T6 the screen's owner\n");
    Rig r;
    const int a = r.add("Alpha", FH2_HUD_PART_BAR | FH2_HUD_PART_CHIPS, 3000);
    const int b = r.add("Beta", FH2_HUD_PART_BAR, 60000);
    r.build();
    r.step();
    FH2HudState s = bar_frame();
    r.e->set_state(a, &s);
    r.e->set_state(b, &s);
    r.e->show(a, FH2_HUD_PART_BAR);
    r.steps(10);
    CHECK(r.e->owner() == a && (r.bits(a) & FH2_HUD_SHOWN));
    r.e->show(b, FH2_HUD_PART_BAR);
    r.steps(2);
    check(r.e->owner() == b && (r.bits(a) & FH2_HUD_TAKEN) && !(r.bits(a) & FH2_HUD_SHOWN) && (r.bits(b) & FH2_HUD_SHOWN) && !(r.bits(b) & FH2_HUD_TAKEN),
          "T6 A shows, B shows: A TAKEN and not shown, B SHOWN");
    check(r.lines_with("[Alpha] HUD layer: Beta showed its layer, so this one is hidden") == 1, "T6 A is told why in its log line");
    r.e->show(a, FH2_HUD_PART_BAR);
    r.steps(2);
    check(r.e->owner() == a && (r.bits(b) & FH2_HUD_TAKEN) && !(r.bits(a) & FH2_HUD_TAKEN) && (r.bits(a) & FH2_HUD_SHOWN),
          "T6 A shows again: B TAKEN, A's TAKEN gone");
    r.e->show(b, FH2_HUD_PART_BAR);
    r.steps(2);
    r.e->show(b, 0);
    r.steps(20);
    check(r.e->owner() == -1 && r.e->shown_parts() == 0 && !(r.bits(a) & FH2_HUD_SHOWN) && (r.bits(a) & FH2_HUD_TAKEN),
          "T6 B shows, then hides: nothing shown, and A doesn't come back");
    CHECK(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 0 && r.e->pulse(b, FH2_HUD_PULSE_SHUTTER) == 0);
    // a lapsed layer never owns the screen
    r.e->show(a, FH2_HUD_PART_BAR);
    r.steps(160);   // 3.2 s with no call from A
    check((r.bits(a) & FH2_HUD_LAPSED) && r.e->owner() == -1, "T6 A lapses: it no longer owns the screen");
    r.e->show(b, FH2_HUD_PART_BAR);
    r.steps(3);
    r.e->show(b, 0);
    r.steps(3);
    CHECK(r.e->owner() == -1);   // B's show and hide don't bring the lapsed A back
    // a layer whose on_status crashes is hidden at once and loses its hold
    r.e->show(a, FH2_HUD_PART_BAR);
    r.e->hold(a, 1);
    r.steps(10);
    CHECK(r.e->owner() == a && r.e->hold_wanted());
    r.crash_status = a;
    r.kbd          = true;   // a change of bits (KEYBOARD): on_status is called, and crashes
    r.steps(3);
    const int crashes = r.lines_with("[Alpha] HUD layer: its on_status crashed (exception 0xC0000005); on_status is off, the layer hidden and its hold let go");
    check(crashes == 1 && r.e->owner() == -1 && !r.e->hold_wanted(), "T6 A's on_status crashes: A hidden, its hold let go, logged once");
    check(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 0, "T6 ... and pulse_hud then says 0: the layer the platform hid isn't shown");
    r.kbd = false;
    // the engine off: nobody owns the screen and shows are refused
    r.e->show(b, FH2_HUD_PART_BAR);
    r.steps(3);
    CHECK(r.e->owner() == b);
    r.e->set_off(HudOff::interlock);
    r.steps(2);
    check(r.e->owner() == -1 && r.e->show(b, FH2_HUD_PART_BAR) == 0 && r.e->hold(b, 1) == 0 && r.e->play_sound(b, FH2_SOUND_INFO) == 0 && !(r.bits(b) & FH2_HUD_AVAILABLE),
          "T6 the layer off: no owner; show, hold and sounds refused; AVAILABLE gone");
    CHECK(r.lines_with("[Beta] show_hud_layer: the HUD layer is off this session (see the lines above)") == 1);
}

// ---- T7: text and keys ----------------------------------------------------------------------------------

static void t7_text() {
    std::printf("\n== T7 text and key caps\n");
    const auto caps = [](const wchar_t* k) {
        unsigned f = 0;
        return hud_key_caps(k, &f);
    };
    HudKeyCaps k = caps(L"Ctrl+Z");
    CHECK(k.given && k.two && k.k0 == L"CTRL" && k.join == L"+" && k.k1 == L"Z" && k.angle0 < 0 && k.angle1 < 0);
    k = caps(L"Left/Right");
    CHECK(k.two && k.k0.empty() && k.k1.empty() && k.join == L"/" && k.angle0 == 180 && k.angle1 == 0);
    k = caps(L"Up");
    CHECK(!k.two && k.k0.empty() && k.angle0 == 270);
    k = caps(L"down");
    CHECK(!k.two && k.angle0 == 90);   // any case
    k = caps(L"Num +");
    CHECK(!k.two && k.k0 == L"NUM +");
    k = caps(L"+");
    CHECK(!k.two && k.k0 == L"+");
    k = caps(L"Ctrl+Shift+Z");
    CHECK(k.two && k.k0 == L"CTRL" && k.join == L"+" && k.k1 == L"SHIFT+Z");
    unsigned f = 0;
    k          = hud_key_caps(L"Abcdefghijklm", &f);
    CHECK(k.k0 == L"ABCDEFGHIJKL" && (f & kHudTextCut));
    CHECK(!caps(L"").given);

    Rig r;
    const int a = r.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
    r.build();
    r.step();
    r.e->show(a, FH2_HUD_PART_ALL);
    // capitals: tabs, line1, keys, chips and row labels (Polish included); the rest as given
    const char* pl = "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87";   // "zazolc" with its Polish letters
    const std::wstring PL = L"ZA\x017B\x00D3\x0141\x0106", pl_w = L"za\x017C\x00F3\x0142\x0107";
    FH2HudState s{};
    s.size         = sizeof s;
    s.tabs[0]      = pl;
    s.line1        = pl;
    s.line1_after  = pl;
    s.line2        = pl;
    s.prompt_count = 1;
    s.prompts[0]   = {FH2_GLYPH_A, 0, "Esc", pl, 0, {0, 0, 0}};
    s.chips[0].text = pl;
    s.corner.text   = pl;
    s.row_count     = 1;
    s.rows[0]       = {pl, pl, FH2_ROW_NORMAL, {0, 0, 0}};
    r.e->set_state(a, &s);
    r.steps(3);
    check(r.text("Tab0") == PL && r.text("L1") == PL && r.text("C0") == PL && r.text("C4") == PL && r.text("R0Label") == PL && r.text("P0K0") == L"ESC",
          "T7 capitals for tabs, line1, keys, chips and row labels, Polish included");
    check(r.text("L1After") == pl_w && r.text("L2") == pl_w && r.text("P0Label") == pl_w && r.text("R0Value") == pl_w, "T7 the other texts as given");
    CHECK(r.lines_with("letters outside the set") == 0);   // Polish is in the set seen working

    // each limit cuts, and logs once per field
    Rig q;
    const int b = q.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
    q.build();
    q.step();
    q.e->show(b, FH2_HUD_PART_ALL);
    const std::string long_text(120, 'x');
    const char* L = long_text.c_str();
    FH2HudState t{};
    t.size        = sizeof t;
    t.tabs[0]     = L;
    t.tabs[1]     = L;
    t.tab_keys[0] = L;
    t.tab_keys[1] = L;
    t.line1       = L;
    t.line1_after = L;
    t.line2       = L;
    t.strip       = FH2_HUD_STRIP_RAIL;
    t.strip_left  = L;
    t.strip_right = L;
    t.strip_after = L;
    t.prompt_count = 2;
    t.prompts[0]   = {FH2_GLYPH_A, 0, L, L, 0, {0, 0, 0}};
    t.prompts[1]   = {FH2_GLYPH_B, 0, L, L, 0, {0, 0, 0}};
    t.chips[0].text = L;
    t.corner.text   = L;
    t.tip_before    = L;
    t.tip_after     = L;
    t.tip_button    = {FH2_GLYPH_A, 0, L, nullptr, 0, {0, 0, 0}};
    t.row_count     = 1;
    t.rows[0]       = {L, L, FH2_ROW_NORMAL, {0, 0, 0}};
    t.panel_footer  = L;
    t.countdown_label = L;
    q.e->set_state(b, &t);
    q.e->set_state(b, &t);
    q.steps(8);
    struct Limit {
        const char *field, *prop;
        int n;
    };
    const Limit limits[] = {{"tabs[0]", "Tab0", 16},          {"tab_keys[0]", "TabKeyL", 12},   {"line1", "L1", 40},
                            {"line1_after", "L1After", 24},   {"line2", "L2", 64},              {"strip_left", "RailLeft", 12},
                            {"strip_right", "RailRight", 12}, {"strip_after", "RowAfter", 48},  {"prompts[0].label", "P0Label", 24},
                            {"prompts[0].key", "P0K0", 12},   {"chips[0].text", "C0", 24},      {"corner.text", "C4", 24},
                            {"tip_before", "TipBefore", 48},  {"tip_after", "TipAfter", 48},    {"tip_button.key", "TipK0", 12},
                            {"rows[0].label", "R0Label", 32}, {"rows[0].value", "R0Value", 32}, {"panel_footer", "PanelFooter", 96},
                            {"countdown_label", "CountLabel", 48}};
    for (const Limit& l : limits) {
        const std::string line = std::string("[TrialEditor] set_hud_state: ") + l.field + " is longer than " + std::to_string(l.n) +
                                 " characters; the rest is cut";
        const std::size_t len = q.text(l.prop).size();
        check(static_cast<int>(len) == l.n && q.lines_with(line) == 1, "T7 %s cut to %d (%zu) and logged once", l.field, l.n, len);
    }
    check(q.lines_with("tabs[1] is longer") == 0 && q.lines_with("tab_keys[1] is longer") == 0 && q.lines_with("prompts[1].label is longer") == 0 &&
              q.lines_with("prompts[1].key is longer") == 0,
          "T7 the same field again (tabs[1], tab_keys[1], prompts[1]) isn't logged again");
    check(q.text("TabKeyR").size() == 12 && q.text("P1Label").size() == 24, "T7 ... but is cut");

    // letters, control characters, enums, reserved, rows, dots
    Rig w;
    const int c = w.add("TrialEditor", FH2_HUD_PART_ALL, 60000);
    w.build();
    w.step();
    w.e->show(c, FH2_HUD_PART_ALL);
    FH2HudState u{};
    u.size            = sizeof u;
    u.line2           = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";   // Cyrillic
    u.strip_after     = "\xD0\x94\xD0\xB0";                                  // more Cyrillic, another field
    u.line1           = "A\nB";
    u.line1_after     = "A\tB";
    u.strip           = 7;
    u.prompt_count    = 2;
    u.prompts[0]      = {FH2_GLYPH_A, 0, "Space", "Place", 0, {0, 1, 0}};
    u.prompts[1]      = {FH2_GLYPH_B, 0, "Esc", "Leave", 0, {0, 0, 2}};
    u.row_count       = 2;
    u.rows[0]         = {"Normal", nullptr, FH2_ROW_NORMAL, {0, 0, 0}};
    u.rows[1]         = {"Heading", nullptr, FH2_ROW_HEADING, {0, 0, 0}};
    u.row_selected    = 1;
    u.chips[0]        = {"Dots", 9, 2, {}};
    u.chips[1]        = {"Filled", 3, 7, {}};
    w.e->set_state(c, &u);
    w.e->set_state(c, &u);
    w.steps(8);
    check(w.lines_with("[TrialEditor] set_hud_state: line2 has letters outside the set seen working in the HUD's fonts (U+041F); they may show as boxes") == 1 &&
              w.lines_with("letters outside the set") == 1,
          "T7 Cyrillic logs the letters line once (for the first field), and keeps the letters");
    CHECK(w.text("L2") == L"\x041F\x0440\x0438\x0432\x0435\x0442");   // kept as given
    check(w.text("L1") == L"A B" && w.text("L1After") == L"A B" &&
              w.lines_with("[TrialEditor] set_hud_state: line1 has a line break or another control character; shown as a space") == 1 &&
              w.lines_with("[TrialEditor] set_hud_state: line1_after has a line break or another control character; shown as a space") == 1,
          "T7 \"A\\nB\" and \"A\\tB\" become \"A B\", with the control-character line once each");
    check(w.lines_with("[TrialEditor] set_hud_state: strip = 7 isn't one of its values; 0 used") == 1 && w.val("StripRow") == 0 && w.val("Dots") == 0 &&
              w.val("Rail") == 0,
          "T7 an unknown enum value uses 0 and logs once");
    check(w.lines_with("[TrialEditor] set_hud_state: prompts[0]'s reserved isn't zero") == 1 && w.lines_with("reserved isn't zero") == 1,
          "T7 a reserved field not zero logs once");
    check(w.val("R0Sel") == 0 && w.val("R1Sel") == 0 && w.val("R1Style") == 2, "T7 a selected heading row isn't filled");
    u.row_selected = 0;
    w.e->set_state(c, &u);
    w.steps(8);
    CHECK(w.val("R0Sel") == 1);
    check(w.val("C0D0") == 2 && w.val("C0D1") == 2 && w.val("C0D2") == 1 && w.val("C0D4") == 1 && w.val("C1D0") == 2 && w.val("C1D2") == 2 &&
              w.val("C1D3") == 0 && w.lines_with("[TrialEditor] set_hud_state: chips[0].dots = 9 is more than 5; 5 used") == 1 &&
              w.lines_with("[TrialEditor] set_hud_state: chips[1].dots_filled is more than its dots; all of them filled") == 1,
          "T7 dots clamped to 5 and dots_filled to dots, each logged once");
    // a line1 of 39 letters and an emoji (a surrogate pair at units 40-41): cut to 39 units, not 40
    std::string e39(39, 'e');
    e39 += "\xF0\x9F\x98\x80";
    u.line1 = e39.c_str();
    w.e->set_state(c, &u);
    w.steps(8);
    CHECK(w.text("L1") == std::wstring(39, L'E'));
    // two dots on the strip's row, centred on the screen: centres 744 and 768 in the timeline's column (x 204), so 948 and 972
    u.strip      = FH2_HUD_STRIP_DOTS;
    u.strip_row  = 1;
    u.mark_count = 2;
    w.e->set_state(c, &u);
    w.steps(8);
    check(w.fval("Mark0X") + 20 == 744.0f && w.fval("Mark1X") + 20 == 768.0f && w.val("StripRow") == 1 && w.val("Dots") == 0,
          "T7 two strip_row dots centred at %.1f and %.1f (744 and 768: the screen's middle)", w.fval("Mark0X") + 20, w.fval("Mark1X") + 20);
    // and on line 2 without strip_row: Dot<j> is style + 1
    u.strip_row    = 0;
    u.mark_count   = 3;
    u.marks[1].style = FH2_HUD_MARK_CURRENT;
    u.marks[2].style = FH2_HUD_MARK_DIM;
    w.e->set_state(c, &u);
    w.steps(8);
    CHECK(w.val("Dots") == 1 && w.val("StripRow") == 0 && w.val("Dot0") == 1 && w.val("Dot1") == 2 && w.val("Dot2") == 3 && w.val("Dot3") == 0);
    // a mark's place on the timeline, and the keys' arrows reaching the page
    u.strip          = FH2_HUD_STRIP_RAIL;
    u.mark_count     = 1;
    u.marks[0]       = {0.5f, FH2_HUD_MARK_DIAMOND, 0, 12, 0};
    u.prompts[0].key = "Left/Right";
    w.e->set_state(c, &u);
    w.steps(8);
    CHECK(w.fval("Mark0X") == 532.0f && w.fval("Mark0Angle") == 45.0f && w.text("Mark0Num") == L"12");
    CHECK(w.val("P0K0Arrow") == 1 && w.fval("P0K0Angle") == 180.0f && w.val("P0K1Shown") == 1 && w.val("P0K1Arrow") == 1 && w.fval("P0K1Angle") == 0.0f);
    check(w.val("P0Kbd") == 0 && w.val("P1Kbd") == 0, "T7 a prompt with a glyph shows it on a pad");
    w.kbd = true;
    w.steps(8);
    check(w.val("P0Kbd") == 1, "T7 ... and its key caps on the keyboard");
    // the state too small
    FH2HudState small{};
    small.size = 8;
    CHECK(w.e->set_state(c, &small) == 0 && w.e->set_state(c, &small) == 0 &&
          w.lines_with("[TrialEditor] set_hud_state: the FH2HudState is too small: set size = sizeof(FH2HudState); ignored") == 1);
}

// ---- T8: the device judge -------------------------------------------------------------------------------

static void t8_device() {
    std::printf("\n== T8 the device judge\n");
    HudDeviceJudge j;
    CHECK(j.round("key 0x41", nullptr, 0) && j.kbd);                 // the newest input wins
    CHECK(j.round(nullptr, "buttons", 200) && !j.kbd);
    CHECK(!j.round("key 0x41", "buttons", 400) && !j.kbd);           // both kinds in one round change nothing
    CHECK(j.round("key 0x41", nullptr, 400) && j.kbd);
    CHECK(!j.round(nullptr, "buttons", 450) && j.kbd);               // at most one switch per 150 ms: it waits ...
    CHECK(!j.round(nullptr, nullptr, 500) && j.kbd);
    CHECK(j.round(nullptr, nullptr, 550) && !j.kbd);                 // ... then wins
    CHECK(!j.round(nullptr, "buttons", 600) && !j.kbd);              // the same device again: no switch
    CHECK(!j.round("key 0x41", nullptr, 650) && !j.round(nullptr, "buttons", 660) && !j.round(nullptr, nullptr, 900) && !j.kbd);   // a later input replaces one waiting

    HudMouseMoves m;
    CHECK(!m.round(true, 100, 100));   // the first round only reads the place
    CHECK(!m.round(true, 900, 500));   // a single jump (the window placing the pointer) isn't the mouse
    CHECK(!m.round(true, 900, 500));
    CHECK(!m.round(true, 901, 500) && !m.round(true, 902, 500));   // two rounds moving 2 px: not enough
    CHECK(!m.round(true, 902, 500));
    CHECK(!m.round(true, 903, 500) && m.round(true, 905, 500));    // two rounds moving 3 px: the mouse
    m.forget();
    CHECK(!m.round(true, 0, 0) && !m.round(true, 50, 0));          // after forget(), a fresh start

    HudKeyWatch w;
    bool down[256] = {};
    char buf[48];
    const uint32_t hotkeys[] = {VK_F9};
    down['A'] = true;
    const char* got = w.round(down, hotkeys, 1, buf, sizeof buf);
    CHECK(got && std::strcmp(got, "key 0x41") == 0);
    CHECK(!w.round(down, hotkeys, 1, buf, sizeof buf));   // held, not going down
    down['A']    = false;
    down[VK_LWIN] = true;
    w.round(down, hotkeys, 1, buf, sizeof buf);
    down['R'] = true;
    CHECK(!w.round(down, hotkeys, 1, buf, sizeof buf));   // a key with a Windows key held doesn't count
    down[VK_LWIN] = false;
    CHECK(!w.round(down, hotkeys, 1, buf, sizeof buf));   // ... nor once the Windows key is let go, still held
    down['R'] = false;
    for (int vk : {VK_SHIFT, VK_LCONTROL, VK_RMENU, VK_F9}) {
        down[vk] = true;
        check(!w.round(down, hotkeys, 1, buf, sizeof buf), "T8 key 0x%02X (a modifier or a hotkey's key) doesn't count", vk);
        down[vk] = false;
        w.round(down, hotkeys, 1, buf, sizeof buf);
    }
    down[VK_LBUTTON] = true;
    got              = w.round(down, hotkeys, 1, buf, sizeof buf);
    CHECK(got && std::strcmp(got, "mouse button 1") == 0);
    down[VK_LBUTTON] = false;
    // the sweep's first round after coming to the front only reads the state: a key held then doesn't count later
    HudKeyWatch fresh;
    down['B'] = true;
    fresh.round(down, hotkeys, 1, buf, sizeof buf);   // its result isn't judged
    CHECK(!fresh.round(down, hotkeys, 1, buf, sizeof buf));

    HudPad was, now;
    CHECK(!hud_pad_input(now, was));
    now.buttons = 0x1000;
    CHECK(hud_pad_input(now, was) && std::strcmp(hud_pad_input(now, was), "buttons") == 0);
    now     = was;
    now.lt  = 30;
    CHECK(!hud_pad_input(now, was));   // not past 30
    now.lt = 31;
    CHECK(std::strcmp(hud_pad_input(now, was), "trigger") == 0);
    was.lt = 31;
    now.lt = 38;
    CHECK(!hud_pad_input(now, was));   // 7 past it
    now.lt = 39;
    CHECK(hud_pad_input(now, was));
    was    = HudPad{};
    now    = HudPad{};
    now.lx = 7849;
    CHECK(!hud_pad_input(now, was));
    now.lx = 7850;
    CHECK(std::strcmp(hud_pad_input(now, was), "left stick") == 0);
    was.rx = 9000;
    now    = was;
    now.rx = 9000 + 1023;
    CHECK(!hud_pad_input(now, was));
    now.rx = 9000 + 1024;
    CHECK(std::strcmp(hud_pad_input(now, was), "right stick") == 0);
}

// ---- T9: pulses -----------------------------------------------------------------------------------------

static void t9_pulses() {
    std::printf("\n== T9 pulses\n");
    {
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_COUNTDOWN, 60000);
        r.build();
        r.step();
        FH2HudState s{};
        s.size            = sizeof s;
        s.countdown_label = "Test run";
        bool all_ok       = true;
        for (uint32_t n = 3; n >= 1; --n) {
            s.countdown = n;
            r.e->set_state(a, &s);
            if (n == 3) r.e->show(a, FH2_HUD_PART_COUNTDOWN);
            double pop_at = -1, off_at = -1;
            int in_step = -1;
            for (int i = 0; i < 50; ++i) {   // a second
                r.step();
                const std::vector<Told> v = r.last();
                for (std::size_t k = 0; k < v.size(); ++k) {
                    if (v[k].name == "CountText" && v[k].text == std::to_wstring(n)) {
                        if (n == 3) {
                            // the first number came with the show: CountVis and Count after it, and no pop yet
                            const int vis = index_of(v, "CountVis", 1), fade = index_of(v, "Count", 1), pop = index_of(v, "CountPop", 1);
                            if (vis > static_cast<int>(k) && fade > vis && pop < 0) in_step = i;
                        } else if (k + 1 < v.size() && v[k + 1].name == "CountPop" && v[k + 1].bits == 1) {
                            pop_at = v[k].t;
                        }
                    }
                    // its pop two frames after the ring came in, once the page has laid it out
                    if (n == 3 && in_step >= 0 && i == in_step + 2 && v[k].name == "CountPop" && v[k].bits == 1) pop_at = v[k].t;
                    if (v[k].name == "CountPop" && v[k].bits == 0 && pop_at >= 0) off_at = v[k].t;
                }
            }
            check(pop_at >= 0 && off_at - pop_at >= 400 && off_at - pop_at < 420,
                  n == 3 ? "T9 countdown %u, with the show: CountText, CountVis and Count, then CountPop true two frames later, false %.0f ms after that"
                         : "T9 countdown %u: CountText then CountPop true in one frame, false %.0f ms later",
                  n, off_at - pop_at);
            all_ok = all_ok && pop_at >= 0;
        }
        // numbers 200 ms apart
        s.countdown = 9;
        r.e->set_state(a, &s);
        r.steps(10);
        s.countdown = 8;
        r.e->set_state(a, &s);
        r.step();
        const std::vector<Told> v1 = r.last();
        r.step();
        const std::vector<Told> v2 = r.last();
        check(told_in(v1, "CountPop", 0) && !told_name(v1, "CountText") && index_of(v2, "CountPop", 1) == 1 && v2[0].name == "CountText" && v2[0].text == L"8",
              "T9 numbers 200 ms apart: CountPop false told, then the number and CountPop true the next frame");
        // countdown 0 clears the number; the part stays
        s.countdown = 0;
        r.e->set_state(a, &s);
        r.steps(25);
        CHECK(r.text("CountText").empty() && r.e->shown_parts() == FH2_HUD_PART_COUNTDOWN);
    }
    {
        // the shutter: true, false 400 ms later, the root kept visible meanwhile
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
        r.build();
        r.step();
        CHECK(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 0);   // never shown
        FH2HudState s = bar_frame();
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR);
        r.steps(10);
        CHECK(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 1);
        r.step();
        const double on = r.t - 20;
        CHECK(told_in(r.last(), "Shutter", 1));
        r.e->show(a, FH2_HUD_PART_INSTANT);   // the bar goes at once; the blink keeps the root
        r.step();
        CHECK(told_in(r.last(), "BarVis", 0) && !told_in(r.last(), "Shown", 0));
        double off = -1, shown_off = -1;
        for (int i = 0; i < 30; ++i) {
            r.step();
            for (const Told& t : r.last()) {
                if (t.name == "Shutter" && t.bits == 0 && off < 0) off = t.t;
                if (t.name == "Shown" && t.bits == 0 && shown_off < 0) shown_off = t.t;
            }
        }
        check(off - on >= 400 && off - on < 420 && shown_off >= off, "T9 the shutter: false %.0f ms later, and the root kept visible until then", off - on);
        CHECK(r.e->pulse(a, FH2_HUD_PULSE_SHUTTER) == 0 && r.lines_with("pulse_hud: the layer isn't shown, so there's nothing to blink") == 1);
        CHECK(r.e->pulse(a, 2) == 0 && r.lines_with("[TrialEditor] pulse_hud: 2 isn't a pulse this preview has") == 1);
        {
            // a changing unknown value every frame doesn't flood the log: still the first one's line only
            bool all_zero = true;
            for (int v = 100; v < 1100; ++v) all_zero = all_zero && r.e->pulse(a, v) == 0;
            check(all_zero && r.lines_with("isn't a pulse this preview has") == 1, "T9 1000 more unknown pulses: refused, and still one line (%d)",
                  r.lines_with("isn't a pulse this preview has"));
        }
        // after show(INSTANT) and CLEAN: show with changed content and a pulse together
        r.steps(5);
        CHECK(r.e->clean());
        FH2HudState b = full_frame();
        r.e->set_state(a, &b);
        r.e->show(a, FH2_HUD_PART_BAR);
        const int pulsed = r.e->pulse(a, FH2_HUD_PULSE_SHUTTER);
        check(pulsed == 1, "T9 pulse_hud right after show_hud_layer returns 1");
        const std::size_t start = r.g.told.size();
        r.step();
        const std::vector<Told> v = r.last();
        const int shown = index_of(v, "Shown", 1), shutter = index_of(v, "Shutter", 1);
        int first_content = -1;
        for (int i = 0; i < static_cast<int>(v.size()); ++i)
            if (first_content < 0 && budgeted(r.prop(v[i].name))) first_content = i;
        check(shown >= 0 && shutter == shown + 1 && first_content > shutter && !told_in(v, "BarVis", 1),
              "T9 Shown then Shutter told true in that frame, before the bar's content, and BarVis not yet (%d, %d, %d)", shown, shutter, first_content);
        r.steps(10);
        int last_content = -1, vis = -1;
        for (std::size_t i = start; i < r.g.told.size(); ++i) {
            if (budgeted(r.prop(r.g.told[i].name))) last_content = static_cast<int>(i);
            if (r.g.told[i].name == "BarVis" && r.g.told[i].bits == 1 && vis < 0) vis = static_cast<int>(i);
        }
        check(vis > last_content && last_content >= 0, "T9 ... and BarVis true only once its content is told");
    }
    {
        // CountPop with its CountText while the countdown is in, with 100 values carried over
        Rig r;
        const int a = r.add("TrialEditor", FH2_HUD_PART_BAR | FH2_HUD_PART_COUNTDOWN, 60000);
        r.build();
        r.step();
        FH2HudState s = small_frame();
        s.countdown   = 3;
        r.e->set_state(a, &s);
        r.e->show(a, FH2_HUD_PART_BAR | FH2_HUD_PART_COUNTDOWN);
        r.steps(40);
        FH2HudState b = full_frame();
        b.countdown   = 2;
        r.e->set_state(a, &b);
        r.step();
        const uint64_t carried = r.carried[r.f];
        bool together = false;
        for (int i = 0; i < 20 && !together; ++i) {
            if (i) r.step();
            const std::vector<Told> v = r.last();
            for (std::size_t k = 0; k < v.size(); ++k)
                if (v[k].name == "CountText" && v[k].text == L"2") together = k + 1 < v.size() && v[k + 1].name == "CountPop" && v[k + 1].bits == 1;
        }
        check(carried >= 100 && together, "T9 CountPop true told right after its CountText with the countdown in and %llu values carried over",
              static_cast<unsigned long long>(carried));
    }
}

// ---- T10: sounds ----------------------------------------------------------------------------------------

static void t10_sounds() {
    std::printf("\n== T10 sounds\n");
    Rig r;
    const int a = r.add("TrialEditor", FH2_HUD_PART_BAR, 60000);
    r.build();
    r.step(false);   // off screen
    for (uint32_t snd : {FH2_SOUND_NOTIFY, FH2_SOUND_TICK, FH2_SOUND_TICK_LOW, FH2_SOUND_TICK_END})
        check(r.e->play_sound(a, snd) == 0, "T10 %s refused off screen", HudEngine::sound_name(snd));
    CHECK(r.lines_with("[TrialEditor] play_ui_sound: NOTIFY only sounds in the world, and the free-roam HUD page isn't on screen (a menu or the map is open); "
                       "not played") == 1);
    for (uint32_t snd : {FH2_SOUND_ERROR, FH2_SOUND_INFO, FH2_SOUND_SHUTTER})
        check(r.e->play_sound(a, snd) == 1, "T10 %s accepted off screen", HudEngine::sound_name(snd));
    std::vector<int> played;
    for (int i = 0; i < 4; ++i) played.push_back(r.step(false).sound);
    check(played == std::vector<int>({FH2_SOUND_ERROR, FH2_SOUND_INFO, FH2_SOUND_SHUTTER, -1}), "T10 one played a frame, oldest first");
    r.step();
    CHECK(r.e->play_sound(a, FH2_SOUND_TICK) == 1);
    r.t += 50;
    CHECK(r.e->play_sound(a, FH2_SOUND_TICK) == 0);   // a repeat within 100 ms
    r.t += 50;
    CHECK(r.e->play_sound(a, FH2_SOUND_TICK) == 1);   // 100 ms on
    r.steps(3);
    CHECK(r.e->play_sound(a, 4) == 0 && r.e->play_sound(a, 12) == 0 && r.lines_with("[TrialEditor] play_ui_sound: 4 isn't a sound this preview plays") == 1);
    {
        // a changing unknown sound doesn't flood the log: still the first one's line only
        bool all_zero = true;
        for (uint32_t v = 100; v < 1100; ++v) all_zero = all_zero && r.e->play_sound(a, v) == 0;
        check(all_zero && r.lines_with("isn't a sound this preview plays") == 1, "T10 1000 more unknown sounds: refused, and still one line (%d)",
              r.lines_with("isn't a sound this preview plays"));
    }
    // one older than 500 ms dropped, its line once
    r.e->play_sound(a, FH2_SOUND_INFO);
    r.t += 600;
    const int late = r.step().sound;
    r.e->play_sound(a, FH2_SOUND_INFO);
    r.t += 600;
    r.step();
    check(late == -1 && r.lines_with("[TrialEditor] sound: INFO dropped: the game's frame didn't come within 0.5 s") == 1,
          "T10 a sound older than 500 ms dropped, with its line once");
    // a world sound queued on screen and the page gone before the frame
    r.e->play_sound(a, FH2_SOUND_NOTIFY);
    const int gone = r.step(false).sound;
    check(gone == -1 && r.lines_with("[TrialEditor] sound: NOTIFY dropped: the free-roam HUD page went off screen before it could play") == 1,
          "T10 NOTIFY queued on screen, the page gone before its frame: dropped, logged");
    // sounds off: refused
    r.e->set_sounds_available(false);
    CHECK(r.e->play_sound(a, FH2_SOUND_SHUTTER) == 0 &&
          r.lines_with("[TrialEditor] play_ui_sound: the game's sound code isn't as expected, or sounds are off after a crash; not played") == 1);
}

int wmain(int argc, wchar_t** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path(L".");
    std::error_code ec;
    fs::create_directories(out, ec);
    t1_template(out);
    t0_no_layer();
    t2_staging();
    t3_strings();
    t4_lease();
    t5_parts();
    t6_owner();
    t7_text();
    t8_device();
    t9_pulses();
    t10_sounds();
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    std::fflush(stdout);
    return fails;
}

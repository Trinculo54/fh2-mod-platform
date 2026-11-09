// In-game settings (API 4, sliders API 5): On/Off and slider rows for mods on a Mods page of
// the game's own. Also the MODS tab (platform 2.5.0) and the mods' own pages (API 8).
//
// What the player sees:
//  - Pause > Settings gets a sixth tile, MODS. It opens a page laid out like the HUD options
//    page, with every mod's rows in its list, the row's description beside it, and a panel
//    under the list while a change is waiting for a restart.
//  - Coming back from the Mods page with a change that needs a restart (not asked about
//    yet) brings up the game's own message box: Restart now / Restart later / Go back.
//    Restart now closes the game as Azimuth's own Quit does and starts it again.
//
// How, in short (game addresses are for the 24 Nov 2015 build; each is checked first):
//  - The menus are XAML pages in media\UI.zip, run by AVUI, a C++ take on WPF. The platform
//    writes three files to mediaoverride: the Settings page with the tile added, and the
//    Mods page, A_ModOptions.xaml, which is the HUD page's A_HUDOptions.xaml with our rows.
//  - Bindings find properties by name through AVUI::Type::FindProperty (0x14157FF60). It's
//    hooked: for "FH2Mod_..." names the game doesn't know, it returns property descriptors of
//    our own, laid out like the HUD page's On/Off properties (EnableDrivatarLiveries,
//    0x140D58660), with our getter and setter.
//  - The Settings page object is made once while the game boots. Its tiles are wired in
//    0x140DC8A60 with 0x140D9FC10(page, "PART_Button_X", &pmf); that's hooked to wire ours.
//    Its controller grid is a fixed 5 x 2 (navigation manager at page+0x410, columns at
//    +0x9C), rebuilt from that count each time: it's set to 6 just before.
//  - Our tile runs the HUD tile's handler (0x140DC6C40), which fires the state-machine
//    event that opens the HUD page. Opening it builds the page's scene record twice
//    (0x140B3A3E0: { id 0x49, "HudOptions", L"Anthem\A_HudOptions" at +0x28, factory }),
//    and the second copy, the one the new page loads its XAML from, goes into the page.
//    Both builds just after our tile was pressed are pointed at A_ModOptions instead.
//  - Sliders are the game's own Slider control (the audio volumes, the controller dead zones),
//    styled like A_Style_Slider_Options but with each row's range, step and number format,
//    bound to float properties of ours laid out like a float property of the game's
//    (0x140ED6E90), boxed with 0x1403F5990.
//  - Extra Options adds its own rows to any page declared x:Class="ForzaUI.A_HudOptions";
//    the Mods page writes it with a space before the '=', which keeps them off it.
//  - The message box is the one the options pages use for "Unsaved changes": see the popup
//    section below.
#include "db_guards.hpp"
#include "hud_layer.hpp"
#include "l10n.hpp"
#include "lang.hpp"
#include "page_guard.hpp"
#include "platform.hpp"
#include "popups.hpp"
#include "seen.hpp"
#include "strings.hpp"
#include "text.hpp"
#include "ui_rules.hpp"
#include "ui_sounds.hpp"
#include "ui_player.hpp"
#include "ui_templates.hpp"
#include "updates.hpp"
#include "xml_text.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fh2platform {
namespace {

const char* const kWho = "MODS";   // the log's tag (up to 2.8.0, "Mods page")
FH2Mod g_owner_mod;                 // owns the platform's files and hooks for this
FH2Mod g_hub_owner;                 // the MODS tab's own rows (each mod's Load switches)
const FH2ModApi* g_api = nullptr;
uintptr_t g_base       = 0;

void slog(const std::string& s) { log_line(kWho, s); }

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

bool code_matches(uintptr_t rva, const uint8_t* expect, std::size_t n) {
    return readable(reinterpret_cast<void*>(g_base + rva), n) &&
           std::memcmp(reinterpret_cast<void*>(g_base + rva), expect, n) == 0;
}

// ---- the rows -------------------------------------------------------------------------

struct Row {
    FH2Mod* mod;
    std::string key, label, description;
    std::string prop;                // the XAML binds to this: "FH2Mod_3"
    std::atomic<int> value{0};
    int boot = 0;                    // the value the game started with
    bool restart = false;
    FH2SettingChangedFn on_change = nullptr;
    void* user = nullptr;
    uint8_t* desc = nullptr;         // our AVUI property descriptor, made on first use
    // sliders (API 5)
    bool slider = false;
    std::atomic<float> fvalue{0};
    float fboot = 0, fmin = 0, fmax = 1, fstep = 0.01f, scale = 1;
    int decimals = 0;
    FH2SliderChangedFn on_slide = nullptr;
    // choices (API 9): a slider over 0 .. n-1, shown by name; `value` is the index
    std::vector<std::string> choices;
    bool hotkey = false;             // one of a hotkey's two rows (hotkeys.cpp): under "Keys and buttons" on its mod's page
};
std::mutex g_mu;                     // guards the list (rows are never removed)
std::deque<Row> g_rows;
// Link rows (API 6): only on their mod's page under Installed mods, where A opens the URL.
struct LinkRow {
    FH2Mod* mod;
    std::string key, label, description, url;
};
std::deque<LinkRow> g_links;         // guarded by g_mu
std::atomic<bool> g_written{false};  // the pages have been written

// Pages of the mods' own (API 8): a row on their mod's page under Installed mods opens them
// (as a view of that page, like the mod's own), and a page can take over a game tile.
struct PageRowData {
    std::string key, label, value, description, footer;
    int style       = FH2_ROW_NORMAL;
    bool value_alert = false;
};
struct ModPage {
    FH2Mod* mod;
    std::string key, title, label, description;
    int tile = -1;                   // index in kTiles, or -1
    FH2PageOpenFn on_open     = nullptr;
    FH2PageSelectFn on_select = nullptr;
    void* user                = nullptr;
    std::vector<PageRowData> rows;
    // a page of the mod's own design (set_page_xaml)
    FH2PageButtonFn on_button = nullptr;
    FH2PageSlotFn on_slot     = nullptr;
    bool custom               = false;
    bool xaml_refused         = false;   // XAML was refused by the check (page_guard.cpp) and none ever passed: the page says so instead
    std::string xaml_resources, xaml_content, xaml_help;
    std::string xaml_written;        // the content as last written (what's on screen)
    std::array<std::atomic<int>, FH2_PAGE_SLOTS> slots{};
    std::string tile_text;           // on its tile (set_page_tile_text)
    uint32_t tile_back = 0, tile_fore = 0;   // its tile's colours, 0xAARRGGBB; 0 the game's (set_page_tile_colors)
    // API 10: its help bar (set_page_help: the prompts accepted; none = the platform's own) and its hint line
    // (set_page_hints' texts, and show_page_hint's choice: -1 for none, and its FH2_HINT_ kind)
    std::vector<HelpPrompt> help;
    std::vector<std::string> hints;
    int hint            = -1;
    uint32_t hint_kind  = FH2_HINT_INFO;
};
std::deque<ModPage> g_pages;         // guarded by g_mu (pages are never removed)
constexpr std::size_t kMaxPages    = 64;
constexpr std::size_t kMaxPageRows = 1000;

// The game's tiles a page can take over: each one's handler calls 0x140CB47B0(page, &out,
// 0xFE), a UI command that brought up the Xbox's own achievements app. Those calls (hook_call)
// open the page instead; the handler's own checks still run first.
struct GameTile {
    const char* name;
    uintptr_t calls[2];              // the CALL instructions (rva)
    const char* buttons[2];          // the tiles in A_PauseMenu.xaml
};
constexpr GameTile kTiles[] = {
    // PART_Button_Achievements (0x140DBD610, on the PROGRESS tab) and the PORSCHE tab's
    // PART_Button_Panama_Achievements (0x140DBF150, which first checks the tile has the focus)
    {"Achievements", {0x0DBD635, 0x0DBF185}, {"PART_Button_Achievements", "PART_Button_Panama_Achievements"}},
};
// Where the player finds each tile, for the row of a page that's only on its tile: whole sentences, so each
// language words them its own way, with the tabs' and the tile's names as the game shows them there.
const char* const kTileOnlyThere[] = {"Open it from Pause > PROGRESS > ACHIEVEMENTS, or the PORSCHE tab's ACHIEVEMENTS tile."};
const char* const kTileToo[]       = {"Pause > PROGRESS > ACHIEVEMENTS opens it too."};
constexpr std::size_t kTileCount = sizeof kTiles / sizeof kTiles[0];
// the page in g_pages that has each tile, or -1 (guarded by g_mu)
std::array<int, kTileCount> g_tile_page = [] {
    std::array<int, kTileCount> a{};
    a.fill(-1);
    return a;
}();

bool valid_key(const char* k) {
    if (!k || !*k) return false;
    for (; *k; ++k)
        if (!(std::isalnum(static_cast<unsigned char>(*k)) || *k == '_')) return false;
    return true;
}

bool has_link(FH2Mod* m, const char* key) {
    for (const LinkRow& l : g_links)
        if (l.mod == m && l.key == key) return true;
    return false;
}

Row* find_row(FH2Mod* m, const char* key) {
    for (Row& r : g_rows)
        if (r.mod == m && r.key == key) return &r;
    return nullptr;
}

Row* row_by_prop(const wchar_t* name) {
    // "FH2Mod_<n>"
    if (std::wcsncmp(name, L"FH2Mod_", 7) != 0) return nullptr;
    const wchar_t* d = name + 7;
    if (!*d) return nullptr;
    std::size_t n = 0;
    for (; *d; ++d) {
        if (*d < L'0' || *d > L'9') return nullptr;
        n = n * 10 + (*d - L'0');
    }
    return n < g_rows.size() ? &g_rows[n] : nullptr;
}

Row* row_by_desc(void* desc) {
    for (Row& r : g_rows)
        if (r.desc == desc) return &r;
    return nullptr;
}

// A restart row that's not what the game started with: it waits for the next start.
bool row_changed(const Row& r) {
    return r.restart && (r.slider ? std::fabs(r.fvalue.load() - r.fboot) > r.fstep * 0.5f : r.value.load() != r.boot);
}

bool restart_pending() {
    for (Row& r : g_rows)
        if (row_changed(r)) return true;
    return false;
}

// The restart rows' values, to tell whether the restart popup has asked about them already: a live
// row (a slider moved after Restart later) doesn't bring it back.
std::string current_values() {
    std::string v;
    for (Row& r : g_rows)
        if (r.restart) v += (r.slider ? std::to_string(r.fvalue.load()) : std::to_string(r.value.load())) + ",";
    return v;
}

// ---- property descriptors -----------------------------------------------------------------
constexpr uintptr_t RVA_FIND_PROPERTY   = 0x157FF60;   // AVUI::Type::FindProperty(type, name)
constexpr uintptr_t RVA_OBJECT_CTOR     = 0x143EB80;   // AVUI::Object::Object(this)
constexpr uintptr_t RVA_BOX_INT         = 0x04702D0;   // ObjectPtr* box(ObjectPtr* out, const int*)
constexpr uintptr_t RVA_BOX_BOOL        = 0x0336BA0;   // ObjectPtr* box(ObjectPtr* out, const bool*)
constexpr uintptr_t RVA_BOOL_TYPE       = 0x15C9550;   // the bool type, (0)
constexpr uintptr_t RVA_NOTIFY_PROP     = 0x0CF9C60;   // raise PropertyChanged(obj, desc)
constexpr uintptr_t RVA_MODEL_PROPERTY  = 0x0D58660;   // the HUD page's EnableDrivatarLiveries descriptor
constexpr uintptr_t RVA_FLOAT_PROPERTY  = 0x0ED6E90;   // a float property's descriptor, for sliders
constexpr uintptr_t RVA_BOX_FLOAT       = 0x03F5990;   // ObjectPtr* box(ObjectPtr* out, const float*)
using BoxIntFn    = void** (*)(void** out, const int* value);
using BoxBoolFn   = void** (*)(void** out, const bool* value);
using BoxFloatFn  = void** (*)(void** out, const float* value);
using TypeFn      = void* (*)(int);
using NotifyFn    = void (*)(void* obj, void* desc);
using AccessorFn  = uint8_t* (*)();
using ObjectCtorFn = void (*)(void* self);

std::mutex g_desc_mu;
bool g_sliders_ok = false;           // the game's float property and float boxing look as expected
uint8_t* g_notice_desc = nullptr;    // FH2Mod_RestartNotice

// A descriptor laid out like the model's: +0x28 getter(out, desc, obj), +0x30 setter(desc,
// obj, value), +0x40 name, +0x48 value type (the model's, an int, unless given), +0x50 flags.
// Sliders copy a float property instead.
uint8_t* new_descriptor(const char* name, void* getter, void* setter, void* type, uintptr_t model_rva = RVA_MODEL_PROPERTY) {
    uint8_t* model = reinterpret_cast<AccessorFn>(g_base + model_rva)();
    if (!model || !readable(model, 0x58)) return nullptr;
    auto* d = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!d) return nullptr;
    reinterpret_cast<ObjectCtorFn>(g_base + RVA_OBJECT_CTOR)(d);
    std::memcpy(d, model, 8);                                            // vtable
    *reinterpret_cast<int32_t*>(d + 0x10) += 1 << 20;                    // never freed
    *reinterpret_cast<void**>(d + 0x28) = getter;
    *reinterpret_cast<void**>(d + 0x30) = setter;
    std::memcpy(d + 0x38, model + 0x38, 8);
    *reinterpret_cast<const char**>(d + 0x40) = name;
    if (type)
        *reinterpret_cast<void**>(d + 0x48) = type;
    else
        std::memcpy(d + 0x48, model + 0x48, 8);
    d[0x50] = model[0x50];
    return d;
}

void notify_changed(void* obj, void* desc) {
    __try {
        reinterpret_cast<NotifyFn>(g_base + RVA_NOTIFY_PROP)(obj, desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// A crash caught in a mod's callback: the line, and its waiting pop-ups and tip dropped (API 10).
void crashed_in(FH2Mod* m, const std::string& what) {
    log_line(m->name, "crashed in its " + what);
    ui_mod_faulted(m);
}

bool call_on_change(FH2SettingChangedFn fn, void* user, const char* key, int v) {
    __try {
        fn(user, key, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void** __cdecl row_get(void** out, void* desc, void*) {
    Row* r = row_by_desc(desc);
    *out   = nullptr;
    if (r && r->slider) {
        float f = r->fvalue.load();
        reinterpret_cast<BoxFloatFn>(g_base + RVA_BOX_FLOAT)(out, &f);
        return out;
    }
    int v = r ? r->value.load() : 0;
    reinterpret_cast<BoxIntFn>(g_base + RVA_BOX_INT)(out, &v);
    return out;
}

bool call_on_slide(FH2SliderChangedFn fn, void* user, const char* key, float v) {
    __try {
        fn(user, key, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// a value inside the slider's range, on one of its steps
float snap(const Row& r, float v) {
    if (v < r.fmin) v = r.fmin;
    if (v > r.fmax) v = r.fmax;
    if (r.fstep > 0) v = r.fmin + std::round((v - r.fmin) / r.fstep) * r.fstep;
    if (v > r.fmax) v = r.fmax;
    return v;
}

std::string show(const Row& r, float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", r.decimals, static_cast<double>(v * r.scale));
    return b;
}

void notify_panel(void* obj);   // below, with the panel's properties
// The page of the game's options class (A_HUDOptions.xaml's, which every page the platform writes is) loaded last: the
// page a setting's on_change, or a slot's on_slot, comes from (d_hud_page_loaded; null without the MODS tab's hooks).
std::atomic<void*> g_last_hud_page{nullptr};

// A row the player changed, in its mod's log lines. The Load switches say it themselves (set_mod_enabled, set_mod_online).
void log_set(const Row* r, const std::string& value) {
    if (r->mod != &g_hub_owner) log_line(r->mod->name, r->key + " set to " + value + " on its MODS page");
}

void slider_set(Row* r, void* obj, uint8_t* value) {
    float f = 0;
    std::memcpy(&f, value + 0x28, sizeof f);
    if (!std::isfinite(f)) return;
    f               = snap(*r, f);
    const float old = r->fvalue.exchange(f);
    if (std::fabs(old - f) < r->fstep * 0.01f) return;
    if (!r->choices.empty()) {   // a choice row (API 9): its index, to on_change
        const int v = static_cast<int>(std::lround(f));
        r->value    = v;
        log_set(r, r->choices[static_cast<std::size_t>(v)]);
        if (r->on_change) {
            UiCallback cb{r->mod, UiCb::setting, g_last_hud_page.load()};
            ui_note_callback(r->mod);
            if (!call_on_change(r->on_change, r->user, r->key.c_str(), v)) crashed_in(r->mod, "on_change for " + r->key);
        }
    } else {
        log_set(r, show(*r, f));
        if (r->on_slide) {
            UiCallback cb{r->mod, UiCb::setting, g_last_hud_page.load()};
            ui_note_callback(r->mod);
            if (!call_on_slide(r->on_slide, r->user, r->key.c_str(), f)) crashed_in(r->mod, "on_slide for " + r->key);
        }
    }
    if (r->restart) notify_panel(obj);   // the panel names the rows waiting for a restart
}

void __cdecl row_set(void* desc, void* obj, uint8_t* value) {
    Row* r = row_by_desc(desc);
    if (!r || !value || !readable(value + 0x28, 4)) return;
    if (r->slider) return slider_set(r, obj, value);
    const int v   = *reinterpret_cast<int*>(value + 0x28);
    const int old = r->value.exchange(v);
    if (old == v) return;
    log_set(r, std::to_string(v));
    if (r->on_change) {
        UiCallback cb{r->mod, UiCb::setting, g_last_hud_page.load()};
        ui_note_callback(r->mod);
        if (!call_on_change(r->on_change, r->user, r->key.c_str(), v)) crashed_in(r->mod, "on_change for " + r->key);
    }
    if (r->restart) notify_panel(obj);   // the panel names the rows waiting for a restart
}

void** __cdecl notice_get(void** out, void*, void*) {
    bool v = restart_pending();
    *out   = nullptr;
    reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
    return out;
}
void __cdecl notice_set(void*, void*, uint8_t*) {}

uint8_t* descriptor_for(Row* r) {
    std::scoped_lock lk{g_desc_mu};
    if (r->slider && !g_sliders_ok) return nullptr;
    if (!r->desc)
        r->desc = new_descriptor(r->prop.c_str(), reinterpret_cast<void*>(&row_get), reinterpret_cast<void*>(&row_set), nullptr,
                                 r->slider ? RVA_FLOAT_PROPERTY : RVA_MODEL_PROPERTY);
    return r->desc;
}

// FH2Mod_Selected: the list's SelectedIndex (it follows the focus), bound two-way on Installed
// mods so a press of A knows which mod's row it was on.
std::atomic<int> g_selected{0};
std::atomic<void*> g_view_obj{nullptr};    // what Installed mods' bindings read from (for PropertyChanged)
uint8_t* g_selected_desc = nullptr;
void** __cdecl selected_get(void** out, void*, void*) {
    int v = g_selected.load();
    *out  = nullptr;
    reinterpret_cast<BoxIntFn>(g_base + RVA_BOX_INT)(out, &v);
    return out;
}
void __cdecl selected_set(void*, void* obj, uint8_t* value) {
    if (obj) g_view_obj = obj;
    if (value && readable(value + 0x28, 4)) {
        g_selected = *reinterpret_cast<int*>(value + 0x28);
    }
}
uint8_t* selected_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_selected_desc)
        g_selected_desc = new_descriptor("FH2Mod_Selected", reinterpret_cast<void*>(&selected_get), reinterpret_cast<void*>(&selected_set), nullptr);
    return g_selected_desc;
}

// FH2Mod_ViewSelected: the same for the list of the mod's page that's showing (each mod's
// list binds it), so A on a link row knows which row it was on.
std::atomic<int> g_view_selected{0};
uint8_t* g_view_selected_desc = nullptr;
void** __cdecl view_selected_get(void** out, void*, void*) {
    int v = g_view_selected.load();
    *out  = nullptr;
    reinterpret_cast<BoxIntFn>(g_base + RVA_BOX_INT)(out, &v);
    return out;
}
void __cdecl view_selected_set(void*, void*, uint8_t* value) {
    if (value && readable(value + 0x28, 4)) g_view_selected = *reinterpret_cast<int*>(value + 0x28);
}
uint8_t* view_selected_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_view_selected_desc)
        g_view_selected_desc = new_descriptor("FH2Mod_ViewSelected", reinterpret_cast<void*>(&view_selected_get),
                                              reinterpret_cast<void*>(&view_selected_set), nullptr);
    return g_view_selected_desc;
}

// FH2Mod_View_<k>: whether view k of Installed mods is showing (0: the list, 1..64: a mod's
// settings, then the platform's own page, then the mods' pages). Showing another view hides the
// rest; see switch_view. FH2Mod_ViewIn_<k> is the same, but only when the view came forwards (A, from the list or a
// mod's row), FH2Mod_ViewBack_<k> only when it came back (B); their DataTriggers play the
// switch's "in" and "back" animations. FH2Mod_Bar_<b>: which help bar shows (kBarList and so on).
constexpr int kMaxModViews  = 64;
constexpr int kMaxPageViews = 64;
constexpr int kMaxViews     = 1 + kMaxModViews + 1 + kMaxPageViews;   // the list, the mods, the platform's page, pages
enum ViewKind { kViewList = 0, kViewMod, kViewPage, kViewPageSelect, kViewKinds };
std::atomic<int> g_view{0};
std::atomic<bool> g_view_back{false};       // the view showing came back (B), not forwards
std::array<std::atomic<int>, kMaxViews> g_view_kind{};   // each view's ViewKind, as last written
uint8_t* g_view_desc[kMaxViews]      = {};
uint8_t* g_view_in_desc[kMaxViews]   = {};
uint8_t* g_view_back_desc[kMaxViews] = {};
uint8_t* g_bar_desc[kViewKinds]      = {};
// A page view with a help bar of its own (set_page_help, API 10): FH2Mod_HelpBar_<k> shows it, and the kind's bar
// (FH2Mod_Bar_<b>) hides while that view is showing.
std::array<std::atomic<bool>, kMaxViews> g_view_own_help{};
uint8_t* g_help_bar_desc[kMaxViews] = {};
int view_kind(int k) { return k >= 0 && k < kMaxViews ? g_view_kind[k].load() : kViewList; }
bool view_own_help(int k) { return k >= 0 && k < kMaxViews && g_view_own_help[k].load(); }
void** __cdecl view_get(void** out, void* desc, void* obj) {
    // Only Installed mods binds these, and they're read as it loads: so switch_view can notify
    // from the first press of A, before the selection has moved and FH2Mod_Selected been set.
    if (obj) g_view_obj = obj;
    const int now = g_view.load();
    const bool back = g_view_back.load();
    bool v = false;
    for (int k = 0; k < kMaxViews; ++k) {
        if (g_view_desc[k] == desc) v = now == k;
        else if (g_view_in_desc[k] == desc) v = now == k && !back;
        else if (g_view_back_desc[k] == desc) v = now == k && back;
        else continue;
        break;
    }
    for (int b = 0; b < kViewKinds; ++b)
        if (g_bar_desc[b] == desc) v = view_kind(now) == b && !view_own_help(now);
    for (int k = 0; k < kMaxViews; ++k)
        if (g_help_bar_desc[k] == desc) v = now == k;
    *out = nullptr;
    reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
    return out;
}
void __cdecl view_set(void*, void*, uint8_t*) {}

// "FH2Mod_View_<k>" and the like: the number after `prefix` (below `limit`), or -1
int name_number(const wchar_t* n, const wchar_t* prefix, int limit) {
    const std::size_t len = std::wcslen(prefix);
    if (std::wcsncmp(n, prefix, len) != 0 || !n[len]) return -1;
    int k = 0;
    for (const wchar_t* d = n + len; *d; ++d) {
        if (*d < L'0' || *d > L'9') return -1;
        k = k * 10 + (*d - L'0');
        if (k >= limit) return -1;
    }
    return k;
}

uint8_t* flag_descriptor(uint8_t*& slot, const std::string& name) {
    std::scoped_lock lk{g_desc_mu};
    if (!slot) {
        static std::deque<std::string> names;   // the descriptor keeps the pointer
        names.push_back(name);
        void* bool_type = reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0);
        slot = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&view_get), reinterpret_cast<void*>(&view_set), bool_type);
    }
    return slot;
}

// FH2Mod_UpdateBadge: "!" on the MODS tab (its Tag, as the other tabs' notification counts)
// while a mod has a newer version out, "" otherwise. The value is an AVUI String, made once
// with 0x141663220 and never freed; the property's type is the one the game gives the tabs'
// badge properties (0x1416A4B30).
constexpr uintptr_t RVA_BADGE_TYPE = 0x16A4B30;
alignas(16) uint8_t g_badge_on[0x80], g_badge_off[0x80];
bool g_badge_strings = false;
bool any_update_out();
int updates_out();
bool any_problem();
bool new_to_player();   // the NEW badge's cue (with the first-run cues, below)
uint8_t* tile_string(const std::string& text);
// The MODS tab shows one badge at a time: red (a mod isn't working), else yellow (an update is out), else NEW
// (the player hasn't opened YOUR MODS yet, or a mod is new). Each is repeated on the tile that resolves it:
// red and NEW on YOUR MODS, yellow on UPDATES, whatever the tab shows.
enum BadgeKind { kBadgeError, kBadgeUpdate, kBadgeNew, kBadgeTileUpdate, kBadgeTileNew, kBadgeKinds };
const char* const kBadgeNames[kBadgeKinds] = {"FH2Mod_ErrorBadge", "FH2Mod_UpdateBadge", "FH2Mod_NewBadge", "FH2Mod_TileUpdateBadge",
                                              "FH2Mod_TileNewBadge"};
uint8_t* g_badge_descs[kBadgeKinds] = {};
std::atomic<void*> g_badge_obj{nullptr};   // the pause menu page that read them
std::string badge_text(int kind) {
    const bool problem = any_problem(), update = any_update_out();
    switch (kind) {
        case kBadgeError: return problem ? "!" : "";
        case kBadgeUpdate: return update && !problem ? "!" : "";
        case kBadgeNew: return !problem && !update && new_to_player() ? tr("NEW") : "";
        case kBadgeTileUpdate: return update ? "!" : "";
        case kBadgeTileNew: return !problem && new_to_player() ? tr("NEW") : "";
        default: return "";
    }
}
void** __cdecl badge_get(void** out, void* desc, void* obj) {
    *out = nullptr;
    if (!g_badge_strings) {
        reinterpret_cast<void (*)(void*, const char*, int)>(g_base + 0x1663220)(g_badge_on, "!", -1);
        reinterpret_cast<void (*)(void*, const char*, int)>(g_base + 0x1663220)(g_badge_off, "", -1);
        *reinterpret_cast<int32_t*>(g_badge_on + 0x10) += 1 << 20;   // never freed
        *reinterpret_cast<int32_t*>(g_badge_off + 0x10) += 1 << 20;
        g_badge_strings = true;
    }
    if (obj) g_badge_obj = obj;
    int kind = kBadgeError;
    while (kind < kBadgeKinds && g_badge_descs[kind] != desc) ++kind;
    const std::string text = badge_text(kind);
    uint8_t* v = text.empty() ? g_badge_off : text == "!" ? g_badge_on : tile_string(text);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(v + 0x10));   // the caller releases it
    *out = v;
    return out;
}
void __cdecl badge_set(void*, void*, uint8_t*) {}
uint8_t* badge_descriptor(int kind) {
    std::scoped_lock lk{g_desc_mu};
    static const uint8_t expect[] = {0x48, 0x8D, 0x05, 0x59, 0xB0, 0x02, 0x02, 0xC3};
    uint8_t*& d = g_badge_descs[kind];
    if (!d && code_matches(RVA_BADGE_TYPE, expect, sizeof expect)) {
        void* type = reinterpret_cast<TypeFn>(g_base + RVA_BADGE_TYPE)(0);
        d = new_descriptor(kBadgeNames[kind], reinterpret_cast<void*>(&badge_get), reinterpret_cast<void*>(&badge_set), type);
    }
    return d;
}

// A tile a mod's page has, from its set_page_tile_text and set_page_tile_colors:
//  - FH2Mod_TileText_<t>: the text, an AVUI String of the tabs' badge type (like
//    FH2Mod_UpdateBadge). Each text's String is made once and never freed (a few per session).
//  - FH2Mod_TileBack_<t>, _TileTitle_<t>, _TileLine_<t>, _TileIcon_<t>: Colors (the type and
//    boxing of the game's TeamColor property, 0x140C09BD0: 0x1415C8D80 and 0x140BEA4E0, four
//    floats R, G, B, A from 0 to 1). Unset, they're the game's: the tile background, the tile
//    title, the club tile's count, and white (the icon untinted).
// The getter notes what it read from, the pause menu page, so the next time that page is wired
// it's told they may have changed.
enum TileProp { kTileText, kTileBack, kTileTitle, kTileLine, kTileIcon, kTileProps };
const wchar_t* const kTilePropNames[kTileProps] = {L"FH2Mod_TileText_", L"FH2Mod_TileBack_", L"FH2Mod_TileTitle_", L"FH2Mod_TileLine_",
                                                   L"FH2Mod_TileIcon_"};
constexpr uintptr_t RVA_COLOR_TYPE = 0x15C8D80;   // the Color type, (0)
constexpr uintptr_t RVA_BOX_COLOR  = 0xBEA4E0;    // ObjectPtr* box(ObjectPtr* out, const float rgba[4])
uint8_t* g_tile_desc[kTileProps][kTileCount] = {};
std::atomic<void*> g_tile_obj[kTileCount] = {};
std::mutex g_tile_text_mu;
std::deque<std::pair<std::string, uint8_t*>> g_tile_strings;   // guarded by g_tile_text_mu
// The text is UTF-8, but 0x141663220 widens each byte on its own (sign-extended: anything past ASCII comes out as
// boxes), into a buffer of length + 1 UTF-16 units at +0x28, the length at +0x30. So it's given as many ASCII
// placeholders as the text has UTF-16 units, and the buffer is then filled with the text itself.
uint8_t* tile_string(const std::string& text) {
    std::scoped_lock lk{g_tile_text_mu};
    for (auto& [t, str] : g_tile_strings)
        if (t == text) return str;
    const std::wstring w = widen(text);
    uint8_t* str = static_cast<uint8_t*>(_aligned_malloc(0x80, 16));
    std::memset(str, 0, 0x80);
    reinterpret_cast<void (*)(void*, const char*, int)>(g_base + 0x1663220)(str, std::string(w.size(), '?').c_str(), -1);
    auto* buf = *reinterpret_cast<wchar_t**>(str + 0x28);
    if (buf && *reinterpret_cast<int32_t*>(str + 0x30) == static_cast<int32_t>(w.size())) std::memcpy(buf, w.data(), w.size() * sizeof(wchar_t));
    *reinterpret_cast<int32_t*>(str + 0x10) += 1 << 20;   // never freed
    g_tile_strings.emplace_back(text, str);
    return str;
}
// The page on tile t's text and colours (as given: 0 for the game's).
struct TileLook {
    std::string text;
    uint32_t back = 0, fore = 0;
};
TileLook tile_look(std::size_t t) {
    std::scoped_lock lk{g_mu};
    const int j = t < kTileCount ? g_tile_page[t] : -1;
    if (j < 0) return {};
    return {g_pages[j].tile_text, g_pages[j].tile_back, g_pages[j].tile_fore};
}
uint32_t tile_color(int prop, const TileLook& l) {
    switch (prop) {
        case kTileBack: return l.back ? l.back : 0xFFF5F5F5;    // A_Color_Tile_Background
        case kTileTitle: return l.fore ? l.fore : 0xFF21293A;   // A_Color_Tile_Title
        case kTileLine: return l.fore ? l.fore : 0xFFFE5723;    // A_Color_Tile_Selected (the club count)
        default: return l.back ? l.back : 0xFFFFFFFF;           // the icon in the background's colour
    }
}
void** __cdecl tile_prop_get(void** out, void* desc, void* obj) {
    *out = nullptr;
    for (std::size_t t = 0; t < kTileCount; ++t)
        for (int prop = 0; prop < kTileProps; ++prop) {
            if (g_tile_desc[prop][t] != desc) continue;
            if (obj) g_tile_obj[t] = obj;
            const TileLook l = tile_look(t);
            if (prop == kTileText) {
                uint8_t* v = tile_string(l.text);
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(v + 0x10));   // the caller releases it
                *out = v;
            } else {
                const uint32_t c = tile_color(prop, l);
                alignas(16) float rgba[4] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f};
                reinterpret_cast<void** (*)(void**, const float*)>(g_base + RVA_BOX_COLOR)(out, rgba);
            }
            return out;
        }
    return out;
}
uint8_t* tile_descriptor(int prop, std::size_t t) {
    std::scoped_lock lk{g_desc_mu};
    static const uint8_t badge_type[] = {0x48, 0x8D, 0x05, 0x59, 0xB0, 0x02, 0x02, 0xC3};
    static const uint8_t color_type[] = {0x80, 0x3D, 0x19, 0xD3, 0x0E, 0x02, 0x00, 0x75, 0x36};
    static const uint8_t box_color[]  = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00};
    uint8_t*& d = g_tile_desc[prop][t];
    if (d) return d;
    const bool text = prop == kTileText;
    if (text ? !code_matches(RVA_BADGE_TYPE, badge_type, sizeof badge_type)
             : !code_matches(RVA_COLOR_TYPE, color_type, sizeof color_type) || !code_matches(RVA_BOX_COLOR, box_color, sizeof box_color))
        return nullptr;
    static std::deque<std::string> names;   // the descriptor keeps the pointer
    std::string name;
    for (const wchar_t* c = kTilePropNames[prop]; *c; ++c) name += static_cast<char>(*c);
    names.push_back(name + std::to_string(t));
    void* type = reinterpret_cast<TypeFn>(g_base + (text ? RVA_BADGE_TYPE : RVA_COLOR_TYPE))(0);
    d = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&tile_prop_get), reinterpret_cast<void*>(&badge_set), type);
    return d;
}

// The MODS tab's own tiles' names, FH2Mod_HubTile_<k> (as their Button's Tag, the way FH2Mod_TileText_ is): asked for
// as the pause menu shows them, so they're in the game's language then (it has none yet when A_PauseMenu.xaml is
// written), with the number of updates as it is then. Strings like FH2Mod_TileText_'s. After the five names come
// the tiles' other lines (kHubLine...): YOUR MODS' subtitle and the mods' names, GET MODS' line, and the Settings
// page's MODS tile's.
constexpr int kHubTiles = 5;   // YOUR MODS, UPDATES, GET MODS, ABOUT, HELP: write_pause_menu's order
enum HubLine { kHubLineMods = kHubTiles, kHubLineName1, kHubLineName4 = kHubLineName1 + 3, kHubLineGetMods, kHubLineSettings, kHubTexts };
uint8_t* g_hub_tile_desc[kHubTexts] = {};
std::atomic<void*> g_hub_tile_obj[kHubTexts] = {};   // the page that read each (the pause menu; Settings for its tile)
std::string hub_tile_text(int k);               // with the MODS tab's pages, below
void** __cdecl hub_tile_get(void** out, void* desc, void* obj) {
    *out = nullptr;
    for (int k = 0; k < kHubTexts; ++k)
        if (g_hub_tile_desc[k] == desc) {
            if (obj) g_hub_tile_obj[k] = obj;
            uint8_t* v = tile_string(hub_tile_text(k));
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(v + 0x10));   // the caller releases it
            *out = v;
            break;
        }
    return out;
}
uint8_t* hub_tile_descriptor(int k) {
    std::scoped_lock lk{g_desc_mu};
    static const uint8_t badge_type[] = {0x48, 0x8D, 0x05, 0x59, 0xB0, 0x02, 0x02, 0xC3};
    uint8_t*& d = g_hub_tile_desc[k];
    if (d || !code_matches(RVA_BADGE_TYPE, badge_type, sizeof badge_type)) return d;
    static std::deque<std::string> names;   // the descriptor keeps the pointer
    names.push_back("FH2Mod_HubTile_" + std::to_string(k));
    void* type = reinterpret_cast<TypeFn>(g_base + RVA_BADGE_TYPE)(0);
    d = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&hub_tile_get), reinterpret_cast<void*>(&badge_set), type);
    return d;
}

// The panel under YOUR MODS' list (write_list_page's kPanelBound), the hint line: FH2Mod_PanelShown on the list
// and on each mod's page, FH2Mod_PanelText what it says there, an AVUI String like the MODS tab's tile names. On the
// list, a hint; on a mod's page, the legend for the rows marked *; on either, once a restart row has changed, the
// rows that wait for a restart. On a page of a mod's own (a view of YOUR MODS, or on its tile) it's the page's hint
// line (API 10): the hint show_page_hint picked, or nothing. FH2Mod_PanelKind_<k> says which look (FH2_HINT_) the
// text is shown in: INFO for the platform's own.
uint8_t* g_panel_text_desc  = nullptr;
uint8_t* g_panel_shown_desc = nullptr;
uint8_t* g_panel_kind_desc[FH2_HINT_FAULT + 1] = {};
std::atomic<void*> g_panel_obj{nullptr};   // what the panel's bindings read from
std::string pending_restart_names();       // with the MODS tab's rows, below
// The mod's page whose hint line is up, or -1; and its hint now ("" for none), with its kind (with the pages, below).
int hint_page_now();
std::string page_hint_text(int j, uint32_t* kind);
bool panel_shown() {
    if (const int j = hint_page_now(); j >= 0) return !page_hint_text(j, nullptr).empty();
    const int v = g_view.load();
    return v == 0 || view_kind(v) == kViewMod;
}
std::string panel_text() {
    if (const int j = hint_page_now(); j >= 0) return page_hint_text(j, nullptr);
    const int v = g_view.load();
    if (!panel_shown()) return "";
    if (const std::string pending = pending_restart_names(); !pending.empty()) return trf("Restart the game for: {0}.", {pending});
    return v == 0 ? tr("Pick a mod to change its settings, open its pages or turn it off.")
                  : tr("* Takes effect the next time you start the game.");
}
uint32_t panel_kind() {
    uint32_t kind = FH2_HINT_INFO;
    if (const int j = hint_page_now(); j >= 0) page_hint_text(j, &kind);
    return kind;
}
void** __cdecl panel_get(void** out, void* desc, void* obj) {
    *out = nullptr;
    if (obj) g_panel_obj = obj;
    if (desc == g_panel_shown_desc) {
        bool v = panel_shown();
        reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
        return out;
    }
    for (uint32_t k = 0; k <= FH2_HINT_FAULT; ++k)
        if (desc == g_panel_kind_desc[k]) {
            bool v = panel_kind() == k;
            reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
            return out;
        }
    uint8_t* v = tile_string(panel_text());
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(v + 0x10));   // the caller releases it
    *out = v;
    return out;
}
uint8_t* panel_descriptor(bool text) {
    std::scoped_lock lk{g_desc_mu};
    static const uint8_t string_type[] = {0x48, 0x8D, 0x05, 0x59, 0xB0, 0x02, 0x02, 0xC3};
    uint8_t*& d = text ? g_panel_text_desc : g_panel_shown_desc;
    if (d) return d;
    if (text) {
        if (!code_matches(RVA_BADGE_TYPE, string_type, sizeof string_type)) return nullptr;
        d = new_descriptor("FH2Mod_PanelText", reinterpret_cast<void*>(&panel_get), reinterpret_cast<void*>(&badge_set),
                           reinterpret_cast<TypeFn>(g_base + RVA_BADGE_TYPE)(0));
    } else {
        d = new_descriptor("FH2Mod_PanelShown", reinterpret_cast<void*>(&panel_get), reinterpret_cast<void*>(&notice_set),
                           reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0));
    }
    return d;
}
uint8_t* panel_kind_descriptor(int k) {
    std::scoped_lock lk{g_desc_mu};
    uint8_t*& d = g_panel_kind_desc[k];
    if (!d) {
        static std::deque<std::string> names;   // the descriptor keeps the pointer
        names.push_back("FH2Mod_PanelKind_" + std::to_string(k));
        d = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&panel_get), reinterpret_cast<void*>(&notice_set),
                           reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0));
    }
    return d;
}
void notify_panel(void* obj) {
    if (!obj) obj = g_panel_obj.load();
    if (!obj) return;
    for (uint8_t* d : {g_panel_text_desc, g_panel_shown_desc, g_panel_kind_desc[0], g_panel_kind_desc[1], g_panel_kind_desc[2]})
        if (d) notify_changed(obj, d);
}

uint8_t* notice_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_notice_desc) {
        void* bool_type = reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0);
        g_notice_desc   = new_descriptor("FH2Mod_RestartNotice", reinterpret_cast<void*>(&notice_get),
                                         reinterpret_cast<void*>(&notice_set), bool_type);
    }
    return g_notice_desc;
}

// FH2Mod_ModNotice: whether the freeroam HUD shows its notice about the mods (see write_hud_notice: a mod
// that didn't load, the first-run welcome or the mods new this time). The game builds the HUD page early, around the start screen, so the value
// is false until the page is first shown (its vtable +0x488, see d_hud_shown), true for a
// little while then, and false again from the next time it's shown: once a session.
uint8_t* g_mod_notice_desc = nullptr;
std::atomic<ULONGLONG> g_mod_notice_at{0};   // when the HUD was first shown (0: not yet)
std::atomic<bool> g_mod_notice_done{false};
enum NoticeKind { kNoticeNone, kNoticeFailed, kNoticeWelcome, kNoticeNewMods };
int g_notice_kind = kNoticeNone;         // what the notice is this session (set before the hooks)
constexpr int kWelcomeSessions = 3;      // the welcome plays in this many sessions at most
bool mod_notice_on() {
    const ULONGLONG at = g_mod_notice_at.load();
    return at && !g_mod_notice_done.load() && GetTickCount64() - at < 15000;
}
void** __cdecl mod_notice_get(void** out, void*, void*) {
    bool v = mod_notice_on();
    *out   = nullptr;
    reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
    return out;
}
uint8_t* mod_notice_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_mod_notice_desc) {
        void* bool_type   = reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0);
        g_mod_notice_desc = new_descriptor("FH2Mod_ModNotice", reinterpret_cast<void*>(&mod_notice_get),
                                           reinterpret_cast<void*>(&notice_set), bool_type);
    }
    return g_mod_notice_desc;
}

// FH2Mod_TitleReady: whether the title screen is hooked, so the lines of the mods' title-screen
// entries can show (write_title_page keeps them collapsed until it's true; a binding that never
// resolves leaves the title screen as the game ships it).
uint8_t* g_title_desc = nullptr;
std::atomic<bool> g_title_ready{false};
void** __cdecl title_ready_get(void** out, void*, void*) {
    bool v = g_title_ready.load();
    *out   = nullptr;
    reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
    return out;
}
uint8_t* title_ready_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_title_desc) {
        void* bool_type = reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0);
        g_title_desc    = new_descriptor("FH2Mod_TitleReady", reinterpret_cast<void*>(&title_ready_get),
                                         reinterpret_cast<void*>(&notice_set), bool_type);
    }
    return g_title_desc;
}

// ---- pages of a mod's own design: their slots (API 8) ------------------------------------------
// FH2Mod_Slot_<n>: an int of the page's, for two-way bindings; FH2Mod_Slot_<n>_Is_<v>: whether
// it's v. They read the page being opened or shown (g_custom_shown). A mod changes them from its
// callbacks, which run with g_in_callback set (on the UI thread, while the page is up).
std::atomic<int> g_custom_shown{-1};       // the page in g_pages whose slots the bindings read
std::atomic<void*> g_custom_obj{nullptr};  // what its bindings read from (for PropertyChanged); null when it's gone
thread_local bool g_in_callback = false;   // in a mod's on_button or on_slot for the page showing
thread_local void* g_callback_page = nullptr;   // in on_button: that page (for focusing); null in on_slot
thread_local int g_setting_slot = -1;           // in on_slot: the slot a binding is setting
// Sets the three for a callback, and puts back what they were (callbacks can nest: a slot set
// from on_button can come back through a binding into on_slot).
struct InCallback {
    bool in;
    void* page;
    int slot;
    InCallback(void* p, int setting) : in(g_in_callback), page(g_callback_page), slot(g_setting_slot) {
        g_in_callback   = true;
        g_callback_page = p;
        g_setting_slot  = setting;
    }
    ~InCallback() {
        g_in_callback   = in;
        g_callback_page = page;
        g_setting_slot  = slot;
    }
};
uint8_t* g_slot_desc[FH2_PAGE_SLOTS] = {};
struct SlotIs {
    int slot, value;
    uint8_t* desc;
};
std::vector<SlotIs> g_slot_is;             // guarded by g_desc_mu

int slot_value(int n) {
    const int j = g_custom_shown.load();
    std::scoped_lock lk{g_mu};
    if (j < 0 || static_cast<std::size_t>(j) >= g_pages.size() || n < 0 || n >= FH2_PAGE_SLOTS) return 0;
    return g_pages[j].slots[n].load();
}

int slot_of(void* desc) {
    for (int n = 0; n < FH2_PAGE_SLOTS; ++n)
        if (g_slot_desc[n] == desc) return n;
    return -1;
}

// Tells the page's bindings slot n changed: its _Is_ properties, and the slot itself if asked.
void notify_slot(int n, bool self) {
    void* obj = g_custom_obj.load();
    if (!obj) return;
    std::vector<uint8_t*> descs;
    {
        std::scoped_lock lk{g_desc_mu};
        for (const SlotIs& s : g_slot_is)
            if (s.slot == n) descs.push_back(s.desc);
        if (self && g_slot_desc[n]) descs.push_back(g_slot_desc[n]);
    }
    for (uint8_t* d : descs) notify_changed(obj, d);
}

bool call_on_slot(FH2PageSlotFn fn, void* user, const char* key, int slot, int value) {
    __try {
        fn(user, key, slot, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void** __cdecl slot_get(void** out, void* desc, void* obj) {
    if (obj) g_custom_obj = obj;
    int v = slot_value(slot_of(desc));
    *out  = nullptr;
    reinterpret_cast<BoxIntFn>(g_base + RVA_BOX_INT)(out, &v);
    return out;
}
void __cdecl slot_set(void* desc, void* obj, uint8_t* value) {
    const int n = slot_of(desc), j = g_custom_shown.load();
    if (n < 0 || j < 0 || !value || !readable(value + 0x28, 4)) return;
    if (obj) g_custom_obj = obj;
    const int v = *reinterpret_cast<int*>(value + 0x28);
    FH2Mod* mod = nullptr;
    FH2PageSlotFn fn = nullptr;
    void* user = nullptr;
    std::string key;
    {
        std::scoped_lock lk{g_mu};
        if (static_cast<std::size_t>(j) >= g_pages.size()) return;
        ModPage& pg = g_pages[j];
        if (pg.slots[n].exchange(v) == v) return;
        mod  = pg.mod;
        fn   = pg.on_slot;
        user = pg.user;
        key  = pg.key;
    }
    notify_slot(n, false);
    if (!fn) return;
    // inside the binding's set: no focusing from here, and the slot being set isn't notified again
    InCallback in{nullptr, n};
    UiCallback cb{mod, UiCb::page, g_last_hud_page.load(), j};
    ui_note_callback(mod);
    if (!call_on_slot(fn, user, key.c_str(), n, v)) crashed_in(mod, "on_slot for page " + key);
}
void** __cdecl slot_is_get(void** out, void* desc, void* obj) {
    if (obj) g_custom_obj = obj;
    int n = -1, want = 0;
    {
        std::scoped_lock lk{g_desc_mu};
        for (const SlotIs& s : g_slot_is)
            if (s.desc == desc) {
                n    = s.slot;
                want = s.value;
            }
    }
    bool v = n >= 0 && slot_value(n) == want;
    *out   = nullptr;
    reinterpret_cast<BoxBoolFn>(g_base + RVA_BOX_BOOL)(out, &v);
    return out;
}

// "FH2Mod_Slot_<n>" or "FH2Mod_Slot_<n>_Is_<v>"
uint8_t* slot_descriptor(const wchar_t* n) {
    const wchar_t* d = n + 12;   // after "FH2Mod_Slot_"
    int slot = 0, digits = 0;
    for (; *d >= L'0' && *d <= L'9' && digits < 3; ++d, ++digits) slot = slot * 10 + (*d - L'0');
    if (!digits || slot >= FH2_PAGE_SLOTS) return nullptr;
    if (!*d) {
        std::scoped_lock lk{g_desc_mu};
        if (!g_slot_desc[slot]) {
            static std::deque<std::string> names;   // the descriptor keeps the pointer
            names.push_back("FH2Mod_Slot_" + std::to_string(slot));
            g_slot_desc[slot] = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&slot_get), reinterpret_cast<void*>(&slot_set), nullptr);
        }
        return g_slot_desc[slot];
    }
    if (std::wcsncmp(d, L"_Is_", 4) != 0) return nullptr;
    d += 4;
    int value = 0;
    digits    = 0;
    for (; *d >= L'0' && *d <= L'9' && digits < 4; ++d, ++digits) value = value * 10 + (*d - L'0');
    if (!digits || *d) return nullptr;
    std::scoped_lock lk{g_desc_mu};
    for (const SlotIs& s : g_slot_is)
        if (s.slot == slot && s.value == value) return s.desc;
    static std::deque<std::string> names;
    names.push_back("FH2Mod_Slot_" + std::to_string(slot) + "_Is_" + std::to_string(value));
    void* bool_type = reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0);
    uint8_t* desc   = new_descriptor(names.back().c_str(), reinterpret_cast<void*>(&slot_is_get), reinterpret_cast<void*>(&view_set), bool_type);
    if (desc) g_slot_is.push_back({slot, value, desc});
    return desc;
}

using FindPropertyFn = uint8_t* (*)(uint8_t* type, uint8_t* name);
FindPropertyFn o_find_property = nullptr;

const wchar_t* avui_string(uint8_t* name) {
    __try {
        const wchar_t* n = name && readable(name + 0x28, 8) ? *reinterpret_cast<const wchar_t**>(name + 0x28) : nullptr;
        return n && readable(n, 16) ? n : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t* d_find_property(uint8_t* type, uint8_t* name) {
    if (uint8_t* r = o_find_property(type, name)) return r;
    const wchar_t* n = avui_string(name);
    if (!n || std::wcsncmp(n, L"FH2Mod_", 7) != 0) return nullptr;
    if (std::wcscmp(n, L"FH2Mod_RestartNotice") == 0) return notice_descriptor();
    if (std::wcscmp(n, L"FH2Mod_PanelText") == 0) return panel_descriptor(true);
    if (std::wcscmp(n, L"FH2Mod_PanelShown") == 0) return panel_descriptor(false);
    if (const int k = name_number(n, L"FH2Mod_PanelKind_", FH2_HINT_FAULT + 1); k >= 0) return panel_kind_descriptor(k);
    if (const int k = name_number(n, L"FH2Mod_HelpBar_", kMaxViews); k >= 0) return flag_descriptor(g_help_bar_desc[k], "FH2Mod_HelpBar_" + std::to_string(k));
    if (std::wcscmp(n, L"FH2Mod_Selected") == 0) return selected_descriptor();
    if (std::wcscmp(n, L"FH2Mod_ViewSelected") == 0) return view_selected_descriptor();
    for (int kind = 0; kind < kBadgeKinds; ++kind)
        if (std::wcscmp(n, widen(kBadgeNames[kind]).c_str()) == 0) return badge_descriptor(kind);
    if (std::wcscmp(n, L"FH2Mod_ModNotice") == 0) return mod_notice_descriptor();
    if (std::wcscmp(n, L"FH2Mod_TitleReady") == 0) return title_ready_descriptor();
    if (const int k = name_number(n, L"FH2Mod_View_", kMaxViews); k >= 0) return flag_descriptor(g_view_desc[k], "FH2Mod_View_" + std::to_string(k));
    if (const int k = name_number(n, L"FH2Mod_ViewIn_", kMaxViews); k >= 0)
        return flag_descriptor(g_view_in_desc[k], "FH2Mod_ViewIn_" + std::to_string(k));
    if (const int k = name_number(n, L"FH2Mod_ViewBack_", kMaxViews); k >= 0)
        return flag_descriptor(g_view_back_desc[k], "FH2Mod_ViewBack_" + std::to_string(k));
    if (const int b = name_number(n, L"FH2Mod_Bar_", kViewKinds); b >= 0) return flag_descriptor(g_bar_desc[b], "FH2Mod_Bar_" + std::to_string(b));
    for (int prop = 0; prop < kTileProps; ++prop)
        if (const int t = name_number(n, kTilePropNames[prop], static_cast<int>(kTileCount)); t >= 0) return tile_descriptor(prop, t);
    if (const int k = name_number(n, L"FH2Mod_HubTile_", kHubTexts); k >= 0) return hub_tile_descriptor(k);
    if (std::wcsncmp(n, L"FH2Mod_Slot_", 12) == 0) return slot_descriptor(n);
    if (std::wcsncmp(n, L"FH2Mod_Hud_", 11) == 0) return hud_layer_find(n);
    Row* r = row_by_prop(n);
    return r ? descriptor_for(r) : nullptr;
}

// ---- the pages ----------------------------------------------------------------------------

// xml_escape (mods' text in the pages, non-ASCII as &#NNN;) is in xml_text.cpp.

std::string read_ui(const char* name) {
    const auto zip = game_mount() / L"media" / L"UI.zip";
    std::vector<char> x = unzip_file(zip, name);
    if (x.empty()) slog(std::string("couldn't read ") + name + " from " + narrow(zip.wstring()));
    return std::string(x.begin(), x.end());
}

// Replaces what's between `open` (or the end of its tag) and `close`.
bool replace_between(std::string& s, const std::string& open, const std::string& close, const std::string& with,
                     bool after_tag) {
    std::size_t a = s.find(open);
    if (a == std::string::npos) return false;
    a = after_tag ? s.find('>', a) : a + open.size() - 1;
    if (a == std::string::npos) return false;
    const std::size_t b = s.find(close, a);
    if (b == std::string::npos) return false;
    s.replace(a + 1, b - (a + 1), with);
    return true;
}

std::string num(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(f));
    return b;
}

// A slider's style: A_Style_Slider_Options (Shared_OptionsScreens.xaml) with the row's range,
// step and number. Shown as a percentage (scale 100) it uses the game's own "fracpercentage";
// otherwise the converter's Scale.
std::string slider_resources(const Row& r, std::size_t i) {
    const std::string n     = std::to_string(i);
    const bool percent      = std::fabs(r.scale - 100.0f) < 1e-3f;
    std::string res = "            <local:NumUnitConverter x:Key=\"FH2Mod_Num_" + n + "\" FractionDigits=\"" +
                      std::to_string(r.decimals) + "\" ShowUnit=\"False\"" +
                      (percent || r.scale == 1.0f ? "" : " Scale=\"" + num(r.scale) + "\"") + " />\r\n";
    res += "            <Style x:Key=\"FH2Mod_Slider_" + n + "\" TargetType=\"Slider\" BasedOn=\"{StaticResource A_Style_Slider_Options}\">\r\n"
           "                <Setter Property=\"Minimum\" Value=\"" + num(r.fmin) + "\" />\r\n"
           "                <Setter Property=\"Maximum\" Value=\"" + num(r.fmax) + "\" />\r\n"
           "                <Setter Property=\"SmallChange\" Value=\"" + num(r.fstep) + "\" />\r\n"
           "                <Setter Property=\"Template\">\r\n"
           "                    <Setter.Value>\r\n"
           "                        <ControlTemplate TargetType=\"Slider\">\r\n"
           "                            <Grid FocusManager.IsFocusScope=\"True\">\r\n"
           "                                <Grid.ColumnDefinitions><ColumnDefinition /><ColumnDefinition /></Grid.ColumnDefinitions>\r\n"
           "                                <local:FZTextBlock x:Name=\"PART_Label\" Text=\"{Binding Tag.Heading, RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Left\" />\r\n"
           "                                <DockPanel Grid.Column=\"1\">\r\n"
           "                                    <local:FZTextBlock DockPanel.Dock=\"Right\" Width=\"64\" TextAlignment=\"Right\" HorizontalAlignment=\"Right\" "
           "Text=\"{Binding Value, RelativeSource={RelativeSource TemplatedParent}, " +
           (percent ? std::string("ConverterParameter=fracpercentage, ") : std::string()) + "Converter={StaticResource FH2Mod_Num_" + n +
           "}}\" Style=\"{StaticResource A_Style_SliderControlText}\" />\r\n"
           "                                    <Grid Margin=\"32, 0, 8, 0\" Height=\"22\">\r\n"
           "                                        <local:FZMaterial x:Name=\"PART_SliderMaterial\" Color=\"{StaticResource A_Color_MediumGreyText}\" />\r\n"
           "                                        <Track x:Name=\"PART_Track\">\r\n"
           "                                            <Track.DecreaseRepeatButton>\r\n"
           "                                                <RepeatButton x:Name=\"Left\" Style=\"{StaticResource A_Style_RepeatButton_Left_Unfocused}\"/>\r\n"
           "                                            </Track.DecreaseRepeatButton>\r\n"
           "                                        </Track>\r\n"
           "                                    </Grid>\r\n"
           "                                </DockPanel>\r\n"
           "                            </Grid>\r\n"
           "                            <ControlTemplate.Triggers>\r\n"
           "                                <DataTrigger Binding=\"{Binding RelativeSource={RelativeSource Mode=FindAncestor, AncestorType=ListBoxItem}, Path=IsKeyboardFocusWithin}\" Value=\"True\">\r\n"
           "                                    <Setter TargetName=\"PART_SliderMaterial\" Property=\"Color\" Value=\"{StaticResource A_Color_Header_Background}\"/>\r\n"
           "                                    <Setter TargetName=\"Left\" Property=\"Style\" Value=\"{StaticResource A_Style_RepeatButton_Left_Focused}\"/>\r\n"
           "                                </DataTrigger>\r\n"
           "                            </ControlTemplate.Triggers>\r\n"
           "                        </ControlTemplate>\r\n"
           "                    </Setter.Value>\r\n"
           "                </Setter>\r\n"
           "            </Style>\r\n";
    return res;
}

// A choice row's style (API 9): laid out like the game's own option rows (A_Style_SpinnerControl_Default,
// Shared_Spinners.xaml: the label, then the value between a left and a right arrow), but a Slider over
// the choices' numbers, so it binds to a float property of ours like a slider. The control's
// SpinnerControl takes its items from a list the page's C++ object makes, which a page of ours
// can't; this shows each name by the slider's Value instead, as the game's own rating slider shows
// its stars (F606_RateDialog.xaml: Trigger Property="Value" Value="1", ...). The Track is there,
// unseen, for the Slider's own workings.
std::string choice_resources(const Row& r, std::size_t i) {
    const std::string n    = std::to_string(i);
    const std::size_t last = r.choices.size() - 1;
    std::string names, triggers;
    for (std::size_t c = 0; c <= last; ++c) {
        names += "                                        <local:FZTextBlock x:Name=\"FH2Mod_Choice_" + std::to_string(c) + "\" Text=\"" +
                 xml_escape(r.choices[c]) + "\" Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Center\"" +
                 (c ? " Visibility=\"Hidden\"" : "") + " />\r\n";
        std::string set;
        if (c == 0) set += "                                    <Setter TargetName=\"LeftArrow\" Property=\"Visibility\" Value=\"Hidden\" />\r\n";
        if (c == last) set += "                                    <Setter TargetName=\"RightArrow\" Property=\"Visibility\" Value=\"Hidden\" />\r\n";
        if (c) {
            set += "                                    <Setter TargetName=\"FH2Mod_Choice_0\" Property=\"Visibility\" Value=\"Hidden\" />\r\n";
            set += "                                    <Setter TargetName=\"FH2Mod_Choice_" + std::to_string(c) +
                   "\" Property=\"Visibility\" Value=\"Visible\" />\r\n";
        }
        triggers += "                                <Trigger Property=\"Value\" Value=\"" + std::to_string(c) + "\">\r\n" + set +
                    "                                </Trigger>\r\n";
    }
    return "            <Style x:Key=\"FH2Mod_Choice_" + n + "\" TargetType=\"Slider\" BasedOn=\"{StaticResource A_Style_Slider_Options}\">\r\n"
           "                <Setter Property=\"Minimum\" Value=\"0\" />\r\n"
           "                <Setter Property=\"Maximum\" Value=\"" + std::to_string(last) + "\" />\r\n"
           "                <Setter Property=\"SmallChange\" Value=\"1\" />\r\n"
           "                <Setter Property=\"LargeChange\" Value=\"1\" />\r\n"
           "                <Setter Property=\"Template\">\r\n"
           "                    <Setter.Value>\r\n"
           "                        <ControlTemplate TargetType=\"Slider\">\r\n"
           "                            <Grid FocusManager.IsFocusScope=\"True\" VerticalAlignment=\"Center\">\r\n"
           "                                <Grid.ColumnDefinitions><ColumnDefinition /><ColumnDefinition /></Grid.ColumnDefinitions>\r\n"
           "                                <local:FZTextBlock x:Name=\"PART_Label\" Text=\"{Binding Tag.Heading, RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Left\" />\r\n"
           "                                <Grid Grid.Column=\"1\">\r\n"
           "                                    <Track x:Name=\"PART_Track\" Opacity=\"0\" />\r\n"
           "                                    <DockPanel>\r\n"
           "                                        <Grid DockPanel.Dock=\"Left\"><ContentControl x:Name=\"LeftArrow\" Style=\"{StaticResource A_Style_Arrow_Left}\" /></Grid>\r\n"
           "                                        <Grid DockPanel.Dock=\"Right\"><ContentControl x:Name=\"RightArrow\" Style=\"{StaticResource A_Style_Arrow_Right}\" /></Grid>\r\n"
           "                                        <Grid>\r\n" +
           names +
           "                                        </Grid>\r\n"
           "                                    </DockPanel>\r\n"
           "                                </Grid>\r\n"
           "                            </Grid>\r\n"
           "                            <ControlTemplate.Triggers>\r\n" +
           triggers +
           "                            </ControlTemplate.Triggers>\r\n"
           "                        </ControlTemplate>\r\n"
           "                    </Setter.Value>\r\n"
           "                </Setter>\r\n"
           "            </Style>\r\n";
}

// A page laid out like the HUD options page (A_HUDOptions.xaml), with a list of our own. An item
// is one of our rows, or a line of information: a row with a label and a description and no
// value to change.
struct PageItem {
    int row = -1;                    // index in g_rows, or -1 for a line of information
    std::string label, description;
    std::string value;               // a line of information: shown on the right
    std::string value_alert;         // or this, in red (a mod that isn't working)
    bool yellow = false;             // the value in yellow: a newer version is out (yellow always means an update)
    int button = -1;                 // a line of information that opens something: its button number
    // under the description, each on a line of its own (Installed mods): a note (turned off,
    // and so on), the version, the same in red when there's a newer one, and the GitHub page
    std::string note, version, version_alert, link;
    bool note_alert = false;         // the note is an error (shown in red)
    std::string open;                // what A opens on this row (a link row's URL), or ""
    int style = FH2_ROW_NORMAL;      // a line of information: FH2_ROW_DIM, FH2_ROW_HEADING (API 8)
    std::string name;                // its x:Name, when something focuses it ("" for none)
    int open_view = 0;               // on Installed mods: the view (a mod's page) A shows, or 0
    std::string select;              // a page's row: the key A passes to on_select, or ""
    bool skip = false;               // a line of information the cursor passes over (not the first row)
    std::string hint;                // the last sentence of the description: what A does here
};

// A row's x:Name, so it can be focused: the first row is always `first_name` (the page focuses
// it); a line of information can have a name of its own or a button number; on a mod's page every
// row has one (`auto_names` and its place), so Go back and B can come back to it.
std::string item_name(const PageItem& it, std::size_t n, const std::string& first_name, const std::string& auto_names = "") {
    if (n == 0) return first_name;
    if (!it.name.empty()) return it.name;
    if (it.button >= 0) return "PART_Button_FH2Mod_" + std::to_string(it.button);
    return auto_names.empty() ? "" : auto_names + std::to_string(n);
}

// a line of information's look (FH2_ROW_...): its template (see write_list_page)
const char* info_template(int style, bool yellow) {
    switch (style) {
        case FH2_ROW_DIM: return "FH2Mod_InfoRow_Dim";
        case FH2_ROW_HEADING: return "FH2Mod_InfoRow_Heading";
        default: return yellow ? "FH2Mod_InfoRow_Update" : "FH2Mod_InfoRow";
    }
}

// The rows of one list, as XAML; their data items (and slider styles) go into `res`. Keys
// start with `prefix`; the first row is named `first_name`, and the others as item_name says.
std::string list_rows(const std::vector<PageItem>& items, const std::string& prefix, const std::string& first_name, std::string& res,
                      const std::string& auto_names = "") {
    std::string rows = "\r\n";
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t n = 0; n < items.size(); ++n) {
            const PageItem& it = items[n];
            const std::string key   = prefix + std::to_string(n);
            const std::string nm    = item_name(it, n, first_name, auto_names);
            const std::string first = nm.empty() ? "" : " x:Name=\"" + nm + "\"";
            // The row's data item (Tag): Heading is its label (a slider's style shows it),
            // Description goes in the panel. A line of information about a mod also has a
            // second one as its Content, for the footer under the description (see
            // write_list_page): Heading the version, Description the same in red when a newer
            // one is out (only one of the two is filled), Icon the GitHub page. Its own label and
            // value are on the button (BindingProxy.A, .C) for FH2Mod_InfoRow.
            // A note goes after the description, except an error on a line of information: that
            // shows in red at the top of the footer (from the data item's Icon, which a line of
            // information doesn't otherwise use; FZTextBlock doesn't break lines, not even at
            // &#10;, so it can't sit a line below in the same text).
            const bool info_row  = it.row < 0 || static_cast<std::size_t>(it.row) >= g_rows.size();
            const bool red_note  = info_row && it.note_alert;
            auto data_item = [&](const std::string& label, const std::string& desc) {
                std::string d = desc;
                if (!it.note.empty() && !red_note) d += (d.empty() ? "" : " ") + it.note;
                if (!it.hint.empty()) d = join_sentences(d, it.hint);
                res += "            <local:TileMenuDataItem x:Key=\"" + key + "\" Heading=\"" + xml_escape(label) + "\" Description=\"" +
                       xml_escape(d) + "\" Icon=\"" + (red_note ? xml_escape(it.note) : "") + "\" />\r\n";
            };
            std::string content;
            if (!it.version.empty() || !it.version_alert.empty() || !it.link.empty()) {
                res += "            <local:TileMenuDataItem x:Key=\"" + key + "_V\" Heading=\"" + xml_escape(it.version) + "\" Description=\"" +
                       xml_escape(it.version_alert) + "\" Icon=\"" + xml_escape(it.link) + "\" />\r\n";
                content = " Content=\"{StaticResource " + key + "_V}\"";
            }
            if (info_row) {
                data_item(it.label, it.description);
                const std::string& name = nm;
                // The list's items aren't focusable themselves (A_Style_ListBoxItem_List_Default), only
                // what's in them, so a Button that isn't leaves the cursor nothing to stop on there.
                const bool skip = it.skip && n > 0;
                rows += "                                        <Button Tag=\"{StaticResource " + key +
                        "}\" local:BindingProxy.A=\"" + xml_escape(it.label) + "\" local:BindingProxy.C=\"" + xml_escape(it.value) + "\" local:BindingProxy.B=\"" + xml_escape(it.value_alert) +
                        "\"" + content + " Template=\"{StaticResource " + info_template(it.style, it.yellow) + "}\"" + (name.empty() ? "" : " x:Name=\"" + name + "\"") +
                        (skip ? " Focusable=\"False\"" : "") + " />\r\n";
                continue;
            }
            // A row that needs a restart ends in " *" (the panel under the list says what the mark means), and
            // says so at the end of its description. No "(Mod)" after it: every row on a mod's page is its own.
            const Row& r     = g_rows[it.row];
            const std::string label = (it.label.empty() ? r.label : it.label) + (r.restart ? " *" : "");
            std::string desc = it.description.empty() ? r.description : it.description;
            if (r.restart) desc = join_sentences(desc, tr("Takes effect the next time you start the game."));
            data_item(label, desc);
            if (!r.choices.empty()) {
                res += choice_resources(r, it.row);
                rows += "                                        <Slider Value=\"{Binding " + r.prop + "}\" Tag=\"{StaticResource " + key +
                        "}\" Style=\"{StaticResource FH2Mod_Choice_" + std::to_string(it.row) + "}\"" + first + " />\r\n";
                continue;
            }
            if (r.slider) {
                res += slider_resources(r, it.row);
                rows += "                                        <Slider Value=\"{Binding " + r.prop + "}\" Tag=\"{StaticResource " + key +
                        "}\" Style=\"{StaticResource FH2Mod_Slider_" + std::to_string(it.row) + "}\"" + first + " />\r\n";
                continue;
            }
            rows += "                                        <local:SpinnerControl Label=\"" + xml_escape(label) + "\" Tag=\"{StaticResource " +
                    key + "}\" SelectedIndex=\"{Binding " + r.prop +
                    "}\" ItemsSource=\"{Binding OnOffOptions}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\"" + first +
                    " />\r\n";
        }
    }
    rows += "                                ";
    return rows;
}

// sets an attribute of one element's XAML (adds it if it's not there)
void swap_attr(std::string& el, const std::string& name, const std::string& value) {
    const std::size_t a = el.find(name + "=\"");
    if (a == std::string::npos) {
        el.insert(el.find(' '), " " + name + "=\"" + value + "\"");
        return;
    }
    const std::size_t v = a + name.size() + 2, e = el.find('"', v);
    el.replace(v, e - v, value);
}

// A view of YOUR MODS: a mod's page ("MODS / <MOD>", settings true) or a page of the mod's own (its title alone).
struct ViewTitle {
    std::string title;
    bool settings = true;
};
// The grey part of a title before the white one, as in the game's "SETTINGS / HUD": the MODS tab's
// pages and each mod's page say MODS (the tab's own name, in English in every language).
const char* const kModsTag = "MODS";

// What goes in the panel under a page's list (see write_list_page): nothing; the legend for the
// rows marked * (the list that stands in for the MODS tab); or the text of FH2Mod_PanelText while
// FH2Mod_PanelShown is true (YOUR MODS: a hint under the list, the legend or the rows waiting for a
// restart on a mod's page).
enum PanelKind { kPanelNone, kPanelLegend, kPanelBound };

// A line of information is a Button with one of these templates: label left, value right (or
// the same in red). FH2Mod_Text_Dim is the rows' text in grey, FH2Mod_Text_Heading in the
// game's orange; both turn the selected row's colour when it has the focus, as the rows' own
// text (A_Style_SliderControlText, Shared_OptionsScreens.xaml) does.
std::string info_row_resources() {
    auto text_style = [](const char* key, const char* color) {
        return std::string("            <Style x:Key=\"") + key +
               "\" TargetType=\"local:FZTextBlock\" BasedOn=\"{StaticResource A_Style_SliderControlText}\">\r\n"
               "                <Setter Property=\"Color\" Value=\"" + color + "\" />\r\n"
               "                <Style.Triggers>\r\n"
               "                    <DataTrigger Binding=\"{Binding RelativeSource={RelativeSource Mode=FindAncestor, AncestorType=ListBoxItem}, "
               "Path=IsKeyboardFocusWithin}\" Value=\"True\">\r\n"
               "                        <Setter Property=\"Color\" Value=\"{StaticResource A_Color_TileSelected_Text}\" />\r\n"
               "                    </DataTrigger>\r\n"
               "                </Style.Triggers>\r\n"
               "            </Style>\r\n";
    };
    auto row = [](const char* key, const char* label_style, const char* value_style) {
        return std::string("            <ControlTemplate x:Key=\"") + key + "\" TargetType=\"{x:Type Button}\">\r\n"
               "                <Grid VerticalAlignment=\"Center\">\r\n"
               "                    <Grid.ColumnDefinitions>\r\n"
               "                        <ColumnDefinition />\r\n"
               "                        <ColumnDefinition />\r\n"
               "                    </Grid.ColumnDefinitions>\r\n"
               "                    <Grid Grid.Column=\"0\">\r\n"
               "                        <local:FZTextBlock Text=\"{Binding (local:BindingProxy.A), RelativeSource={RelativeSource TemplatedParent}}\" "
               "Style=\"{StaticResource " + label_style + "}\" HorizontalAlignment=\"Left\" />\r\n"
               "                    </Grid>\r\n"
               "                    <Grid Grid.Column=\"1\">\r\n"
               "                        <local:FZTextBlock Text=\"{Binding (local:BindingProxy.C), RelativeSource={RelativeSource TemplatedParent}}\" "
               "Style=\"{StaticResource " + value_style + "}\" HorizontalAlignment=\"Center\" />\r\n"
               "                        <local:FZTextBlock Text=\"{Binding (local:BindingProxy.B), RelativeSource={RelativeSource TemplatedParent}}\" "
               "Color=\"{StaticResource FH2Mod_Color_Red}\" "
               "Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Center\" />\r\n"
               "                    </Grid>\r\n"
               "                </Grid>\r\n"
               "            </ControlTemplate>\r\n";
    };
    // FH2Mod_Text_Update: an update's value, in the MODS tab's badge yellow (the selected row's colour while it has the focus)
    return text_style("FH2Mod_Text_Dim", "#FF8C8C8C") + text_style("FH2Mod_Text_Heading", "{StaticResource A_Color_TileSelected_Background}") +
           text_style("FH2Mod_Text_Update", "#FFFFC400") +
           row("FH2Mod_InfoRow", "A_Style_SliderControlText", "A_Style_SliderControlText") +
           row("FH2Mod_InfoRow_Dim", "FH2Mod_Text_Dim", "FH2Mod_Text_Dim") +
           row("FH2Mod_InfoRow_Heading", "FH2Mod_Text_Heading", "A_Style_SliderControlText") +
           row("FH2Mod_InfoRow_Update", "A_Style_SliderControlText", "FH2Mod_Text_Update");
}

// removes an attribute from one element's XAML
void erase_attr(std::string& el, const std::string& name) {
    const std::size_t a = el.find(" " + name + "=\"");
    if (a == std::string::npos) return;
    const std::size_t e = el.find('"', a + name.size() + 3);
    if (e != std::string::npos) el.erase(a, e + 1 - a);
}

// ---- the help bar and hint line of a mod's page (API 10) ---------------------------------------

// What a prompt set_page_help accepted says: the game's own word from its HelpButtons table, as the platform's own
// help bars do ({local:Loc ...}, in the player's language), "On" and "Off" in the platform's (the game's own On and
// Off are in a table no page names them from), or the mod's label as literal text (braces made round, so the page
// doesn't read it as markup).
std::string help_value(const HelpPrompt& h) {
    switch (h.word) {
        case FH2_WORD_SELECT: return "{local:Loc HelpButtons.IDS_Select}";
        case FH2_WORD_BACK: return "{local:Loc HelpButtons.IDS_Back}";
        case FH2_WORD_ACCEPT: return "{local:Loc HelpButtons.IDS_Accept}";
        case FH2_WORD_TOGGLE: return "{local:Loc HelpButtons.IDS_Toggle}";
        case FH2_WORD_PREVIOUS: return "{local:Loc HelpButtons.IDS_Previous}";
        case FH2_WORD_NEXT: return "{local:Loc HelpButtons.IDS_Next}";
        case FH2_WORD_ON: return xml_escape(tr("On"));
        case FH2_WORD_OFF: return xml_escape(tr("Off"));
        default: break;
    }
    std::string s = h.label;
    for (char& c : s)
        if (c == '{') c = '(';
        else if (c == '}') c = ')';
    return xml_escape(s);
}

// A page's help bar: the game's DefaultHelpPanel `bar` with (A) Select when the page has on_select and (B) Back,
// which `help` (set_page_help's prompts) relabels or adds to; every other button's prompt taken out. The game's
// panel puts them in its own order.
std::string page_help_bar(std::string bar, const std::vector<HelpPrompt>& help, bool select) {
    const char* const attrs[FH2_GLYPH_RB + 1] = {nullptr,
                                                 "local:ControllerButtonPanel.ADescription",
                                                 "local:ControllerButtonPanel.BDescription",
                                                 "local:ControllerButtonPanel.XDescription",
                                                 "local:ControllerButtonPanel.YDescription",
                                                 "local:ControllerButtonPanel.LBumperDescription",
                                                 "local:ControllerButtonPanel.RBumperDescription"};
    std::string value[FH2_GLYPH_RB + 1];
    if (select) value[FH2_GLYPH_A] = "{local:Loc HelpButtons.IDS_Select}";
    value[FH2_GLYPH_B] = "{local:Loc HelpButtons.IDS_Back}";
    for (const HelpPrompt& h : help)
        if (h.glyph >= FH2_GLYPH_A && h.glyph <= FH2_GLYPH_RB) value[h.glyph] = help_value(h);
    for (uint32_t g = FH2_GLYPH_A; g <= FH2_GLYPH_RB; ++g) {
        if (value[g].empty()) erase_attr(bar, attrs[g]);
        else swap_attr(bar, attrs[g], value[g]);
    }
    return bar;
}

// A page view's help bar on YOUR MODS, or a page's on its tile: `own` when set_page_help gave it prompts.
struct ViewHelp {
    bool own    = false;
    bool select = false;
    std::vector<HelpPrompt> help;
};

// The hint line: the panel's text (FH2Mod_PanelText) in one of three looks, by FH2Mod_PanelKind_<k>: the rows' grey
// for INFO (the YOUR MODS hint and the restart notice look as they always have), the game's orange with its info
// icon for ATTENTION, red with its alert icon for FAULT (never yellow: on the platform's pages yellow means an
// update). `shown` is the converter that hides it while FH2Mod_PanelShown is false (hidden, not collapsed, so the page
// doesn't move as it comes and goes), `kind` the one that collapses the looks not in use.
std::string hint_panel(const std::string& shown, const std::string& kind, const std::string& margin) {
    auto vis  = [&kind](int k) { return " Visibility=\"{Binding FH2Mod_PanelKind_" + std::to_string(k) + ", Converter={StaticResource " + kind + "}}\""; };
    auto icon = [&vis](int k, const char* path) {
        return "                            <local:FZMaterial Grid.Column=\"0\" Width=\"24\" Height=\"24\" Margin=\"0,0,10,0\" VerticalAlignment=\"Top\" "
               "ImageSource=\"/ForzaUI;component/Textures/Anthem/" + std::string(path) + "\"" + vis(k) + " />\r\n";
    };
    auto text = [&vis](int k, const char* color) {
        return "                            <local:FZTextBlock Grid.Column=\"1\" Text=\"{Binding FH2Mod_PanelText}\" Color=\"" + std::string(color) +
               "\" Style=\"{StaticResource A_Style_FZTextBlock_Body_Tight}\" TextWrapping=\"Wrap\"" + vis(k) + " />\r\n";
    };
    return "\r\n                    <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_Panel_Background}\" Margin=\"" + margin + "\"\r\n"
           "                                      Visibility=\"{Binding FH2Mod_PanelShown, Converter={StaticResource " + shown + "}}\">\r\n"
           "                        <Border Margin=\"20,14,20,14\">\r\n"
           "                        <Grid>\r\n"
           "                            <Grid.ColumnDefinitions>\r\n"
           "                                <ColumnDefinition Width=\"Auto\" />\r\n"
           "                                <ColumnDefinition />\r\n"
           "                            </Grid.ColumnDefinitions>\r\n" +
           icon(FH2_HINT_ATTENTION, "HUD/InfoIconSmall.png") + icon(FH2_HINT_FAULT, "Icons/Alert_Icon.png") +
           text(FH2_HINT_INFO, "{StaticResource A_Color_SubHeader_Text}") + text(FH2_HINT_ATTENTION, "#FFFE5723") + text(FH2_HINT_FAULT, "#FFE0141E") +
           "                        </Grid>\r\n"
           "                        </Border>\r\n"
           "                    </local:FZMaterial>";
}

// `a_help`: what the help bar says A does on a page that tracks the selection (Select), or
// null for nothing. `tag`: the grey part of the title before `title` ("" for none; XAML, so a
// {local:Loc ...} or plain text). `panel`: what shows under the list (PanelKind). On a page with
// views, each view's rows get x:Names (see item_name), so a row can be focused again.
bool write_list_page(const char* file, const std::string& title, const std::vector<PageItem>& items, bool track_selection = false,
                     const std::vector<std::vector<PageItem>>* views = nullptr, const std::vector<ViewTitle>* view_titles = nullptr,
                     const char* a_help = "{local:Loc HelpButtons.IDS_Select}", const std::string& tag = "", int panel = kPanelNone,
                     const std::vector<ViewHelp>* view_help = nullptr, const ViewHelp* own_help = nullptr) {
    std::string page = read_ui("Scenes/Anthem/A_HUDOptions.xaml");
    if (page.empty()) return false;
    std::string res = "\r\n            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowNotice\" UseHidden=\"true\" InvertResult=\"false\" />\r\n"
                      "            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowView\" UseHidden=\"false\" InvertResult=\"false\" />\r\n"
                      "            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowList\" UseHidden=\"true\" InvertResult=\"false\" />\r\n"
                      "            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_HideView\" UseHidden=\"false\" InvertResult=\"true\" />\r\n"
                      "            <Color x:Key=\"FH2Mod_Color_Red\">#FFE0141E</Color>\r\n"        // the MODS tab's badge red
                      "            <Color x:Key=\"FH2Mod_Color_Yellow\">#FFFFC400</Color>\r\n" +   // and its yellow (updates)
                      info_row_resources();

    std::string rows = list_rows(items, "FH2Mod_Item_", "FirstItem", res);
    std::string views_style;   // the view switch's animation (see below), on pages with views
    std::vector<std::string> view_rows;
    if (views)
        for (std::size_t k = 0; k < views->size(); ++k)
            view_rows.push_back(list_rows((*views)[k], "FH2Mod_V" + std::to_string(k + 1) + "_", "FH2Mod_First_" + std::to_string(k + 1), res,
                                          "FH2Mod_R" + std::to_string(k + 1) + "_"));
    res += "        ";
    const std::string title_attr = "Content=\"{local:Loc ScreenTitles.IDS_HUDOptions}\"";
    const std::string help_y     = "local:ControllerButtonPanel.YDescription=";
    const std::string desc_panel = "Style=\"{StaticResource A_Style_DescriptionPanel}\"";
    if (page.find(title_attr) == std::string::npos || page.find(help_y) == std::string::npos ||
        page.find(desc_panel) == std::string::npos ||
        !replace_between(page, "<ResourceDictionary>", "</ResourceDictionary>", res, false) ||
        !replace_between(page, "<local:CommonListBox", "</local:CommonListBox>", rows, true)) {
        slog("A_HUDOptions.xaml doesn't look as expected; no " + title + " page");
        return false;
    }
    if (track_selection) {
        const std::string list = "x:Name=\"OptionsList\"";
        const std::size_t l    = page.find(list);
        if (l == std::string::npos) {
            slog("A_HUDOptions.xaml doesn't look as expected; no " + title + " page");
            return false;
        }
        page.insert(l + list.size(), " SelectedIndex=\"{Binding FH2Mod_Selected, Mode=TwoWay}\"");
    }
    if (views && !views->empty()) {
        // Every view is a list of its own, hidden unless FH2Mod_View_<k> says it's showing; view 0
        // is the page's own OptionsList. Hidden rather than collapsed: a list that has never
        // been laid out can't take the focus the moment it shows. Each list has its own copy of
        // the description panel, collapsed (they sit side by side). The panel's DataContext is
        // the list's selected row, so its Visibility reads the view through the list's.
        const std::size_t l0 = page.find("<local:CommonListBox"), l1 = page.find("</local:CommonListBox>", l0);
        const std::size_t tag_end = page.find('>', l0);
        const std::size_t dp = page.find(desc_panel);
        const std::size_t d0 = dp == std::string::npos ? dp : page.rfind("<ContentControl", dp), d1 = page.find("/>", dp);
        if (l1 == std::string::npos || d0 == std::string::npos || d1 == std::string::npos || d0 < l1) {
            slog("A_HUDOptions.xaml doesn't look as expected; no " + title + " page");
            return false;
        }
        auto list_vis = [](std::size_t k) {
            return " Visibility=\"{Binding FH2Mod_View_" + std::to_string(k) + ", Converter={StaticResource FH2Mod_ShowList}}\"";
        };
        // (moved onto the Grid around the panel by the footer code below, where the DataContext
        // is the page's: a path through ElementName didn't follow the change)
        auto panel_vis = [](std::size_t k) {
            return " Visibility=\"{Binding FH2Mod_View_" + std::to_string(k) + ", Converter={StaticResource FH2Mod_ShowList}}\"";
        };
        std::string panel = page.substr(d0, d1 + 2 - d0);
        std::string panels;
        for (std::size_t k = 1; k <= view_rows.size(); ++k) {
            std::string pk = panel;
            const std::size_t e = pk.find("ElementName=OptionsList");
            if (e == std::string::npos) return false;
            pk.replace(e, std::string("ElementName=OptionsList").size(), "ElementName=FH2Mod_List_" + std::to_string(k));
            pk.insert(std::string("<ContentControl").size(), panel_vis(k));
            panels += "\r\n                        " + pk;
        }
        // the panels on top of each other in a Grid (hidden, not collapsed, like the lists: an old
        // one stays a moment while the switch animates)
        page.insert(d1 + 2, panels + "\r\n                        </Grid>");
        page.insert(d0 + std::string("<ContentControl").size(), panel_vis(0));
        page.insert(d0, "<Grid>\r\n                        ");
        // the lists (all before the panels, so their positions above stand)
        std::string open_tag = page.substr(l0, tag_end + 1 - l0);
        std::string lists;
        for (std::size_t k = 1; k <= view_rows.size(); ++k) {
            std::string t = open_tag;
            const std::size_t nm = t.find("x:Name=\"OptionsList\"");
            if (nm == std::string::npos) return false;
            t.replace(nm, std::string("x:Name=\"OptionsList\"").size(), "x:Name=\"FH2Mod_List_" + std::to_string(k) + "\"");
            if (const std::size_t si = t.find(" SelectedIndex=\"{Binding FH2Mod_Selected, Mode=TwoWay}\""); si != std::string::npos)
                t.erase(si, std::string(" SelectedIndex=\"{Binding FH2Mod_Selected, Mode=TwoWay}\"").size());
            t.insert(t.find("x:Name=\"FH2Mod_List_"), "SelectedIndex=\"{Binding FH2Mod_ViewSelected, Mode=TwoWay}\" ");
            lists += "\r\n<Grid" + list_vis(k) + " Style=\"{StaticResource FH2Mod_Style_ListView_" + std::to_string(k) + "}\">\r\n" + t +
                     view_rows[k - 1] + "</local:CommonListBox>\r\n</Grid>";
        }
        // The view switch plays as the game's page transitions do (Global_Transitions.xaml): the
        // old content goes (0.2 s: fades, and moves outwards, quadratic ease in), then the new
        // comes (0.3 s: fades in, from behind, quartic ease out), the description panel 0.15 s
        // after the rest. Opacity and ZPanel.ZOffset on the page's own StackPanels are its
        // transitions' to drive (animated there, nothing showed), so a Grid of ours around the
        // page's content (title, list and panel) fades, and zooms with a ScaleTransform in place
        // of the depth: going into a mod, out to 1.1 then in from 0.9; coming back, out to 0.9
        // then in from 1.1. Each list and panel sits in a Grid of its own whose Visibility the
        // switch holds for those first 0.2 s (the old one stays, the new one waits unseen), so
        // the new list has the focus from the start.
        auto frames = [](const std::string& prop, const std::string& keys) {
            return "                                        <DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"" + prop + "\">\r\n" +
                   keys + "                                        </DoubleAnimationUsingKeyFrames>\r\n";
        };
        auto lin = [](const char* t, const std::string& v) {
            return "                                            <LinearDoubleKeyFrame KeyTime=\"" + std::string(t) + "\" Value=\"" + v + "\" />\r\n";
        };
        auto disc = [](const char* t, const std::string& v) {
            return "                                            <DiscreteDoubleKeyFrame KeyTime=\"" + std::string(t) + "\" Value=\"" + v + "\" />\r\n";
        };
        auto ease = [](const char* t, const std::string& v, const char* fn, const char* mode) {
            return "                                            <EasingDoubleKeyFrame KeyTime=\"" + std::string(t) + "\" Value=\"" + v + "\">\r\n"
                   "                                                <EasingDoubleKeyFrame.EasingFunction>\r\n"
                   "                                                    <" + fn + " EasingMode=\"" + mode + "\"/>\r\n"
                   "                                                </EasingDoubleKeyFrame.EasingFunction>\r\n"
                   "                                            </EasingDoubleKeyFrame>\r\n";
        };
        auto board = [](const std::string& animations) {
            return "                                <BeginStoryboard>\r\n"
                   "                                    <Storyboard>\r\n" + animations +
                   "                                    </Storyboard>\r\n"
                   "                                </BeginStoryboard>\r\n";
        };
        auto vis_frames = [](bool stays) {   // Visible now; Hidden after 0.2 s when it's the old one
            return std::string("                                        <ObjectAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.Visibility)\">\r\n"
                               "                                            <DiscreteObjectKeyFrame KeyTime=\"0\" Value=\"{x:Static Visibility.Visible}\"/>\r\n") +
                   (stays ? "" : "                                            <DiscreteObjectKeyFrame KeyTime=\"0:0:0.2\" Value=\"{x:Static Visibility.Hidden}\"/>\r\n") +
                   "                                        </ObjectAnimationUsingKeyFrames>\r\n";
        };
        // the whole content: out, then in
        auto content = [&](const char* out_to, const char* in_from) {
            const std::string sx = "(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleX)";
            const std::string sy = "(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleY)";
            const std::string scale = lin("0", "1") + ease("0:0:0.2", out_to, "QuadraticEase", "EaseIn") + disc("0:0:0.2", in_from) +
                                      ease("0:0:0.5", "1", "QuarticEase", "EaseOut");
            return "                            <DataTrigger.EnterActions>\r\n" +
                   board(frames("Opacity", lin("0", "1") + lin("0:0:0.2", "0") + lin("0:0:0.5", "1")) + frames(sx, scale) + frames(sy, scale)) +
                   "                            </DataTrigger.EnterActions>\r\n";
        };
        // forwards into view k (FH2Mod_ViewIn_<k>), or back to it (FH2Mod_ViewBack_<k>: the list,
        // or a mod's settings from one of its pages)
        std::string style = "\r\n            <Style x:Key=\"FH2Mod_Style_Views\" TargetType=\"{x:Type FrameworkElement}\">\r\n"
                            "                <Style.Triggers>\r\n";
        for (std::size_t k = 1; k <= view_rows.size(); ++k)
            style += "                    <DataTrigger Binding=\"{Binding FH2Mod_ViewIn_" + std::to_string(k) + "}\" Value=\"True\">\r\n" +
                     content("1.1", "0.9") + "                    </DataTrigger>\r\n";
        for (std::size_t k = 0; k <= view_rows.size(); ++k)
            if (k == 0 || !view_titles || k > view_titles->size() || (*view_titles)[k - 1].settings)
                style += "                    <DataTrigger Binding=\"{Binding FH2Mod_ViewBack_" + std::to_string(k) + "}\" Value=\"True\">\r\n" +
                         content("0.9", "1.1") + "                    </DataTrigger>\r\n";
        style += "                </Style.Triggers>\r\n"
                 "            </Style>\r\n";
        // each list and panel: shown when its view comes (unseen until the old one has gone, the
        // panel 0.15 s later still), kept 0.2 s when it goes
        for (std::size_t k = 0; k <= view_rows.size(); ++k)
            for (const char* kind : {"ListView_", "PanelView_"}) {
                const bool is_panel = kind[0] == 'P';
                const std::string in = is_panel ? lin("0", "0") + lin("0:0:0.35", "0") + lin("0:0:0.65", "1") : disc("0", "0") + disc("0:0:0.2", "1");
                style += "            <Style x:Key=\"FH2Mod_Style_" + std::string(kind) + std::to_string(k) + "\" TargetType=\"{x:Type FrameworkElement}\">\r\n"
                         "                <Style.Triggers>\r\n"
                         "                    <DataTrigger Binding=\"{Binding FH2Mod_View_" + std::to_string(k) + "}\" Value=\"True\">\r\n"
                         "                            <DataTrigger.EnterActions>\r\n" +
                         board(vis_frames(true) + frames("Opacity", in)) +
                         "                            </DataTrigger.EnterActions>\r\n"
                         "                            <DataTrigger.ExitActions>\r\n" +
                         board(vis_frames(false)) +
                         "                            </DataTrigger.ExitActions>\r\n"
                         "                    </DataTrigger>\r\n"
                         "                </Style.Triggers>\r\n"
                         "            </Style>\r\n";
            }
        style += "        ";
        views_style = style;
        const std::size_t close = l1 + std::string("</local:CommonListBox>").size();
        page.insert(close, "\r\n</Grid>" + lists + "\r\n</Grid>");
        page.insert(l0, "<Grid>\r\n<Grid" + list_vis(0) + " Style=\"{StaticResource FH2Mod_Style_ListView_0}\">\r\n");

    }
    // Under each description panel's text, at the bottom of the panel: the selected row's
    // version (red when there's a newer one) and GitHub page, from its data item (see
    // list_rows). Overlaid in a Grid with the game's own panel, not a template of ours: the
    // game's XAML reader crashed on our copy of the panel's template.
    for (std::size_t at = page.find(desc_panel); at != std::string::npos;) {
        const std::size_t e0 = page.rfind("<ContentControl", at), e1 = page.find("/>", at);
        if (e0 == std::string::npos || e1 == std::string::npos) break;
        std::string panel = page.substr(e0, e1 + 2 - e0);
        const std::size_t en = panel.find("ElementName="), en_end = en == std::string::npos ? en : panel.find_first_of("}, ", en);
        if (en_end == std::string::npos) break;
        const std::string list = panel.substr(en + 12, en_end - en - 12);
        std::string vis;   // a view's panel: its Visibility goes on the Grid
        if (const std::size_t v = panel.find(" Visibility=\""); v != std::string::npos) {
            std::size_t v_end = v + 13, depth = 0;
            for (; v_end < panel.size(); ++v_end) {
                if (panel[v_end] == '{') ++depth;
                else if (panel[v_end] == '}') --depth;
                else if (panel[v_end] == '"' && depth == 0) break;
            }
            vis = panel.substr(v, v_end + 1 - v);
            panel.erase(v, v_end + 1 - v);
            // and its switch animation (see the views above)
            if (const std::size_t n0 = vis.find("FH2Mod_View_"); n0 != std::string::npos) {
                const std::size_t n1 = vis.find_first_not_of("0123456789", n0 + 12);
                vis += " Style=\"{StaticResource FH2Mod_Style_PanelView_" + vis.substr(n0 + 12, n1 - n0 - 12) + "}\"";
            }
        }
        const std::string text = "Style=\"{StaticResource A_Style_FZTextBlock_DescriptionBlockBody}\" TextWrapping=\"Wrap\"";
        // Fixed places, so they're where they always are: the version (the normal and the red
        // copy on top of each other; one is empty), then the GitHub page, at the bottom.
        // Inside the panel's own width, so the footer wraps inside it.
        const std::string grid =
            "<Grid Width=\"380\" HorizontalAlignment=\"Left\"" + vis + ">\r\n                        " + panel +
            "\r\n                        <StackPanel VerticalAlignment=\"Bottom\" Margin=\"30,0,30,24\" DataContext=\"{Binding SelectedItem, ElementName=" + list + "}\">\r\n"
            // the row's error, in red, set apart from the description above it
            "                            <local:FZTextBlock Text=\"{Binding Tag.Icon}\" Color=\"{StaticResource FH2Mod_Color_Red}\" Margin=\"0,0,0,24\" " + text + " />\r\n"
            "                            <Grid>\r\n"
            "                                <local:FZTextBlock Text=\"{Binding Content.Heading}\" " + text + " />\r\n"
            "                                <local:FZTextBlock Text=\"{Binding Content.Description}\" Color=\"{StaticResource FH2Mod_Color_Yellow}\" " +
            text + " />\r\n"
            "                            </Grid>\r\n"
            "                            <local:FZTextBlock Text=\"{Binding Content.Icon}\" Margin=\"0,8,0,0\" " + text + " />\r\n"
            "                        </StackPanel>\r\n                    </Grid>";
        page.replace(e0, e1 + 2 - e0, grid);
        at = page.find(desc_panel, e0 + grid.size());
    }
    if (track_selection && (!views || views->empty())) {
        // a list A acts on (Updates): Select / Back; or only Back (a mod's page without on_select)
        const std::string help = "<ContentControl x:Name=\"DefaultHelpPanel\"";
        const std::size_t h0 = page.find(help), h1 = h0 == std::string::npos ? h0 : page.find("/>", h0);
        if (h1 != std::string::npos) {
            std::string bar = page.substr(h0, h1 + 2 - h0);
            if (own_help && own_help->own) {   // a mod's page with a help bar of its own (set_page_help)
                bar = page_help_bar(bar, own_help->help, own_help->select);
            } else {
                if (a_help) swap_attr(bar, "local:ControllerButtonPanel.ADescription", a_help);
                else erase_attr(bar, "local:ControllerButtonPanel.ADescription");
                swap_attr(bar, "local:ControllerButtonPanel.BDescription", "{local:Loc HelpButtons.IDS_Back}");
            }
            page.replace(h0, h1 + 2 - h0, bar);
        }
    }
    if (views && !views->empty()) {
        // The help bar: A is Select on the list, Accept (done, back to the list) in a mod's
        // settings; B is Back in both. Two copies, one collapsed.
        const std::string help = "<ContentControl x:Name=\"DefaultHelpPanel\"";
        const std::size_t h0 = page.find(help), h1 = h0 == std::string::npos ? h0 : page.find("/>", h0);
        if (h1 == std::string::npos) {
            slog("A_HUDOptions.xaml doesn't look as expected; no " + title + " page");
            return false;
        }
        std::string bar = page.substr(h0, h1 + 2 - h0);
        auto set_attr = [](std::string& el, const std::string& name, const std::string& value) {
            const std::size_t a = el.find(name + "=\"");
            if (a == std::string::npos) {
                el.insert(el.find(' '), " " + name + "=\"" + value + "\"");
                return;
            }
            const std::size_t v = a + name.size() + 2, e = el.find('"', v);
            el.replace(v, e - v, value);
        };
        // one per kind of view (FH2Mod_Bar_<kind>): the list (Select), a mod's page (only Back: A
        // doesn't leave it, and changes are saved as they're made; a link row says A opens it), a
        // page of the mod's own (only Back, or Select when its rows do something)
        const char* const a_desc[kViewKinds] = {"{local:Loc HelpButtons.IDS_Select}", nullptr, nullptr, "{local:Loc HelpButtons.IDS_Select}"};
        std::string bars = "<Grid>";
        for (int b = 0; b < kViewKinds; ++b) {
            std::string one = bar;
            if (b) set_attr(one, "x:Name", "FH2Mod_ViewHelpPanel_" + std::to_string(b));
            if (a_desc[b]) set_attr(one, "local:ControllerButtonPanel.ADescription", a_desc[b]);
            else erase_attr(one, "local:ControllerButtonPanel.ADescription");
            set_attr(one, "local:ControllerButtonPanel.BDescription", "{local:Loc HelpButtons.IDS_Back}");
            set_attr(one, "Visibility", "{Binding FH2Mod_Bar_" + std::to_string(b) + ", Converter={StaticResource FH2Mod_ShowView}}");
            bars += "\r\n" + one;
        }
        // a page view with a help bar of its own (set_page_help): shown instead of its kind's
        for (std::size_t k = 1; view_help && k <= view_help->size() && k <= views->size(); ++k) {
            const ViewHelp& vh = (*view_help)[k - 1];
            if (!vh.own) continue;
            std::string one = page_help_bar(bar, vh.help, vh.select);
            set_attr(one, "x:Name", "FH2Mod_ViewHelpPanel_P" + std::to_string(k));
            set_attr(one, "Visibility", "{Binding FH2Mod_HelpBar_" + std::to_string(k) + ", Converter={StaticResource FH2Mod_ShowView}}");
            bars += "\r\n" + one;
        }
        page.replace(h0, h1 + 2 - h0, bars + "\r\n</Grid>");
    }
    // The title: ours, after `tag` in the game's grey ("MODS / YOUR MODS"), or alone without one (a
    // title block with no Tag shows just its Content). A view (a mod's page) has its own,
    // "MODS / <MOD>", shown with it; a page of a mod's own has its title alone.
    const std::size_t t = page.find(title_attr);
    page.replace(t, title_attr.size(), "Content=\"" + xml_escape(title) + "\"");
    {
        const std::size_t e0 = page.rfind("<ContentControl", t), e1 = page.find("/>", t);
        std::string block    = page.substr(e0, e1 + 2 - e0);
        if (const std::size_t tg = block.find(" Tag=\""); tg != std::string::npos) block.erase(tg, block.find('"', tg + 6) + 1 - tg);
        const std::string base = block;   // the views' titles start from this one (no Tag)
        if (!tag.empty()) swap_attr(block, "Tag", tag);
        std::string blocks = block;
        if (views && view_titles && !views->empty()) {
            // On top of each other in a Grid, each in one of its own that the view switch holds
            // as it does the lists (FH2Mod_Style_ListView_<k>), so the title changes with the
            // content rather than before it.
            auto held = [](std::size_t k, const std::string& el) {
                return "<Grid HorizontalAlignment=\"Left\" Visibility=\"{Binding FH2Mod_View_" + std::to_string(k) +
                       ", Converter={StaticResource FH2Mod_ShowList}}\" Style=\"{StaticResource FH2Mod_Style_ListView_" + std::to_string(k) +
                       "}\">" + el + "</Grid>";
            };
            blocks = "<Grid>\r\n                        " + held(0, block);
            for (std::size_t k = 1; k <= views->size() && k <= view_titles->size(); ++k) {
                std::string vt = base;
                const std::string name = upper_game((*view_titles)[k - 1].title);
                swap_attr(vt, "Content", xml_escape(name));
                if ((*view_titles)[k - 1].settings) swap_attr(vt, "Tag", kModsTag);   // "MODS / WRECKINGBALL"
                blocks += "\r\n                        " + held(k, vt);
            }
            blocks += "\r\n                        </Grid>";
        }
        page.replace(e0, e1 + 2 - e0, blocks);
    }
    // Extra Options adds its rows to pages declared x:Class="ForzaUI.A_HudOptions" as the game
    // loads them; a space before the '=' is the same XML but keeps them on the HUD page.
    const std::string cls = "x:Class=\"ForzaUI.A_HudOptions\"";
    if (const std::size_t c = page.find(cls); c != std::string::npos) page.replace(c, cls.size(), "x:Class =\"ForzaUI.A_HudOptions\"");
    // no Y (reset to defaults) on this page
    for (std::size_t y = page.find(help_y); y != std::string::npos; y = page.find(help_y, y)) {
        const std::size_t y_end = page.find('"', page.find('"', y) + 1);
        page.erase(y, y_end + 1 - y);
    }
    // The panel under the list and the description (PanelKind): YOUR MODS' hint and each mod's
    // legend for the rows marked *, or the rows waiting for a restart; hidden (not collapsed) while
    // there's nothing to say, so the page doesn't move when it shows
    // (the last thing in BasicOptions, the StackPanel of title, list and panel: its end is the
    // last </StackPanel> before the first </ContentControl> after it, our footers' StackPanels
    // being inside)
    const std::size_t basic    = page.find("<StackPanel Name=\"BasicOptions\"");
    const std::size_t tilt_end = basic == std::string::npos ? basic : page.find("</ContentControl>", basic);
    const std::size_t row_end  = tilt_end == std::string::npos ? tilt_end : page.rfind("</StackPanel>", tilt_end);
    if (row_end == std::string::npos || row_end < basic) {
        slog("A_HUDOptions.xaml doesn't look as expected; no " + title + " page");
        return false;
    }
    if (panel == kPanelBound)
        page.insert(row_end, hint_panel("FH2Mod_ShowNotice", "FH2Mod_ShowView", "0,6,0,0"));
    else if (panel != kPanelNone)
        page.insert(row_end,
                    "\r\n                    <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_Panel_Background}\" Margin=\"0,6,0,0\"" +
                    std::string(panel == kPanelBound ? "\r\n                                      Visibility=\"{Binding FH2Mod_PanelShown, Converter={StaticResource FH2Mod_ShowNotice}}\"" : "") +
                    ">\r\n"
                    "                        <Border Margin=\"20,14,20,14\">\r\n"
                    "                            <local:FZTextBlock Text=\"" +
                    (panel == kPanelBound ? std::string("{Binding FH2Mod_PanelText}") : xml_escape(tr("* Takes effect the next time you start the game."))) +
                    "\"\r\n"
                    "                                               Color=\"{StaticResource A_Color_SubHeader_Text}\"\r\n"
                    "                                               Style=\"{StaticResource A_Style_FZTextBlock_Body_Tight}\" TextWrapping=\"Wrap\" />\r\n"
                    "                        </Border>\r\n"
                    "                    </local:FZMaterial>");
    if (!views_style.empty()) {
        const std::size_t rd = page.find("</ResourceDictionary>"), b0 = page.find("<StackPanel Name=\"BasicOptions\"");
        const std::size_t tilt_end = b0 == std::string::npos ? b0 : page.find("</ContentControl>", b0);
        const std::size_t b1 = tilt_end == std::string::npos ? tilt_end : page.rfind("</StackPanel>", tilt_end);
        if (rd == std::string::npos || b1 == std::string::npos || b1 < b0 || rd > b0) {
            slog("A_HUDOptions.xaml doesn't look as expected; Installed mods won't animate");
        } else {
            page.insert(b1 + std::string("</StackPanel>").size(), "\r\n                </Grid>");
            page.insert(b0, "<Grid Style=\"{StaticResource FH2Mod_Style_Views}\" RenderTransformOrigin=\"0.5,0.5\">\r\n"
                            "                <Grid.RenderTransform>\r\n"
                            "                    <TransformGroup>\r\n"
                            "                        <ScaleTransform />\r\n"
                            "                    </TransformGroup>\r\n"
                            "                </Grid.RenderTransform>\r\n                ");
            page.insert(rd, views_style);
        }
    }
    return set_file(&g_owner_mod, (std::string("UI/Scenes/Anthem/") + file).c_str(), page.data(), page.size()) != 0;
}

// A page of a mod's own design: A_HUDOptions.xaml's frame (background, player bar, help bar,
// the screen's padding and tilt) around the mod's XAML. Its content takes the place of the
// BasicOptions panel (title, list and description); its resources go into the page's, after
// the platform's converters; its help, if any, takes the place of DefaultHelpPanel.
bool write_custom_page(const char* file, const std::string& resources, const std::string& content, const std::string& help,
                       const ViewHelp* own_help = nullptr, bool hints = false) {
    std::string page = read_ui("Scenes/Anthem/A_HUDOptions.xaml");
    if (page.empty()) return false;
    const std::string res = "\r\n            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowIf\" UseHidden=\"false\" InvertResult=\"false\" />\r\n"
                            "            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowIfHidden\" UseHidden=\"true\" InvertResult=\"false\" />\r\n"
                            "            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_HideIf\" UseHidden=\"false\" InvertResult=\"true\" />\r\n" +
                            resources + "\r\n        ";
    const std::size_t basic    = page.find("<StackPanel Name=\"BasicOptions\"");
    const std::size_t tilt_end = basic == std::string::npos ? basic : page.find("</ContentControl>", basic);
    const std::size_t end      = tilt_end == std::string::npos ? tilt_end : page.rfind("</StackPanel>", tilt_end);
    const std::string help_el  = "<ContentControl x:Name=\"DefaultHelpPanel\"";
    const std::size_t h0 = page.find(help_el), h1 = h0 == std::string::npos ? h0 : page.find("/>", h0);
    if (end == std::string::npos || end < basic || h1 == std::string::npos || h1 > basic ||
        !replace_between(page, "<ResourceDictionary>", "</ResourceDictionary>", res, false)) {
        slog("A_HUDOptions.xaml doesn't look as expected; no page for " + std::string(file));
        return false;
    }
    // (positions again: the resources moved them)
    const std::size_t b0 = page.find("<StackPanel Name=\"BasicOptions\"");
    const std::size_t b1 = page.rfind("</StackPanel>", page.find("</ContentControl>", b0)) + std::string("</StackPanel>").size();
    // with hints (set_page_hints, API 10), the hint line under the content: a Grid, its content in the "*" row, so the
    // content keeps the finite height it has without hints (a StackPanel would measure it with infinite height, and
    // star-sized rows and scrolling lists in it would stop working)
    page.replace(b0, b1 - b0, hints ? "<Grid>\r\n<Grid.RowDefinitions>\r\n<RowDefinition Height=\"*\" />\r\n<RowDefinition Height=\"Auto\" />\r\n"
                                      "</Grid.RowDefinitions>\r\n<Grid Grid.Row=\"0\">\r\n" + content + "\r\n</Grid>\r\n<Grid Grid.Row=\"1\">" +
                                      hint_panel("FH2Mod_ShowIfHidden", "FH2Mod_ShowIf", "0,12,0,0") + "\r\n</Grid>\r\n</Grid>"
                                    : content);
    const std::size_t p0 = page.find(help_el), p1 = page.find("/>", p0) + 2;
    if (own_help && own_help->own) {   // set_page_help's prompts, in place of the mod's help fragment
        page.replace(p0, p1 - p0, page_help_bar(page.substr(p0, p1 - p0), own_help->help, false));
    } else if (help.empty()) {
        std::string bar = page.substr(p0, p1 - p0);
        erase_attr(bar, "local:ControllerButtonPanel.ADescription");
        erase_attr(bar, "local:ControllerButtonPanel.YDescription");
        swap_attr(bar, "local:ControllerButtonPanel.BDescription", "{local:Loc HelpButtons.IDS_Back}");
        page.replace(p0, p1 - p0, bar);
    } else {
        page.replace(p0, p1 - p0, "<Grid>\r\n" + help + "\r\n</Grid>");
    }
    // Extra Options leaves pages alone that aren't declared exactly x:Class="ForzaUI.A_HudOptions"
    const std::string cls = "x:Class=\"ForzaUI.A_HudOptions\"";
    if (const std::size_t c = page.find(cls); c != std::string::npos) page.replace(c, cls.size(), "x:Class =\"ForzaUI.A_HudOptions\"");
    return set_file(&g_owner_mod, (std::string("UI/Scenes/Anthem/") + file).c_str(), page.data(), page.size()) != 0;
}

void hub_rows_text();   // with the MODS tab's rows, below
void follow_language();

bool write_mods_page();   // the list that stands in for the MODS tab, with the tab's pages below
std::string xaml_block(const std::string& text, const std::string& key, const std::string& open, const std::string& close);   // below
bool swap_all(std::string& s, const std::string& from, const std::string& to);

// Pause > Settings: a sixth column with a tall MODS tile, a door to YOUR MODS: "MODS", and under it "Same as
// Pause > MODS" (FH2Mod_HubTile_<kHubLineSettings>, in the game's language as the page shows it). The tile is the
// game's own A_ControlTemplate_VerticalIconTile (Shared_TiledMenus.xaml) with that line under the title; if the
// game's template doesn't look as expected, it's the game's, without the line.
bool write_settings_page() {
    std::string page = read_ui("Scenes/Anthem/A_PauseOptions.xaml");
    if (page.empty()) return false;
    const std::string item_anchor = "x:Key=\"A_MenuDataItem_TeamCredits\"";
    const std::string canvas_end  = "</local:AutoSizeCanvas>";
    const std::size_t i = page.find(item_anchor), c = page.find(canvas_end);
    const std::size_t i_end = i == std::string::npos ? i : page.find('\n', i);
    if (i == std::string::npos || c == std::string::npos || i_end == std::string::npos || c < i_end) {
        slog("A_PauseOptions.xaml doesn't look as expected; no Mods tile");
        return false;
    }
    std::string tile = xaml_block(read_ui("Resources/Anthem/Shared_TiledMenus.xaml"), "A_ControlTemplate_VerticalIconTile", "<ControlTemplate",
                                  "</ControlTemplate>");
    const std::string heading = "<local:FZTextBlock Name=\"Heading\"";
    const bool keyed = swap_all(tile, "x:Key=\"A_ControlTemplate_VerticalIconTile\"", "x:Key=\"FH2Mod_Template_ModsTile\"");
    const std::size_t h0 = tile.find(heading), h1 = h0 == std::string::npos ? h0 : tile.find("/>", h0);   // (after the key's swap)
    const bool line = keyed && h1 != std::string::npos && tile.find(heading, h1) == std::string::npos;
    if (line) {
        const std::string title = tile.substr(h0, h1 + 2 - h0);
        tile.replace(h0, h1 + 2 - h0, "<StackPanel>" + title + "<local:FZTextBlock Text=\"{Binding FH2Mod_HubTile_" + std::to_string(kHubLineSettings) +
                                          ", RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}}\" "
                                          "Style=\"{StaticResource A_Style_FZTextBlock_TileText}\" Margin=\"0,12,0,0\" /></StackPanel>");
    } else {
        slog("the game's tall tile template doesn't look as expected; the Settings page's MODS tile has no second line");
    }
    page.insert(c,
                "\r\n                                <Button x:Name=\"PART_Button_Mods\"\r\n"
                "                                        local:TileMenuNavigationManager.TileState=\"Enabled\"\r\n"
                "                                        DataContext=\"{StaticResource FH2Mod_MenuDataItem_Mods}\"\r\n"
                "                                        local:TileMenuNavigationManager.GridX=\"5\"\r\n"
                "                                        local:TileMenuNavigationManager.GridY=\"0\"\r\n"
                "                                        Style=\"{DynamicResource A_Style_Button_Tile_1x2}\"\r\n"
                "                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_X_5}\"\r\n"
                "                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_0}\"\r\n"
                "                                        Template=\"{StaticResource " + std::string(line ? "FH2Mod_Template_ModsTile" : "A_ControlTemplate_VerticalIconTile") +
                "}\"\r\n"
                "                                        local:UIPageTransitionManager.TransitionIndex=\"6\"\r\n"
                "                                        local:BindingProxy.C=\"DisableMotionBlur\"/>\r\n"
                "                            ");
    page.insert(i_end + 1,
                "            <local:TileMenuDataItem x:Key=\"FH2Mod_MenuDataItem_Mods\" Heading=\"MODS\" "
                "Icon=\"/ForzaUI;component/Textures/Anthem/Icons/TiledMenus/Tuning1x1.png\" />\r\n" +
                    (line ? "            " + tile + "\r\n" : std::string()));
    return set_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseOptions.xaml", page.data(), page.size()) != 0;
}

// ---- the MODS tab: its pages -----------------------------------------------------------------
//
// Pause > MARKETPLACE has been dead since the store closed. The platform turns it into a MODS
// tab: A_PauseMenu.xaml's Marketplace tiles become the tab's six tiles, and each opens a page
// laid out like the Mods page (A_ModPage_01..05.xaml, or A_ModOptions.xaml for All settings).
// Installed mods has a switch per mod in fh2-mods, with that mod's own rows under it.
std::vector<ModEntry> g_entries;     // fh2-mods, in load order (set once, before the hooks)
std::vector<int> g_entry_row;        // each entry's switch row in g_rows
std::vector<int> g_entry_online_row; // each entry's "Load when playing online" row in g_rows, or -1
// The UI templates' player controls (ui_player.cpp, platform 2.9.0): each entry's "Pop-ups" row in g_rows (or -1),
// the platform's own "Mod pop-ups" and "Tips" rows, and the platform's line and page on YOUR MODS as last written
// (its row in the list, and its view).
std::vector<int> g_entry_popups_row;
int g_platform_rows[2] = {-1, -1};
std::atomic<int> g_platform_list_row{-1};
std::atomic<int> g_platform_view{0};
std::mutex g_page_mu;                // one page written at a time

// The MODS tab's pages, A_ModPage_<n>.xaml: YOUR MODS (every mod's page is a view of it), UPDATES, ABOUT and
// HELP; GET MODS opens the browser. A mod's page on a pause-menu tile is 80 and up. (A_ModOptions.xaml is
// only the list that stands in for the tab when its hooks don't go in.)
enum HubPage { kHubInstalled = 1, kHubUpdates, kHubGetMods, kHubAbout, kHubHelp = 6, kHubTilePage = 80 };

// "A_ModPage_01" (12 letters, like "A_HudOptions")
std::wstring hub_name(int p) {
    wchar_t b[16];
    std::swprintf(b, 16, L"A_ModPage_%02d", p);
    return b;
}
std::string hub_file(int p) { return narrow(hub_name(p)) + ".xaml"; }

void hub_switch_changed(void* user, const char*, int v) {
    const auto i = reinterpret_cast<std::uintptr_t>(user);
    if (i < g_entries.size()) set_mod_enabled(g_entries[i].dll, v != 0);
}

void hub_online_changed(void* user, const char*, int v) {
    const auto i = reinterpret_cast<std::uintptr_t>(user);
    if (i < g_entries.size()) set_mod_online(g_entries[i].dll, v != 0);
}

void hub_popups_changed(void* user, const char*, int v) {
    const auto i = reinterpret_cast<std::uintptr_t>(user);
    if (i < g_entries.size() && g_entries[i].mod) mod_popups_changed(g_entries[i].mod, g_entries[i].dll, v);
}

void platform_row_changed(void* user, const char*, int v) {
    player_row_changed(user ? PlayerRow::tips : PlayerRow::mod_popups, v);
}

// One of the UI templates' rows: its text in the game's language now (ui_player.cpp). A choice row keeps its
// number of choices.
void player_text_into(Row& r, const PlayerText& t) {
    r.label       = t.label;
    r.description = t.description;
    if (!t.choices.empty() && t.choices.size() == r.choices.size()) r.choices = t.choices;
}

// The platform's "Mod pop-ups" (a choice) and "Tips" (On/Off) rows, and a "Pop-ups" row for each mod that declared
// pop-ups without a row of its own (under g_mu, after the Load switches).
void add_player_rows() {
    for (int k = 0; k < 2; ++k) {
        const PlayerRow which = k ? PlayerRow::tips : PlayerRow::mod_popups;
        const PlayerText t    = player_row_text(which);
        Row& r        = g_rows.emplace_back();
        r.mod         = &g_hub_owner;
        r.key         = k ? "PlatformTips" : "PlatformModPopups";
        r.label       = t.label;
        r.description = t.description;
        r.prop        = "FH2Mod_" + std::to_string(g_rows.size() - 1);
        r.value       = player_row_value(which);
        r.boot        = r.value;
        r.on_change   = &platform_row_changed;
        r.user        = reinterpret_cast<void*>(static_cast<std::uintptr_t>(k));
        if (!t.choices.empty()) {   // as settings_add makes a choice row: a slider over the choices' numbers
            r.choices = t.choices;
            r.slider  = true;
            r.fmin    = 0;
            r.fmax    = static_cast<float>(t.choices.size() - 1);
            r.fstep   = 1;
            r.fvalue  = static_cast<float>(r.value.load());
            r.fboot   = r.fvalue;
        }
        g_platform_rows[k] = static_cast<int>(g_rows.size() - 1);
    }
    g_entry_popups_row.assign(g_entries.size(), -1);
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const ModEntry& e = g_entries[i];
        if (!e.enabled || !e.mod || !mod_gets_popups_row(e.mod)) continue;
        const PlayerText t = mod_popups_text(e.name);
        Row& r        = g_rows.emplace_back();
        r.mod         = &g_hub_owner;
        r.key         = "ModPopups_" + std::to_string(i);
        r.label       = t.label;
        r.description = t.description;
        r.prop        = "FH2Mod_" + std::to_string(g_rows.size() - 1);
        r.value       = mod_popups_start(e.mod, e.dll);
        r.boot        = r.value;
        r.on_change   = &hub_popups_changed;
        r.user        = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i));
        g_entry_popups_row[i] = static_cast<int>(g_rows.size() - 1);
    }
}

} // namespace

// "fh2mod Platform ui set" (Developer=1): one of the rows above changed as the player would change it, so what's kept
// in fh2-mods\platform-popups.ini can be tried without the game's menus.
std::string settings_player_set(const std::string& which, bool on) {
    FH2SettingChangedFn fn = nullptr;
    void* user             = nullptr;
    std::string key, what;
    int v = 0;
    {
        std::scoped_lock lk{g_mu};
        int row = -1;
        if (_stricmp(which.c_str(), "ModPopups") == 0) {
            row  = g_platform_rows[0];
            v    = on ? 0 : 1;   // the choice's index: Same as Notifications, Off
            what = std::string("Mod pop-ups: ") + (on ? "Same as Notifications" : "Off");
        } else if (_stricmp(which.c_str(), "Tips") == 0) {
            row  = g_platform_rows[1];
            v    = on;
            what = std::string("Tips: ") + (on ? "On" : "Off");
        } else {
            for (std::size_t i = 0; i < g_entries.size(); ++i) {
                if (_stricmp(g_entries[i].name.c_str(), which.c_str()) != 0) continue;
                if (i >= g_entry_popups_row.size() || g_entry_popups_row[i] < 0)
                    return "error: " + g_entries[i].name + " has no Pop-ups row (it didn't declare pop-ups, or has a row of its own)\n";
                row  = g_entry_popups_row[i];
                v    = on;
                what = g_entries[i].name + "'s Pop-ups: " + (on ? "On" : "Off");
                break;
            }
            if (row < 0) return "error: \"" + which + "\" isn't ModPopups, Tips or an installed mod's name\n";
        }
        if (row < 0 || static_cast<std::size_t>(row) >= g_rows.size()) return "error: the platform's rows aren't there yet\n";
        Row& r = g_rows[static_cast<std::size_t>(row)];
        r.value = v;
        if (r.slider) r.fvalue = static_cast<float>(v);
        fn   = r.on_change;
        user = r.user;
        key  = r.key;
    }
    if (fn) call_on_change(fn, user, key.c_str(), v);   // the platform's own: ui_player.cpp keeps it
    return what + ", and kept in fh2-mods\\platform-popups.ini\n";
}

namespace {

// A mod's "Load when playing online" row's text, in the game's language now (English before it has one).
void online_row_text(Row& o, const ModEntry& e) {
    o.label       = tr("Load when playing online");
    o.description = e.offline_only
                        ? trf("Off: when you start the game from the FH2MP launcher, the game doesn't load {0}, so it can't change "
                              "online play. Its author made it for offline play, so it's off unless you turn it on. Takes effect the "
                              "next time you start the game from the launcher.", {e.name})
                        : trf("Off: when you start the game from the FH2MP launcher, the game doesn't load {0}, so it can't change "
                              "online play. Takes effect the next time you start the game from the launcher.", {e.name});
}

void add_hub_rows() {
    g_entries = mod_entries();
    ui_player_pages();   // the platform's own pages (Recent pop-ups, UI templates), before the views are made
    // "Load when playing online" (platform 2.8.0), only where FH2MP is installed, and not for FH2MP
    const bool fh2mp = fh2mp_installed();
    std::scoped_lock lk{g_mu};
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const ModEntry& e = g_entries[i];
        Row& r     = g_rows.emplace_back();
        r.mod      = &g_hub_owner;
        r.key      = "Mod_" + std::to_string(i);
        r.label    = e.name;
        r.prop     = "FH2Mod_" + std::to_string(g_rows.size() - 1);
        r.value    = e.enabled ? 1 : 0;
        r.boot     = r.value;
        r.restart  = true;
        r.on_change = &hub_switch_changed;
        r.user     = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i));
        g_entry_row.push_back(static_cast<int>(g_rows.size() - 1));
        if (!fh2mp || is_fh2mp(e)) {
            g_entry_online_row.push_back(-1);
            continue;
        }
        Row& o       = g_rows.emplace_back();
        o.mod        = &g_hub_owner;
        o.key        = "ModOnline_" + std::to_string(i);
        online_row_text(o, e);
        o.prop       = "FH2Mod_" + std::to_string(g_rows.size() - 1);
        o.value      = e.loads_online ? 1 : 0;
        o.boot       = o.value;
        o.on_change  = &hub_online_changed;   // no restart notice: it's about the next online start
        o.user       = reinterpret_cast<void*>(static_cast<std::uintptr_t>(i));
        g_entry_online_row.push_back(static_cast<int>(g_rows.size() - 1));
    }
    add_player_rows();
}

// The rows that wait for a restart, by name, for the panel under YOUR MODS: up to three, then how many more. A
// mod's Load switch is "Load <Mod>", and its Load when playing online "Load <Mod> online".
std::string pending_restart_names() {
    std::vector<std::string> names;
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t r = 0; r < g_rows.size(); ++r) {
            if (!row_changed(g_rows[r])) continue;
            std::string name = g_rows[r].label;
            for (std::size_t i = 0; i < g_entries.size(); ++i) {
                if (i < g_entry_row.size() && g_entry_row[i] == static_cast<int>(r)) name = trf("Load {0}", {g_entries[i].name});
                if (i < g_entry_online_row.size() && g_entry_online_row[i] == static_cast<int>(r))
                    name = trf("Load {0} online", {g_entries[i].name});
            }
            names.push_back(name);
        }
    }
    std::string list;
    for (std::size_t i = 0; i < names.size() && i < 3; ++i) list += (i ? ", " : "") + names[i];
    if (names.size() > 3) list = trf("{0} and {1} more", {list, std::to_string(names.size() - 3)});
    return list;
}

const char* const kHubTileNames[kHubTiles] = {"YOUR MODS", "UPDATES", "GET MODS", "ABOUT", "HELP"};
std::string mod_problem(const ModEntry& e);   // below

std::string hub_tile_text(int k) {
    if (k >= kHubLineName1 && k <= kHubLineName4) {
        // the mods' names on YOUR MODS: up to four, then "<4th> and N more"; "(off)" after one turned off, a ! before
        // one that isn't working
        const std::size_t n = static_cast<std::size_t>(k - kHubLineName1);
        auto name = [](const ModEntry& e) {
            if (!mod_problem(e).empty()) return "! " + e.name;
            return e.enabled ? e.name : trf("{0} (off)", {e.name});
        };
        if (n >= g_entries.size()) return "";
        if (n == 3 && g_entries.size() > 4) return trf("{0} and {1} more", {name(g_entries[3]), std::to_string(g_entries.size() - 4)});
        return name(g_entries[n]);
    }
    if (k == kHubLineMods) return tr("Settings for each mod");
    if (k == kHubLineGetMods) return tr("Opens in your browser");
    if (k == kHubLineSettings) return tr("Same as Pause > MODS");
    if (k == 1)
        if (const int out = updates_out(); out > 0) return trf("UPDATES ({0})", {std::to_string(out)});
    return tr(kHubTileNames[k]);
}

// Text the mods got from tr() (API 10), in the game's language now: each row's, link's and page's
// that the mod gave as tr()'s text (l10n.hpp). Before the pages are written, under g_mu; the game's
// language is read once for the lot.
void follow_language() {
    const std::string tag = l10n_language();
    auto follow           = [&tag](FH2Mod* m, const std::string& slot, std::string& text) { l10n_follow(m, slot, text, tag); };
    auto row_text         = [&follow](FH2Mod* m, const std::string& slot, std::string& label, std::string& description) {
        follow(m, slot + ".label", label);
        follow(m, slot + ".description", description);
    };
    for (Row& r : g_rows) {
        if (r.mod == &g_hub_owner) continue;
        row_text(r.mod, "row." + r.key, r.label, r.description);
        for (std::size_t c = 0; c < r.choices.size(); ++c) follow(r.mod, "row." + r.key + ".choice." + std::to_string(c), r.choices[c]);
    }
    for (LinkRow& l : g_links) row_text(l.mod, "row." + l.key, l.label, l.description);
    for (ModPage& pg : g_pages) {
        const std::string s = "page." + pg.key;
        follow(pg.mod, s + ".title", pg.title);
        row_text(pg.mod, s, pg.label, pg.description);
        for (std::size_t i = 0; i < pg.rows.size(); ++i) {
            PageRowData& d      = pg.rows[i];
            const std::string r = s + ".row." + std::to_string(i);
            row_text(pg.mod, r, d.label, d.description);
            follow(pg.mod, r + ".value", d.value);
            follow(pg.mod, r + ".footer", d.footer);
        }
        // the hint line's texts and the help bar's labels (API 10), cut again as they were
        for (std::size_t i = 0; i < pg.hints.size(); ++i) {
            follow(pg.mod, s + ".hint." + std::to_string(i), pg.hints[i]);
            pg.hints[i] = cut_ellipsis(pg.hints[i], kHintChars);
        }
        for (HelpPrompt& h : pg.help)
            if (!h.word) {
                follow(pg.mod, s + ".help." + glyph_name(h.glyph), h.label);
                h.label = cut_ellipsis(h.label, kPromptLabelChars);
            }
    }
}

// The mods' descriptions on the MODS tab, in the game's language now (under g_page_mu).
void follow_descriptions() {
    for (ModEntry& e : g_entries)
        if (e.mod) e.description = mod_description(e.mod);
}

// The "Load when playing online" rows' text, in the game's language now (under g_mu).
void hub_rows_text() {
    for (std::size_t i = 0; i < g_entries.size() && i < g_entry_online_row.size(); ++i) {
        if (g_entry_online_row[i] < 0) continue;
        online_row_text(g_rows[g_entry_online_row[i]], g_entries[i]);
    }
    // the UI templates' rows, and the platform's own pages (ui_player.cpp)
    for (int k = 0; k < 2; ++k)
        if (g_platform_rows[k] >= 0) player_text_into(g_rows[g_platform_rows[k]], player_row_text(k ? PlayerRow::tips : PlayerRow::mod_popups));
    for (std::size_t i = 0; i < g_entries.size() && i < g_entry_popups_row.size(); ++i)
        if (g_entry_popups_row[i] >= 0) player_text_into(g_rows[g_entry_popups_row[i]], mod_popups_text(g_entries[i].name));
    for (ModPage& pg : g_pages)
        if (pg.mod == platform_self()) platform_page_text(pg.key, pg.title, pg.label, pg.description);
}




// Extra Options' log starts with "<name> <version> - ..."; empty when Extra Options isn't loaded.
std::string extra_options_line() {
    const std::filesystem::path dir = extra_options_dir();
    if (dir.empty()) return "";
    std::string line = "Installed";
    if (FILE* f = _wfopen((dir / L"FH2_ExtraOptions.log").c_str(), L"rb")) {
        char b[256] = {};
        if (std::fgets(b, sizeof b, f)) {
            std::string l = b;
            if (const auto d = l.find(" - "); d != std::string::npos) l.resize(d);
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.pop_back();
            if (!l.empty()) line = l;
        }
        std::fclose(f);
    }
    return line;
}

PageItem info(const std::string& label, const std::string& description) { return {-1, label, description}; }

// ---- the mods' pages (API 8) ------------------------------------------------------------------
std::vector<int> g_page_view;   // each page's view on Installed mods (0: none), as last written (guarded by g_mu)

// Whether a page of the mod's own design is left out this session (under g_mu): its XAML failed the check, or the
// game stopped while it was opening last time (page_guard.cpp). page_left_out says why, in the game's language.
bool left_out(const ModPage& pg) { return pg.custom && (pg.xaml_refused || page_guard_skipped(pg.mod, pg.key)); }
std::string page_left_out(const ModPage& pg) {
    if (!left_out(pg)) return "";
    if (pg.xaml_refused)
        return trf("This page from {0} has a mistake that would crash the game, so it's left out. fh2-mods\\platform.log has more.", {pg.mod->name});
    if (page_guard_skipped(pg.mod, pg.key))
        return trf("The game crashed opening this page last time, so it's left out until {0} is updated. fh2-mods\\platform.log has more.",
                   {pg.mod->name});
    return "";
}

// A page's rows, as its list shows them (under g_mu).
std::vector<PageItem> page_items(const ModPage& pg) {
    std::vector<PageItem> items;
    for (const PageRowData& r : pg.rows) {
        PageItem it = info(r.label, r.description);
        (r.value_alert ? it.value_alert : it.value) = r.value;
        it.style   = r.style;
        it.version = r.footer;   // the line at the bottom of the description panel
        it.select  = pg.on_select ? r.key : "";
        items.push_back(it);
    }
    if (items.empty()) items.push_back(info(tr("Nothing here yet"), trf("{0} hasn't put anything on this page.", {pg.mod->name})));
    return items;
}

bool call_on_open(FH2PageOpenFn fn, void* user, const char* key) {
    __try {
        fn(user, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_on_select(FH2PageSelectFn fn, void* user, const char* page, const char* row) {
    __try {
        fn(user, page, row);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The mods' on_open, before their pages are written: page `only`, or every page. Called
// without g_mu held, so they can set their rows.
void pages_opening(int only = -1) {
    struct Call {
        FH2Mod* mod;
        FH2PageOpenFn fn;
        void* user;
        std::string key;
        int j;
    };
    std::vector<Call> calls;
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t j = 0; j < g_pages.size(); ++j)
            if (g_pages[j].on_open && (only < 0 || static_cast<std::size_t>(only) == j))
                calls.push_back({g_pages[j].mod, g_pages[j].on_open, g_pages[j].user, g_pages[j].key, static_cast<int>(j)});
    }
    for (const Call& c : calls) {
        // every page's as YOUR MODS opens (none of them shown yet), or one page's as its tile opens it
        UiCallback cb{c.mod, only < 0 ? UiCb::page_open_all : UiCb::page_open, nullptr, c.j};
        ui_note_callback(c.mod);
        if (!call_on_open(c.fn, c.user, c.key.c_str())) crashed_in(c.mod, "on_open for page " + c.key);
    }
}

// What A does on a row of a list on the MODS tab: open a page in the browser, show a view, or
// tell a mod (on_select); and the row's name, to focus it again.
struct RowAct {
    std::string url;
    int view = 0;
    std::string select;
    std::string name;
};
std::vector<RowAct> row_acts(const std::vector<PageItem>& items, const std::string& first_name, const std::string& auto_names = "") {
    std::vector<RowAct> acts;
    for (std::size_t n = 0; n < items.size(); ++n)
        acts.push_back({items[n].open, items[n].open_view, items[n].select, item_name(items[n], n, first_name, auto_names)});
    return acts;
}

// what a mod is, for its description: what it does, and whether it's loaded
std::string about_mod(const ModEntry& e) {
    return e.description.empty() ? tr("No description.") : shown(e.description);   // the platform's own for fh2wheel
}

std::string mod_note(const ModEntry& e) {
    if (!e.enabled) return tr("Turned off: the game doesn't load it.");
    if (e.online_off)
        return tr("Off for online play: you started the game from the FH2MP launcher, and it's set not to load then "
                  "(\"Load when playing online\" on its page).");
    if (stopped_online(e.mod))
        return tr("Stopped for online play: FH2MP went online after this mod had started, so its hooks are off until "
                  "you restart the game. Start the game from the FH2MP launcher to leave it out from the start.");
    if (_wcsicmp(e.dll.c_str(), L"fh2wheel.dll") == 0) return tr("Not a platform mod: its settings are in fh2wheel-config.exe.");
    if (!e.platform_mod) return tr("A mod from before the platform: it has no settings here.");
    return "";
}

// The newer version a mod (turned on) has out, without a leading v, or "".
std::string newer_out(const ModEntry& e) {
    if (!e.enabled || !e.mod || e.mod->repo.empty() || !e.mod->check_updates) return "";
    const std::string latest = latest_known(e.mod);
    if (latest.empty() || compare_versions(latest, e.version) <= 0) return "";
    return latest[0] == 'v' || latest[0] == 'V' ? latest.substr(1) : latest;
}

// the lines under a mod's description: its version (in yellow when a newer one is out: yellow
// always means an update), and its GitHub page. A whole line for each state, so each language
// words it its own way.
void add_mod_lines(PageItem& it, const ModEntry& e) {
    if (it.note.empty()) it.note = mod_note(e);
    std::string latest;
    if (e.enabled && e.mod && !e.mod->repo.empty() && e.mod->check_updates) latest = latest_known(e.mod);
    if (const std::string l = newer_out(e); !l.empty()) {
        it.version_alert = e.version.empty() ? trf("Version unknown ({0} is out)", {l}) : trf("Version {0} ({1} is out)", {e.version, l});
    } else if (e.version.empty()) {
        it.version = tr("Version unknown");   // the note and the row's value say whether it's on
    } else {
        const char* line = !e.enabled                                               ? "Version {0} (turned off)"
                           : e.online_off || stopped_online(e.mod)                  ? "Version {0} (off for online play)"
                           : !e.problem.empty()                                     ? "Version {0} (didn't load)"
                           : !e.mod || e.mod->repo.empty() || !e.mod->check_updates ? "Version {0} (doesn't check for updates)"
                           : latest.empty()                                         ? "Version {0} (not checked yet)"
                                                                                    : "Version {0} (up to date)";
        it.version = trf(line, {e.version});
    }
    if (!e.repo.empty()) it.link = "github.com/" + e.repo;
}

// how many mods (turned on) have a newer version out
int updates_out() {
    int n = 0;
    for (const ModEntry& e : g_entries)
        if (e.enabled && e.mod && !e.mod->repo.empty() && e.mod->check_updates) {
            const std::string latest = latest_known(e.mod);
            if (!latest.empty() && compare_versions(latest, e.version) > 0) ++n;
        }
    return n;
}
bool any_update_out() { return updates_out() > 0; }

// why a mod (turned on) isn't working: it didn't load or start, or it needs a newer platform
std::string mod_problem(const ModEntry& e) {
    if (!e.enabled) return "";
    if (!e.problem.empty()) return shown(e.problem);
    if (e.mod && !e.mod->platform_version.empty() && compare_versions(e.mod->platform_version, FH2MOD_PLATFORM_VERSION) > 0)
        return trf("It needs FH2 Mod Platform {0} or newer; this is {1}. Reinstall it to get the platform it came with.",
                   {e.mod->platform_version, FH2MOD_PLATFORM_VERSION});
    return "";
}
bool any_problem() {
    for (const ModEntry& e : g_entries)
        if (!mod_problem(e).empty()) return true;
    return false;
}

// Car packs (car_packs.cpp), under the mods on Installed mods: a heading, then a row per pack with
// the cars it adds, and why it isn't working when it isn't; or why the packs aren't read.
std::vector<PageItem> car_pack_items() {
    std::vector<PageItem> items;
    const std::string unread = car_packs_unread();
    if (car_packs().empty() && unread.empty()) return items;
    PageItem heading = info(tr("Car packs"), tr("Packs in the fh2-cars folder: new cars for the Autoshow."));
    heading.style    = FH2_ROW_HEADING;
    heading.skip     = true;
    items.push_back(heading);
    if (!unread.empty()) items.push_back(info(tr("Car packs aren't read"), shown(unread)));
    for (const CarPack& p : car_packs()) {
        std::string cars;   // the cars' names, as the Autoshow shows them
        for (std::size_t i = 0; i < p.cars.size(); ++i)
            cars += (i ? ", " : "") + (i < p.car_names.size() && !p.car_names[i].empty() ? p.car_names[i] : p.cars[i]);
        PageItem it = info(p.title.empty() ? p.name : p.title,
                           p.cars.empty()        ? tr("A car pack that changes the game's own cars, and adds none.")
                           : p.cars.size() == 1 ? trf("A car pack. It adds a car to the Autoshow: {0}.", {cars})
                                                : trf("A car pack. It adds {0} cars to the Autoshow: {1}.",
                                                      {std::to_string(p.cars.size()), cars}));
        it.value   = p.version;
        it.version = p.version.empty() ? tr("Version unknown") : trf("Version {0}", {p.version});
        it.link    = "fh2-cars\\" + p.name;
        if (const std::string why = car_pack_problem(p.name); !why.empty()) {   // it isn't working: says so, in red
            it.value_alert = "! " + tr("Error") + " !";
            it.value.clear();
            it.note       = trf("It isn't working: {0} fh2-mods\\platform.log has more.", {shown(why)});
            it.note_alert = true;
        } else if (const std::string warn = car_pack_sql_warning(p.name); !warn.empty()) {   // it works, but could clash
            it.note       = trf("Warning: {0} fh2-mods\\platform.log has more.", {warn});
            it.note_alert = true;
        }
        items.push_back(it);
    }
    return items;
}

std::string mods_folder();             // with About, below

// ---- being told once (fh2-mods\platform-seen.txt, see seen.hpp) -----------------------------------
// The platform's own entries ("*"):
//   home              YOUR MODS has been opened, from either door. Until then the title screen says where mod
//                     settings are, the MODS tab has a NEW badge, and the freeroam HUD welcomes the player (below).
//   welcome.sessions  how many sessions the HUD's welcome has played in: three at most.
//   first             "new" or "upgrade": the first launch of this platform here, and whether an older one had
//                     run first (platform-mods.txt was there and platform-seen.txt wasn't). Set by mod_entries
//                     (platform.cpp) before platform-mods.txt is written again, so a second start of the game in
//                     the same launch sees it too. An upgrade gets "Mod settings have moved" until moved.told.
//   moved.told        the freeroam HUD has been shown in a session whose title screen said mod settings have moved:
//                     from then on it says where they are.
// A mod's (its DLL's file name):
//   mod.new           it appeared in fh2-mods since the launch before (set by mod_entries, as first is): "New" on
//                     its row and the NEW badge, until its page is opened.
//   mod.announced     the HUD's NEW MOD notice (or the welcome naming it) has played for it: it isn't told again,
//                     until it's new again.
// Deleting the file brings every hint back, which is also how to test them.
bool home_seen() { return seen_has(kSeenPlatform, "home"); }
std::string seen_who(const ModEntry& e) { return narrow(e.dll); }

// A mod that's new since the player last looked: added to fh2-mods since the launch before, and its page not
// opened since.
bool mod_is_new(const ModEntry& e) { return seen_has(seen_who(e), "mod.new"); }

// The NEW badge's cue: YOUR MODS not opened yet, or a mod that's new.
bool new_to_player() {
    if (g_entries.empty()) return false;
    if (!home_seen()) return true;
    for (const ModEntry& e : g_entries)
        if (mod_is_new(e)) return true;
    return false;
}

// YOUR MODS opened (UI thread): the first-run hints are done with.
void mark_home() {
    if (!home_seen() && seen_set(kSeenPlatform, "home")) slog("YOUR MODS has been opened: the hints about where mod settings are stop");
}

// Mod i's page opened: it isn't new any more.
void mark_page_opened(std::size_t i) {
    if (i >= g_entries.size()) return;
    seen_clear(seen_who(g_entries[i]), "mod.announced");
    if (seen_clear(seen_who(g_entries[i]), "mod.new")) slog(g_entries[i].name + "'s page has been opened: it isn't marked new any more");
}

// A line of the platform's own for the title screen, in English: each language's Main.str gets it in its words (tr_in).
std::string title_line(const char* english) { return english; }

// The mods new since the launch before whose NEW MOD notice hasn't played yet (settings_start)
struct FreshMod {
    std::string name, who;
    bool loaded = false;   // running, so the welcome names it too
};
std::vector<FreshMod> g_fresh;
std::vector<std::string> g_fresh_names;
bool g_title_moved  = false;   // the title line this session says mod settings have moved
bool g_title_on_tab = false;   // the title line this session points at the MODS tab

// At start, before the pages and the HUD are written: the mods to tell about as new, and the line about where mod
// settings are on the title screen until YOUR MODS has been opened. `tab`: the MODS tab's files were written;
// `settings_door`: Pause > Settings > MODS is there. Without the tab the line names the Settings door, and
// without either there's none.
void first_run_cues(bool tab, bool settings_door) {
    for (const ModEntry& e : g_entries)
        if (e.enabled && mod_is_new(e) && !seen_has(seen_who(e), "mod.announced")) {
            g_fresh.push_back({e.name, seen_who(e), e.mod != nullptr});
            g_fresh_names.push_back(e.name);
        }
    std::vector<std::string> lines;
    if (!g_entries.empty() && !home_seen()) {
        if (tab) {
            g_title_moved  = seen_get(kSeenPlatform, "first") == "upgrade" && !seen_has(kSeenPlatform, "moved.told");
            g_title_on_tab = true;
            lines.push_back(g_title_moved ? title_line("Mod settings have moved: Pause > MODS") : title_line("Mod settings: Pause > MODS (the last tab)"));
            slog(g_title_moved ? "title screen: mod settings have moved (after an older platform, until the game has been played once)"
                               : "title screen: where mod settings are, until YOUR MODS is opened");
        } else if (settings_door) {
            lines.push_back(title_line("Mod settings: Pause > Settings > MODS"));
            slog("title screen: mod settings are under Pause > Settings > MODS, as the MODS tab's files couldn't be written");
        }
    }
    set_platform_notice(lines);
}

// What A on a mod's row on YOUR MODS shows, counted from what the mod added (its rows, links and
// pages), as the last sentence of the row's description. Counts go in as {0} alone, so each language
// can say them without plural forms ("Ustawienia: 13").
std::string row_hint(const ModEntry& e) {
    if (!mod_problem(e).empty()) return tr("Press A to see what's wrong.");
    if (!e.enabled) return tr("Press A to turn it back on.");
    std::size_t rows = 0, more = 0;
    if (e.mod) {
        std::scoped_lock lk{g_mu};
        for (const Row& r : g_rows) rows += r.mod == e.mod;
        for (const LinkRow& l : g_links) more += l.mod == e.mod;
        for (const ModPage& pg : g_pages) more += pg.mod == e.mod;
    }
    if (!rows) return more ? tr("Press A for its links and pages.") : tr("Press A for its page, where you can turn it off.");
    if (rows == 1) return more ? tr("Press A for its setting and more.") : tr("Press A for its setting.");
    const std::string n = std::to_string(rows);
    return more ? trf("Press A for its {0} settings and more.", {n}) : trf("Press A for its {0} settings.", {n});
}

// YOUR MODS: one row per mod, its version and state, ending in ">" (A shows its page). Yellow for an
// update, red only when it isn't working.
std::vector<PageItem> installed_items() {
    std::vector<PageItem> items;
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const ModEntry& e = g_entries[i];
        PageItem it = info(e.name, about_mod(e));
        const std::string state = !e.enabled ? tr("Off") : e.online_off || stopped_online(e.mod) ? tr("Off online") : mod_is_new(e) ? tr("New") : "";
        std::string value = state;
        if (!e.version.empty()) value += (value.empty() ? "" : "  ") + e.version;
        it.value  = value.empty() ? ">" : value + "  >";
        it.button = i < static_cast<std::size_t>(kMaxModViews) ? static_cast<int>(i) : -1;
        it.hint   = row_hint(e);
        add_mod_lines(it, e);
        if (const std::string l = newer_out(e); !l.empty()) {   // a newer version is out: in yellow
            it.value  = trf("Update {0}", {l}) + "  >";
            it.yellow = true;
        }
        if (const std::string why = mod_problem(e); !why.empty()) {   // it isn't working: says so, in red
            it.value_alert = "! " + tr("Not working");
            it.value.clear();
            it.yellow = false;
            it.note       = trf("It isn't working: {0} fh2-mods\\platform.log has more.", {why});
            it.note_alert = true;
        } else if (const std::string warn = e.enabled ? mod_sql_warning(e.name) : ""; !warn.empty()) {   // its SQL could clash
            it.note       = trf("Warning: {0} fh2-mods\\platform.log has more.", {warn});
            it.note_alert = true;
        }
        items.push_back(it);
    }
    if (g_entries.empty())
        items.push_back(info(tr("No mods installed"), trf("Mods go in the fh2-mods folder next to azimuth.exe. See {0} for some to try.", {tr("GET MODS")})));
    // the platform's own page (the UI templates' player controls, ui_player.cpp), after the mods
    const PlayerText pt = platform_line();
    PageItem platform   = info(pt.label, pt.description);
    platform.value      = std::string(FH2MOD_PLATFORM_VERSION) + "  >";
    platform.hint       = pt.hint;
    platform.button     = static_cast<int>(std::min(g_entries.size(), static_cast<std::size_t>(kMaxModViews)));
    g_platform_list_row = static_cast<int>(items.size());
    items.push_back(platform);
    const std::vector<PageItem> packs = car_pack_items();
    items.insert(items.end(), packs.begin(), packs.end());
    // A database guard stopped a crash this session (db_guards.cpp): the car the game couldn't show, last, in red.
    if (uint32_t id = 0, times = 0; db_guard_fired(&id, &times)) {
        PageItem it = info(tr("Game database"),
                           trf("A car in the game's database has a powertrain ({0}) the game doesn't know, which would have "
                               "stopped the game. The car is shown without its drivetrain instead.",
                               {std::to_string(id)}));
        it.note       = tr("The likely cause is a car pack or a mod that changes the game database. fh2-mods\\platform.log has more.");
        it.note_alert = true;
        items.push_back(it);
    }
    return items;
}

// The platform's own page, "MODS / FH2 MOD PLATFORM" (the UI templates' player controls, ui_player.cpp): its rows as
// ui_player.cpp orders them; its pages (Recent pop-ups, and UI templates with Developer=1) open as views of their own.
// `focus`: the first row that changes something.
std::vector<PageItem> platform_page_items(int& focus) {
    std::vector<PageItem> items;
    focus = -1;
    const std::vector<PlatformLine> lines = platform_page_lines();
    std::scoped_lock lk{g_mu};
    for (const PlatformLine& l : lines) {
        switch (l.kind) {
            case PlatformLine::heading: {
                PageItem h = info(l.text.label, l.text.description);
                h.style    = FH2_ROW_HEADING;
                h.skip     = true;
                items.push_back(h);
                break;
            }
            case PlatformLine::mod_popups:
            case PlatformLine::tips: {
                const int r = g_platform_rows[l.kind == PlatformLine::tips ? 1 : 0];
                if (r < 0) break;
                if (focus < 0) focus = static_cast<int>(items.size());
                items.push_back({r, "", ""});
                break;
            }
            case PlatformLine::action: {   // A runs it (player_select), and the page stays
                PageItem a = info(l.text.label, l.text.description);
                a.hint     = l.text.hint;
                a.select   = l.key;
                items.push_back(a);
                break;
            }
            case PlatformLine::page:   // A shows the page (a view of its own)
                for (std::size_t j = 0; j < g_pages.size(); ++j)
                    if (g_pages[j].mod == platform_self() && g_pages[j].key == l.key) {
                        PageItem it  = info(g_pages[j].label, g_pages[j].description);
                        it.value     = tr("Open");
                        it.hint      = tr("Press A to open it.");
                        it.open_view = j < g_page_view.size() ? g_page_view[j] : 0;
                        it.name      = "PART_Button_FH2Mod_Page_" + std::to_string(j);
                        items.push_back(it);
                    }
                break;
        }
    }
    if (focus < 0) focus = 0;
    return items;
}

// A mod's page, "MODS / <MOD>", in a fixed order: what's wrong with it (red, and it takes the focus)
// or a newer version (yellow, and it doesn't); its own rows in the order it added them (the ones
// that need a restart marked *), then its hotkeys' rows under "Keys and buttons" (as hotkeys.cpp
// added them, not by their keys); its links and pages; then "About <Mod>": what it does, Load this
// mod, and Load when playing online where FH2MP is installed. `focus` is the row the page opens on:
// what's wrong, else the first of the mod's own, else what it does. Never Load this mod, which
// one press of left or right on arrival would turn off.
std::vector<PageItem> mod_page_items(std::size_t i, int& focus) {
    std::vector<PageItem> items;
    const ModEntry& e = g_entries[i];
    focus             = -1;
    if (const std::string why = mod_problem(e); !why.empty()) {   // A opens the mods folder
        PageItem p    = info(tr("Not working"), trf("It isn't working: {0} fh2-mods\\platform.log has more.", {why}));
        p.value_alert = "!";
        p.hint        = tr("Press A to open the mods folder.");
        p.open        = mods_folder();
        p.link        = p.open;
        focus         = 0;
        items.push_back(p);
    } else if (const std::string l = newer_out(e); !l.empty()) {   // A opens the release
        PageItem u = info(tr("Update available"), trf("Version {0} is out. Get it from the mod's GitHub page (below): extract its zip into your "
                                                      "Azimuth folder, then start the game again. Press A to open it.", {l}));
        u.value    = l;
        u.yellow   = true;
        u.open     = "https://github.com/" + e.mod->repo + "/releases/latest";
        u.link     = "github.com/" + e.mod->repo + "/releases";
        items.push_back(u);
    }
    const std::size_t own_at = items.size();
    std::vector<PageItem> keys, more;
    if (e.mod) {
        std::scoped_lock lk{g_mu};
        for (std::size_t r = 0; r < g_rows.size(); ++r)
            if (g_rows[r].mod == e.mod) (g_rows[r].hotkey ? keys : items).push_back({static_cast<int>(r), "", ""});
        for (const LinkRow& l : g_links)
            if (l.mod == e.mod) {   // A opens it, and the page stays; its address shows under the description
                PageItem it = info(l.label, l.description);
                it.value    = tr("Browser");
                it.hint     = tr("Press A to open it in your web browser. The game keeps running behind it.");
                it.open     = l.url;
                it.link     = l.url.substr(l.url.find("://") + 3);
                if (!it.link.empty() && it.link.back() == '/') it.link.pop_back();
                more.push_back(it);
            }
        for (std::size_t j = 0; j < g_pages.size(); ++j)
            if (g_pages[j].mod == e.mod) {   // A shows the page (a view of its own)
                const ModPage& pg = g_pages[j];
                PageItem it = info(pg.label, pg.description);
                const bool on_tile = pg.tile >= 0 && g_tile_page[pg.tile] == static_cast<int>(j);
                if (pg.custom && pg.rows.empty()) {   // only on its tile: a pointer to it, dim (A does nothing here)
                    it.style = FH2_ROW_DIM;
                    it.hint  = on_tile ? tr(kTileOnlyThere[pg.tile]) : tr("It needs a pause-menu tile to open from, and doesn't have one.");
                } else {
                    it.value = tr("Open");
                    it.hint  = on_tile ? join_sentences(tr("Press A to open it."), tr(kTileToo[pg.tile])) : tr("Press A to open it.");
                }
                if (const std::string left_out = page_left_out(pg); !left_out.empty()) {
                    it.note       = left_out;
                    it.note_alert = true;
                }
                it.open_view = j < g_page_view.size() ? g_page_view[j] : 0;
                it.name      = "PART_Button_FH2Mod_Page_" + std::to_string(j);
                more.push_back(it);
            }
    }
    if (focus < 0 && items.size() > own_at) focus = static_cast<int>(own_at);
    if (!keys.empty()) {
        PageItem heading = info(tr("Keys and buttons"), tr("Its hotkeys: a key and a controller chord for each. Off turns one off."));
        heading.style    = FH2_ROW_HEADING;
        heading.skip     = true;
        items.push_back(heading);
        if (focus < 0) focus = static_cast<int>(items.size());
        items.insert(items.end(), keys.begin(), keys.end());
    }
    if (focus < 0 && !more.empty()) focus = static_cast<int>(items.size());
    items.insert(items.end(), more.begin(), more.end());
    if (items.size() == own_at) {   // nothing of its own: why
        PageItem none = info(tr("No settings"), tr(!e.enabled ? "Its settings show here once it's turned on and the game has restarted."
                                                   : e.online_off ? "It isn't loaded for online play, so its settings aren't here this time."
                                                   : e.platform_mod ? "This mod has no settings of its own."
                                                                    : "A mod from before the platform can't add settings here."));
        none.style = FH2_ROW_DIM;
        items.push_back(none);
    }
    PageItem about = info(trf("About {0}", {e.name}), about_mod(e));
    about.style    = FH2_ROW_HEADING;
    about.skip     = true;
    items.push_back(about);
    PageItem what = info(tr("What it does"), about_mod(e));
    what.value    = e.version;
    if (const std::string warn = e.enabled && mod_problem(e).empty() ? mod_sql_warning(e.name) : ""; !warn.empty()) {   // its SQL could clash
        what.note       = trf("Warning: {0} fh2-mods\\platform.log has more.", {warn});
        what.note_alert = true;
    }
    add_mod_lines(what, e);
    if (!e.repo.empty()) {
        what.open = "https://github.com/" + e.repo;
        what.hint = tr("Press A to open its GitHub page.");
    }
    if (focus < 0) focus = static_cast<int>(items.size());
    items.push_back(what);
    if (i < g_entry_popups_row.size() && g_entry_popups_row[i] >= 0) items.push_back({g_entry_popups_row[i], "", ""});   // UI templates
    items.push_back({g_entry_row[i], tr("Load this mod"), tr("Off leaves it in fh2-mods, but the game doesn't load it.")});
    if (i < g_entry_online_row.size() && g_entry_online_row[i] >= 0) items.push_back({g_entry_online_row[i], "", ""});
    return items;
}

// The list that stands in for the MODS tab when its hooks don't go in (Pause > Settings > MODS then opens it),
// A_ModOptions.xaml, "SETTINGS / MODS": a heading with each mod's name, its rows in its order, and its Load switch;
// then where the rest is. Only B Back on its help bar (changes are saved as they're made).
bool write_mods_page() {
    hotkeys_refresh_text();
    hud_layer_refresh_text();
    std::vector<PageItem> items;
    {
        std::scoped_lock lk{g_mu};
        hub_rows_text();
        follow_language();
    }
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const ModEntry& e = g_entries[i];
        PageItem heading  = info(e.name, about_mod(e));
        heading.style     = FH2_ROW_HEADING;
        heading.skip      = true;
        items.push_back(heading);
        if (e.mod) {
            std::scoped_lock lk{g_mu};
            for (std::size_t r = 0; r < g_rows.size(); ++r)
                if (g_rows[r].mod == e.mod) items.push_back({static_cast<int>(r), "", ""});
        }
        if (i < g_entry_popups_row.size() && g_entry_popups_row[i] >= 0) items.push_back({g_entry_popups_row[i], "", ""});   // UI templates
        items.push_back({g_entry_row[i], tr("Load this mod"), tr("Off leaves it in fh2-mods, but the game doesn't load it.")});
    }
    if (!g_entries.empty() && g_platform_rows[0] >= 0) {   // the platform's own rows (ui_player.cpp)
        const PlayerText pt = platform_line();
        PageItem heading    = info(pt.label, pt.description);
        heading.style       = FH2_ROW_HEADING;
        heading.skip        = true;
        items.push_back(heading);
        for (int r : g_platform_rows) items.push_back({r, "", ""});
    }
    if (g_entries.empty())
        items.push_back(info(tr("No mods installed"), trf("Mods go in the fh2-mods folder next to azimuth.exe. See {0} for some to try.", {tr("GET MODS")})));
    else
        items.push_back(info(tr("More for each mod"), tr("Each mod's own page, under Pause > MODS > YOUR MODS, also has its links and pages. When the "
                                                       "MODS tab isn't there, fh2-mods\\platform.log says why.")));
    return write_list_page("A_ModOptions.xaml", "MODS", items, true, nullptr, nullptr, nullptr, "{local:Loc ScreenTitles.IDS_MyProfile}",
                           kPanelLegend);
}

// Updates: the mods with a newer version out (in yellow), then how the checks work (only that,
// saying so, when there are none).
std::vector<std::string> g_update_urls;   // Updates' rows: the GitHub page A opens ("" for none)

std::vector<PageItem> updates_items() {
    std::vector<PageItem> items;
    std::vector<std::string> urls;
    for (const ModEntry& e : g_entries) {
        if (!e.enabled || !e.mod || e.mod->repo.empty() || !e.mod->check_updates) continue;
        const std::string latest = latest_known(e.mod);
        if (latest.empty() || compare_versions(latest, e.version) <= 0) continue;
        const std::string l = latest[0] == 'v' || latest[0] == 'V' ? latest.substr(1) : latest;
        PageItem it = info(e.name, trf("Version {0} is out. Get it from the mod's GitHub page (below): extract its zip into your "
                                       "Azimuth folder, then start the game again. Press A to open it.", {l}));
        it.value  = trf("Update {0}", {l});
        it.yellow = true;
        add_mod_lines(it, e);
        items.push_back(it);
        urls.push_back("https://github.com/" + e.mod->repo + "/releases/latest");
    }
    const std::string how = tr("Mods that name their GitHub page check it for new releases at most every 6 hours. A mod with a new "
                               "release says so on the title screen, under PRESS (A) TO START, and with a yellow ! on the MODS tab. A red ! "
                               "there means a mod isn't working: YOUR MODS says which, and why.");
    PageItem checks = info(tr("How update checks work"), items.empty() ? join_sentences(tr("There aren't any updates at the moment."), how) : how);
    checks.style    = FH2_ROW_DIM;   // A does nothing here
    items.push_back(checks);
    urls.push_back("");
    g_update_urls = urls;
    return items;
}


// About: the platform, Extra Options, the mods folder, reporting a problem, and credits. A
// opens each row's page (or the folder); g_about_urls has them, "" for a row without one.
constexpr const char* kPlatformGitHub = "github.com/Trinculo54/fh2-mod-platform";
constexpr const char* kAuthorGitHub   = "github.com/Trinculo54";
std::vector<std::string> g_about_urls;

std::string mods_folder() { return narrow((azimuth_dir() / L"fh2-mods").wstring()); }

std::vector<PageItem> about_items() {
    std::vector<PageItem> items;
    std::vector<std::string> urls;

    PageItem platform = info("FH2 Mod Platform", tr("Loads the mods in your fh2-mods folder and gives them this MODS tab. Each mod's "
                                                    "settings are under YOUR MODS on this tab, with its switch to turn it off. Press A to "
                                                    "open the platform's GitHub page."));
    platform.value   = FH2MOD_PLATFORM_VERSION;
    platform.version = trf("Version {0} (mod API {1})", {FH2MOD_PLATFORM_VERSION, std::to_string(FH2MOD_API_VERSION)});
    platform.link    = kPlatformGitHub;
    items.push_back(platform);
    urls.push_back(std::string("https://") + kPlatformGitHub);

    const std::string eo = extra_options_line();
    PageItem extra = info("Extra Options", tr(eo.empty() ? "Forza Horizon 2 Extra Options isn't installed. Some mods need it."
                                                         : "Forza Horizon 2 Extra Options is installed. Its own options are on the "
                                                           "game's settings pages."));
    extra.value = tr(eo.empty() ? "Not installed" : "Installed");
    extra.style = FH2_ROW_DIM;   // A does nothing here
    if (!eo.empty() && eo != "Installed") extra.version = eo;
    items.push_back(extra);
    urls.push_back("");

    const std::string folder = mods_folder();
    PageItem open = info(tr("Open mods folder"), tr("Opens fh2-mods in File Explorer, to add, remove or swap mods. Mods you add there "
                                                    "load the next time you start the game."));
    open.link = folder;
    items.push_back(open);
    urls.push_back(folder);

    // fh2-cars, where car packs are read (Azimuth 1.0 or later without Extra Options) and it's there
    if (std::error_code ec; instance_layer() && std::filesystem::is_directory(azimuth_dir() / L"fh2-cars", ec)) {
        const std::string cars = narrow((azimuth_dir() / L"fh2-cars").wstring());
        PageItem packs = info(tr("Open car packs folder"), tr("Opens fh2-cars in File Explorer, to add or remove car packs. Packs you add "
                                                              "there are read the next time you start the game."));
        packs.link = cars;
        items.push_back(packs);
        urls.push_back(cars);
    }

    PageItem report = info(tr("Report a problem"), tr("Something not working? Press A to open the platform's GitHub issues and say what "
                                                      "happened. Attach platform.log from the mods folder: it has what the platform and "
                                                      "every mod did this time."));
    report.link = std::string(kPlatformGitHub) + "/issues";
    items.push_back(report);
    urls.push_back(std::string("https://") + kPlatformGitHub + "/issues");

    PageItem credits = info(tr("Credits"), tr("FH2 Mod Platform is made by Trinculo54. Press A to open their GitHub page."));
    credits.value = "Trinculo54";
    credits.link  = kAuthorGitHub;
    items.push_back(credits);
    urls.push_back(std::string("https://") + kAuthorGitHub);

    g_about_urls = urls;
    return items;
}


// HELP (in place of ALL SETTINGS): where things are and what the marks mean, a row each, the answer
// beside it. Information only: A does nothing. The same as the README says.
std::vector<PageItem> help_items() {
    std::vector<PageItem> items;
    items.push_back(info(tr("Where mod settings are"), tr("Every mod has its own page under MODS > YOUR MODS. Pause > Settings > MODS opens the same list.")));
    items.push_back(info(tr("Changing a setting"), tr("Left and right change it. It's saved straight away; there's nothing to confirm.")));
    items.push_back(info(tr("Settings marked *"), tr("They take effect the next time you start the game. When you leave, the game offers to restart for you.")));
    items.push_back(info(tr("The ! and NEW on MODS"), tr("Red: a mod isn't working, and its row in YOUR MODS says why. Yellow: a new version is out; see UPDATES. "
                                                       "NEW: a mod you haven't looked at yet.")));
    items.push_back(info(tr("Turning a mod off"), tr("Load this mod, at the bottom of its page. It takes effect the next time you start the game. Outside "
                                                   "the game, move its DLL out of the fh2-mods folder.")));
    items.push_back(info(tr("Pop-ups and tips"), tr("FH2 Mod Platform, after the mods in YOUR MODS, turns every mod's pop-ups or tips off. "
                                                  "Recent pop-ups, on its page, says which mod sent each one.")));
    items.push_back(info(tr("Installing and updating"), tr("Extract the mod's zip into your Azimuth folder, then start the game again.")));
    items.push_back(info(tr("If MODS is missing"), tr("The MODS menus come from files the game has to read. If you use Extra Options, update it and keep "
                                                    "EnableMediaOverrides=1. fh2-mods\\platform.log says why.")));
    if (fh2mp_installed())
        items.push_back(info(tr("Online play"), tr("Each mod's page has Load when playing online: Off leaves it out when you start the game from the FH2MP "
                                                 "launcher.")));
    return items;
}

// YOUR MODS, as last written (UI thread, or before the hooks): per view (1..), what A does on each
// row and the row it opens on; which page a view is (-1: a mod's page) and the view it comes back to.
std::vector<std::vector<RowAct>> g_view_acts;
std::vector<int> g_view_focus;
std::vector<int> g_view_page, g_view_owner;
// the row view k (1..) opens on
int view_focus(int k) { return k >= 1 && static_cast<std::size_t>(k - 1) < g_view_focus.size() ? g_view_focus[k - 1] : 0; }
// A page on a tile, as last written: what A does on its rows.
std::vector<RowAct> g_tile_acts[kTileCount];
std::atomic<void*> g_tile_page_obj[kTileCount] = {};   // the page object showing a mod's page on a tile (UI thread)
std::atomic<bool> g_tile_has_panel[kTileCount] = {};  // that page was written with the hint line (its hints, set_page_hints)

bool write_hub_page(int p) {
    hotkeys_refresh_text();
    hud_layer_refresh_text();
    {
        std::scoped_lock lk{g_mu};
        hub_rows_text();
        follow_language();
    }
    std::scoped_lock lk{g_page_mu};
    follow_descriptions();
    switch (p) {
        case kHubInstalled: {
            // views 1..nmods: each mod's settings; then the platform's own page; then the mods' pages, each from its
            // mod's (the platform's own from the platform's)
            const std::size_t nmods = std::min(g_entries.size(), static_cast<std::size_t>(kMaxModViews));
            const std::size_t pview = nmods + 1;   // the platform's page
            std::vector<int> pages, owners;   // the page views: which page, and its mod's view
            {
                std::scoped_lock lk2{g_mu};
                g_page_view.assign(g_pages.size(), 0);
                for (std::size_t j = 0; j < g_pages.size() && pages.size() < static_cast<std::size_t>(kMaxPageViews); ++j) {
                    if (g_pages[j].custom && g_pages[j].rows.empty()) continue;   // only on its tile
                    int owner = g_pages[j].mod == platform_self() ? static_cast<int>(pview) : 0;
                    for (std::size_t i = 0; i < nmods && !owner; ++i)
                        if (g_entries[i].mod && g_entries[i].mod == g_pages[j].mod) owner = static_cast<int>(i + 1);
                    if (!owner) continue;
                    g_page_view[j] = static_cast<int>(pview + 1 + pages.size());
                    pages.push_back(static_cast<int>(j));
                    owners.push_back(owner);
                }
            }
            std::vector<std::vector<PageItem>> views;
            std::vector<ViewTitle> names;
            std::vector<int> focus;   // the row each view opens on
            for (std::size_t i = 0; i < nmods; ++i) {
                int f = 0;
                views.push_back(mod_page_items(i, f));
                names.push_back({g_entries[i].name, true});
                focus.push_back(f);
            }
            {
                int f = 0;
                views.push_back(platform_page_items(f));
                names.push_back({platform_line().label, true});
                focus.push_back(f);
            }
            g_platform_view = static_cast<int>(pview);
            std::vector<int> kinds(kMaxViews, kViewList);
            for (std::size_t i = 1; i <= pview; ++i) kinds[i] = kViewMod;
            std::vector<ViewHelp> helps(pview);   // the mods' own pages and the platform's have the platform's bars; their pages, maybe their own
            g_view_page.assign(1 + pview + pages.size(), -1);
            g_view_owner.assign(1 + pview + pages.size(), 0);
            {
                std::scoped_lock lk2{g_mu};
                for (std::size_t n = 0; n < pages.size(); ++n) {
                    const ModPage& pg = g_pages[pages[n]];
                    const std::size_t k = pview + 1 + n;
                    views.push_back(page_items(pg));
                    names.push_back({pg.title, false});
                    focus.push_back(0);
                    kinds[k]        = pg.on_select ? kViewPageSelect : kViewPage;
                    g_view_page[k]  = pages[n];
                    g_view_owner[k] = owners[n];
                    // as a view of rows, only A (with on_select) and B have a callback, whatever its tile's page takes
                    ViewHelp vh{false, pg.on_select != nullptr, {}};
                    for (const HelpPrompt& h : pg.help)
                        if (h.glyph == FH2_GLYPH_B || (h.glyph == FH2_GLYPH_A && pg.on_select)) vh.help.push_back(h);
                    vh.own = !vh.help.empty();
                    helps.push_back(vh);
                }
            }
            for (int k = 0; k < kMaxViews; ++k) {
                g_view_kind[k]     = kinds[k];
                g_view_own_help[k] = k >= 1 && static_cast<std::size_t>(k) <= helps.size() && helps[k - 1].own;
            }
            g_view_acts.clear();
            for (std::size_t k = 1; k <= views.size(); ++k)
                g_view_acts.push_back(row_acts(views[k - 1], "FH2Mod_First_" + std::to_string(k), "FH2Mod_R" + std::to_string(k) + "_"));
            g_view_focus = focus;
            return write_list_page(hub_file(p).c_str(), tr("YOUR MODS"), installed_items(), true, &views, &names,
                                   "{local:Loc HelpButtons.IDS_Select}", kModsTag, kPanelBound, &helps);
        }
        case kHubUpdates:
            return write_list_page(hub_file(p).c_str(), tr("UPDATES"), updates_items(), true, nullptr, nullptr, "{local:Loc HelpButtons.IDS_Select}",
                                   kModsTag);
        case kHubAbout:
            return write_list_page(hub_file(p).c_str(), tr("ABOUT"), about_items(), true, nullptr, nullptr, "{local:Loc HelpButtons.IDS_Select}",
                                   kModsTag);
        case kHubHelp:   // A does nothing on it: only B Back
            return write_list_page(hub_file(p).c_str(), tr("HELP"), help_items(), true, nullptr, nullptr, nullptr, kModsTag);
        default: break;
    }
    if (p >= kHubTilePage && static_cast<std::size_t>(p - kHubTilePage) < kTileCount) {
        // a mod's page on a game tile: a page of its own
        const std::size_t t = static_cast<std::size_t>(p - kHubTilePage);
        std::vector<PageItem> items;
        std::string title, res, content, help;
        bool select = false, custom = false, hints = false;
        ViewHelp own;
        g_tile_has_panel[t] = false;   // until it's written with one
        {
            std::scoped_lock lk2{g_mu};
            const int j = g_tile_page[t];
            if (j < 0) return false;
            const ModPage& pg = g_pages[j];
            const std::string left_out = page_left_out(pg);
            custom = pg.custom && left_out.empty();
            hints  = !pg.hints.empty() && left_out.empty();
            own    = {!pg.help.empty() && left_out.empty(), pg.on_select != nullptr && !custom, pg.help};
            if (custom) {   // of the mod's own design
                res     = pg.xaml_resources;
                content = pg.xaml_content;
                help    = pg.xaml_help;
                g_pages[j].xaml_written = content;
            } else if (!left_out.empty()) {   // left out: the platform's own row says why, rather than the game crashing
                items = {info(tr("This page couldn't be shown"), left_out)};
                title = pg.title;
                g_pages[j].xaml_written.clear();
            } else {
                items  = page_items(pg);
                title  = pg.title;
                select = pg.on_select != nullptr;
            }
        }
        bool written = false;
        if (custom) {
            g_tile_acts[t].clear();
            written = write_custom_page(hub_file(p).c_str(), res, content, help, &own, hints);
        } else {
            g_tile_acts[t] = row_acts(items, "FirstItem");
            written = write_list_page(hub_file(p).c_str(), upper_game(title), items, true, nullptr, nullptr,
                                      select ? "{local:Loc HelpButtons.IDS_Select}" : nullptr, "", hints ? kPanelBound : kPanelNone, nullptr, &own);
        }
        g_tile_has_panel[t] = written && hints;
        return written;
    }
    return false;
}

// Pause > MARKETPLACE -> MODS, with the tab's tiles where the store's were.
// The MODS tab's badge in red: copies of the tab style (A_Style_Button_TabTitle_Pause, in this
// page) and of the badge style it uses (A_Style_ContentControl_NotificationBadge, in
// Global_Controls.xaml) with the badge's colour swapped, used by the MODS tab only. The rest is
// the game's own XAML, word for word.
std::string xaml_block(const std::string& text, const std::string& key, const std::string& open, const std::string& close) {
    const std::size_t k = text.find("x:Key=\"" + key + "\"");
    if (k == std::string::npos) return "";
    const std::size_t a = text.rfind(open, k), b = text.find(close, k);
    if (a == std::string::npos || b == std::string::npos) return "";
    return text.substr(a, b + close.size() - a);
}

bool swap_all(std::string& s, const std::string& from, const std::string& to) {
    bool any = false;
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
        any = true;
    }
    return any;
}

bool red_badge(std::string& page, std::size_t tab_at) {
    std::string tab_style = xaml_block(page, "A_Style_Button_TabTitle_Pause", "<Style", "</Style>");
    std::string new_badge;   // the game's own badge, for NEW
    const std::string badge = xaml_block(read_ui("Resources/Anthem/Global_Controls.xaml"), "A_Style_ContentControl_NotificationBadge", "<Style",
                                         "</Style>");
    std::string red = badge, yellow = badge;
    if (tab_style.empty() || badge.empty() ||
        !swap_all(tab_style, "x:Key=\"A_Style_Button_TabTitle_Pause\"", "x:Key=\"FH2Mod_Style_Button_TabTitle_Pause\"") ||
        !swap_all(red, "x:Key=\"A_Style_ContentControl_NotificationBadge\"", "x:Key=\"FH2Mod_Style_NotificationBadge_Red\"") ||
        !swap_all(red, "{StaticResource A_Color_TileSelected_Background}", "{StaticResource FH2Mod_Color_BadgeRed}") ||
        !swap_all(yellow, "x:Key=\"A_Style_ContentControl_NotificationBadge\"", "x:Key=\"FH2Mod_Style_NotificationBadge_Yellow\"") ||
        !swap_all(yellow, "{StaticResource A_Color_TileSelected_Background}", "{StaticResource FH2Mod_Color_BadgeYellow}") ||
        !swap_all(yellow, "{StaticResource A_Color_TileSelected_Text}", "{StaticResource FH2Mod_Color_BadgeDark}"))
        return false;
    // The tab's badge (its Tag, FH2Mod_ErrorBadge) in red, and a second one beside it in
    // yellow for updates (FH2Mod_UpdateBadge; it shows only when the red one doesn't).
    const std::size_t c0 = tab_style.find("<ContentControl Content=\"{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}\"");
    const std::size_t c1 = c0 == std::string::npos ? c0 : tab_style.find("/>", c0);
    if (c1 == std::string::npos) return false;
    std::string tag_badge = tab_style.substr(c0, c1 + 2 - c0), update_badge = tag_badge;
    new_badge = tag_badge;
    if (!swap_all(tag_badge, "{StaticResource A_Style_ContentControl_NotificationBadge}", "{StaticResource FH2Mod_Style_NotificationBadge_Red}") ||
        !swap_all(update_badge, "{StaticResource A_Style_ContentControl_NotificationBadge}", "{StaticResource FH2Mod_Style_NotificationBadge_Yellow}") ||
        !swap_all(update_badge, "{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}", "{Binding FH2Mod_UpdateBadge}") ||
        !swap_all(new_badge, "{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}", "{Binding FH2Mod_NewBadge}"))
        return false;
    // a third, in the game's own colour, for NEW (it shows only when neither of the others does)
    tab_style.replace(c0, c1 + 2 - c0, tag_badge + "\r\n                                    " + update_badge + "\r\n                                    " + new_badge);
    // the MODS tab uses the copy
    const std::size_t end = page.find("/>", tab_at), st = page.find("Style=\"{StaticResource A_Style_Button_TabTitle_Pause}\"", tab_at);
    if (end == std::string::npos || st == std::string::npos || st > end) return false;
    page.replace(st, std::string("Style=\"{StaticResource A_Style_Button_TabTitle_Pause}\"").size(),
                 "Style=\"{StaticResource FH2Mod_Style_Button_TabTitle_Pause}\"");
    // the copies go right after the original, so they see the same resources it does
    const std::size_t orig = page.find(xaml_block(page, "A_Style_Button_TabTitle_Pause", "<Style", "</Style>"));
    if (orig == std::string::npos) return false;
    const std::size_t after = page.find("</Style>", orig) + std::string("</Style>").size();
    page.insert(after, "\r\n            <Color x:Key=\"FH2Mod_Color_BadgeRed\">#FFE0141E</Color>"
                       "\r\n            <Color x:Key=\"FH2Mod_Color_BadgeYellow\">#FFFFC400</Color>"
                       "\r\n            <Color x:Key=\"FH2Mod_Color_BadgeDark\">#FF14181E</Color>\r\n            " +
                           red + "\r\n            " + yellow + "\r\n            " + tab_style);
    return true;
}

// A line of text on a MODS tab tile, under its name: FH2Mod_HubTile_<k> (a HubLine), at the bottom left in `style`,
// collapsed while it's empty (the game's converter for its badges).
std::string hub_line(int k, const char* style) {
    const std::string bind = "{Binding FH2Mod_HubTile_" + std::to_string(k) + ", RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}";
    return "                                <local:FZTextBlock Text=\"" + bind + "}\" Style=\"{StaticResource " + style +
           "}\" Visibility=\"" + bind + ", Converter={StaticResource A_StringVisConverter}}\" />\r\n";
}
// A badge in a MODS tab tile's top right corner: the tab's, repeated on the tile that resolves it (BadgeKind).
std::string tile_badge(int kind, const std::string& style) {
    return "                        <ContentControl Content=\"{Binding " + std::string(kBadgeNames[kind]) +
           ", RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}}\" Style=\"{StaticResource " + style +
           "}\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Top\" Height=\"30\" Panel.ZIndex=\"1\" Margin=\"0,20,20,0\" />\r\n";
}

// A MODS tab tile: the game's icon tile (HorizontalIconTile), with an icon of our own drawn
// in its place: Paths in the light grey of the game's icons, big and cut off at the tile's
// edges as theirs are. `strokes` are lines, `fills` solid shapes, `rings` circles with the
// tile's colour inside (a slider's knob).
std::string tile_template(const std::string& key, const char* strokes, const char* fills, const char* rings, const std::string& extra = "") {
    const std::string grey = "#FFDCDCDC", sw = "26";
    std::string icon;
    if (*strokes) icon += "                        <Path Data=\"" + std::string(strokes) + "\" Stroke=\"" + grey + "\" StrokeThickness=\"" + sw + "\" />\r\n";
    if (*fills) icon += "                        <Path Data=\"" + std::string(fills) + "\" Fill=\"" + grey + "\" />\r\n";
    if (*rings)
        icon += "                        <Path Data=\"" + std::string(rings) + "\" Fill=\"#FFF5F5F5\" Stroke=\"" + grey +
                "\" StrokeThickness=\"" + sw + "\" />\r\n";
    return "\r\n            <ControlTemplate x:Key=\"" + key + "\" TargetType=\"{x:Type Button}\">\r\n"
           "                <Grid Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition}\" "
           "local:UIPageTransitionManager.TransitionIndex=\"{Binding (local:UIPageTransitionManager.TransitionIndex), RelativeSource={RelativeSource TemplatedParent}}\">\r\n"
           "                    <Grid Style=\"{StaticResource A_Style_Grid_HiddenWhenLocked}\">\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileShadow}\" />\r\n"
           "                    </Grid>\r\n"
           "                    <Grid Style=\"{StaticResource A_Style_Grid_MotionBlur}\">\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileBackground}\" />\r\n"
           "                        <Grid ClipToBounds=\"True\">\r\n"
           "                        <Grid RenderTransformOrigin=\"1,1\">\r\n"   // drawn big, shown at 80% toward the corner
           "                        <Grid.RenderTransform>\r\n"
           "                            <ScaleTransform ScaleX=\"0.8\" ScaleY=\"0.8\" />\r\n"
           "                        </Grid.RenderTransform>\r\n" + icon +
           "                        </Grid>\r\n"
           "                        </Grid>\r\n"
           "                        <Grid Margin=\"{StaticResource A_Thickness_TilePadding}\">\r\n"
           "                            <local:FZTextBlock Text=\"{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_FZTextBlock_TileTitle}\" HorizontalAlignment=\"Left\" TextAlignment=\"Left\" />\r\n"
           "                        </Grid>\r\n" + extra +
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileSelectionBar}\" />\r\n"
           "                    </Grid>\r\n"
           "                </Grid>\r\n"
           "            </ControlTemplate>";
}

// A tile a mod's page has taken over shows the page's set_page_tile_text and
// set_page_tile_colors: its Button gets Tag="{Binding FH2Mod_TileText_<t>}" (from the page) and
// a copy of the game's icon tile template (A_ControlTemplate_IconTile, Shared_TiledMenus.xaml)
// with one more line, the Tag, in the pause menu's own style for the club tile's member count,
// and the background, title, line and icon colours bound to the page's FH2Mod_Tile..._<t>
// (unset, they're the game's own). Only tiles a mod's page has get it.
std::string tile_template_key(std::size_t t) { return "FH2Mod_Template_IconTile_" + std::to_string(t); }
std::string icon_tile_text_template(std::size_t t) {
    const std::string n = std::to_string(t);
    auto bind = [&](const char* prop) {
        return std::string("{Binding FH2Mod_") + prop + "_" + n + ", RelativeSource={RelativeSource FindAncestor, AncestorType={x:Type Page}}}";
    };
    return "\r\n            <ControlTemplate x:Key=\"" + tile_template_key(t) + "\" TargetType=\"{x:Type Button}\">\r\n"
           "                <Grid Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition}\" "
           "local:UIPageTransitionManager.TransitionIndex=\"{Binding (local:UIPageTransitionManager.TransitionIndex), RelativeSource={RelativeSource TemplatedParent}}\">\r\n"
           "                    <Grid Style=\"{StaticResource A_Style_Grid_HiddenWhenLocked}\">\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileShadow}\" />\r\n"
           "                    </Grid>\r\n"
           "                    <Grid Style=\"{StaticResource A_Style_Grid_MotionBlur}\">\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileBackground}\" Color=\"" + bind("TileBack") + "\" />\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_LockableIcon}\" Color=\"" + bind("TileIcon") + "\" "
           "ImageSource=\"{Binding DataContext.Icon, RelativeSource={RelativeSource TemplatedParent}}\" HorizontalAlignment=\"Stretch\" VerticalAlignment=\"Stretch\" />\r\n"
           "                        <ContentControl Content=\"{Binding (local:BindingProxy.A), RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_ContentControl_NotificationBadge}\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Bottom\" "
           "Height=\"30\" Panel.ZIndex=\"1\" Margin=\"20,0,0,20\" />\r\n"
           "                        <Grid Margin=\"{StaticResource A_Thickness_TilePadding}\">\r\n"
           "                            <local:FZTextBlock x:Name=\"NameText\" Text=\"{Binding DataContext.Heading, RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_FZTextBlock_TileTitle}\" Color=\"" + bind("TileTitle") + "\" "
           "MaxWidth=\"{Binding Width, RelativeSource={RelativeSource TemplatedParent}}\" />\r\n"
           "                            <local:FZTextBlock Text=\"{Binding Tag, RelativeSource={RelativeSource TemplatedParent}}\" "
           "Style=\"{StaticResource A_Style_FZTextBlock_TileClubMemberCount}\" Color=\"" + bind("TileLine") + "\" />\r\n"
           "                            <local:FZMaterial Style=\"{StaticResource A_Style_Tile_LockIcon}\" />\r\n"
           "                        </Grid>\r\n"
           "                        <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileSelectionBar}\" />\r\n"
           "                    </Grid>\r\n"
           "                </Grid>\r\n"
           "            </ControlTemplate>";
}
bool add_tile_text(std::string& page, const char* button, std::size_t t) {
    const std::size_t named = page.find(std::string("x:Name=\"") + button + "\"");
    if (named == std::string::npos) return false;
    const std::size_t a = page.rfind("<Button", named), b = page.find("/>", named);
    if (a == std::string::npos || b == std::string::npos) return false;
    std::string el = page.substr(a, b - a);
    const std::string from = "Template=\"{StaticResource A_ControlTemplate_IconTile}\"";
    const std::size_t at   = el.find(from);
    if (at == std::string::npos || el.find("Tag=") != std::string::npos) return false;
    el.replace(at, from.size(), "Template=\"{StaticResource " + tile_template_key(t) + "}\"\r\n                                Tag=\"{Binding FH2Mod_TileText_" +
                                    std::to_string(t) + ", RelativeSource={RelativeSource AncestorType=Page, Mode=FindAncestor}}\"");
    page.replace(a, b - a, el);
    return true;
}

bool write_pause_menu() {
    std::string page = read_ui("Scenes/Anthem/A_PauseMenu.xaml");
    if (page.empty()) return false;
    const std::string tab      = "Content=\"{local:Loc PauseMenu.IDS_CatTitle_Marketplace}\"";
    const std::string badge    = "Tag=\"{Binding NotificationBadgeMarketplace}\"";
    const std::string items_at = "x:Key=\"A_MenuDataItem_VIPPass\" />";
    const std::string scope    = "x:Name=\"PART_SCOPE_Marketplace\">";
    if (page.find(tab) == std::string::npos || page.find(items_at) == std::string::npos ||
        page.find(scope) == std::string::npos) {
        slog("A_PauseMenu.xaml doesn't look as expected; no MODS tab");
        return false;
    }
    struct Tile {
        const char *name, *item, *heading, *size;
        const char *strokes, *fills, *rings;   // the tile's icon (Path data; see tile_template)
        int x, y;
    };
    static const Tile tiles[] = {
        // three columns of two: Installed mods big on the left, then two pairs (the Season Pass
        // tile, the store's, is left out: nothing wires it)
        {"PART_Button_FH2Mod_Installed", "FH2Mod_Tile_Installed", "INSTALLED MODS", "2x2",   // a puzzle piece
         "M190,250 L300,250 A50,50 0 1 1 380,250 L490,250 L490,350 A50,50 0 1 1 490,430 L490,560 L190,560 L190,450 A50,50 0 1 0 190,370 Z",
         "", "", 0, 0},
        {"PART_Button_CarPacks", "FH2Mod_Tile_Updates", "UPDATES", "2x1",   // two arrows round a circle
         "M331,134 A105,105 0 0 1 521,117 M529,206 A105,105 0 0 1 339,222",
         "M554,98 L541,152 488,136 Z M306,242 L319,188 372,204 Z", "", 2, 0},
        {"PART_Button_ExpansionPacks", "FH2Mod_Tile_GetMods", "GET MODS", "2x1",   // a download into a tray
         "M410,30 L410,190 M335,120 L410,195 485,120 M270,170 L270,250 530,250 530,170", "", "", 2, 1},
        {"PART_Button_Extras", "FH2Mod_Tile_About", "ABOUT", "2x1",   // an i in a circle
         "M420,40 A130,130 0 0 1 420,300 A130,130 0 0 1 420,40 Z M420,140 L420,250", "M420,72 A20,20 0 0 1 420,112 A20,20 0 0 1 420,72 Z", "", 4, 0},
        {"PART_Button_VIPMembership", "FH2Mod_Tile_Help", "HELP", "2x1",   // a ? in a circle
         "M420,40 A130,130 0 0 1 420,300 A130,130 0 0 1 420,40 Z M372,140 A48,48 0 1 1 440,182 L420,200 L420,232",
         "M420,252 A18,18 0 0 1 420,288 A18,18 0 0 1 420,252 Z", "", 4, 1},
    };
    // The badges in the tiles: copies of the game's badge style (Global_Controls.xaml) in red and yellow, with keys
    // and colours of their own (red_badge's copies for the tab come later in the page). NEW uses the game's own.
    std::string res, buttons = "\r\n";
    const std::string badge_style = xaml_block(read_ui("Resources/Anthem/Global_Controls.xaml"), "A_Style_ContentControl_NotificationBadge", "<Style", "</Style>");
    std::string red = badge_style, yellow = badge_style;
    const bool tile_badges =
        !badge_style.empty() && swap_all(red, "x:Key=\"A_Style_ContentControl_NotificationBadge\"", "x:Key=\"FH2Mod_Style_TileBadge_Red\"") &&
        swap_all(red, "{StaticResource A_Color_TileSelected_Background}", "{StaticResource FH2Mod_Color_TileBadgeRed}") &&
        swap_all(yellow, "x:Key=\"A_Style_ContentControl_NotificationBadge\"", "x:Key=\"FH2Mod_Style_TileBadge_Yellow\"") &&
        swap_all(yellow, "{StaticResource A_Color_TileSelected_Background}", "{StaticResource FH2Mod_Color_TileBadgeYellow}") &&
        swap_all(yellow, "{StaticResource A_Color_TileSelected_Text}", "{StaticResource FH2Mod_Color_TileBadgeDark}");
    if (tile_badges)
        res += "\r\n            <Color x:Key=\"FH2Mod_Color_TileBadgeRed\">#FFE0141E</Color>"
               "\r\n            <Color x:Key=\"FH2Mod_Color_TileBadgeYellow\">#FFFFC400</Color>"
               "\r\n            <Color x:Key=\"FH2Mod_Color_TileBadgeDark\">#FF14181E</Color>\r\n            " +
               red + "\r\n            " + yellow;
    else
        slog("the game's badge style doesn't look as expected; the MODS tab's tiles show no badges");
    // The tiles' lines: YOUR MODS' in the club tile's count style (this page's own) and the names in the tiles' text
    // style; GET MODS' the same.
    const bool club_style = page.find("x:Key=\"A_Style_FZTextBlock_TileClubMemberCount\"") < page.find(items_at);
    const char* const first_style = club_style ? "A_Style_FZTextBlock_TileClubMemberCount" : "A_Style_FZTextBlock_TileText";
    std::string mods_lines = "                        <StackPanel Margin=\"{StaticResource A_Thickness_TilePadding}\" VerticalAlignment=\"Bottom\" "
                             "HorizontalAlignment=\"Left\">\r\n" + hub_line(kHubLineMods, first_style);
    for (int k = kHubLineName1; k <= kHubLineName4; ++k) mods_lines += hub_line(k, "A_Style_FZTextBlock_TileText");
    mods_lines += "                        </StackPanel>\r\n";
    if (tile_badges) mods_lines += tile_badge(kBadgeError, "FH2Mod_Style_TileBadge_Red");
    mods_lines += tile_badge(kBadgeTileNew, "A_Style_ContentControl_NotificationBadge");
    const std::string extras[] = {
        mods_lines,
        tile_badges ? tile_badge(kBadgeTileUpdate, "FH2Mod_Style_TileBadge_Yellow") : "",
        "                        <StackPanel Margin=\"{StaticResource A_Thickness_TilePadding}\" VerticalAlignment=\"Bottom\" HorizontalAlignment=\"Left\">\r\n" +
            hub_line(kHubLineGetMods, first_style) + "                        </StackPanel>\r\n",
        "", ""};
    int index = 1;
    for (const Tile& t : tiles) {
        // The name shown is the Button's Tag, FH2Mod_HubTile_<k>: in the game's language, with the counts
        res += std::string("\r\n            <local:TileMenuDataItem x:Key=\"") + t.item + "\" Heading=\"" + t.heading + "\" />";
        res += tile_template(std::string(t.item) + "_Template", t.strokes, t.fills, t.rings, extras[&t - tiles]);
        buttons += std::string("                        <Button Canvas.Left=\"{StaticResource A_Double_TileCoord_X_") + std::to_string(t.x) +
                   "}\"\r\n                                Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_" + std::to_string(t.y) +
                   "}\"\r\n                                DataContext=\"{StaticResource " + t.item +
                   "}\"\r\n                                Style=\"{StaticResource A_Style_Button_Tile_" + t.size +
                   "}\"\r\n                                Template=\"{StaticResource " + std::string(t.item) + "_Template" +
                   "}\"\r\n                                Tag=\"{Binding FH2Mod_HubTile_" + std::to_string(&t - tiles) +
                   ", RelativeSource={RelativeSource AncestorType=Page, Mode=FindAncestor}}" +
                   "\"\r\n                                local:TileMenuNavigationManager.TileState=\"Enabled\""
                   "\r\n                                local:TileMenuNavigationManager.GridX=\"" + std::to_string(t.x) +
                   "\"\r\n                                local:TileMenuNavigationManager.GridY=\"" + std::to_string(t.y) +
                   "\"\r\n                                local:UIPageTransitionManager.TransitionIndex=\"" + std::to_string(index++) +
                   "\"\r\n                                x:Name=\"" + t.name + "\" />\r\n";
    }
    buttons += "                    ";
    if (!replace_between(page, scope, "</Canvas>", buttons, false)) return false;
    // the tiles mods' pages have: their text (the template goes in after the club tile's style)
    if (page.find("x:Key=\"A_Style_FZTextBlock_TileClubMemberCount\"") < page.find(items_at))
        for (std::size_t t = 0; t < kTileCount; ++t) {
            {
                std::scoped_lock lk{g_mu};
                if (g_tile_page[t] < 0) continue;
            }
            bool any = false;
            for (const char* b : kTiles[t].buttons)
                if (add_tile_text(page, b, t)) any = true;
                else slog(std::string(b) + " doesn't look as expected; it shows no text or colours");
            if (any) res += icon_tile_text_template(t);
        }
    page.insert(page.find(items_at) + items_at.size(), res);
    const std::size_t tab_at = page.find(tab);
    page.replace(tab_at, tab.size(), "Content=\"MODS\"");
    if (const std::size_t b = page.find(badge); b != std::string::npos)
        page.replace(b, badge.size(), "Tag=\"{Binding FH2Mod_ErrorBadge}\"");
    if (!red_badge(page, tab_at)) slog("couldn't make the MODS tab's badge red; it's the game's colour");
    return set_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml", page.data(), page.size()) != 0;
}

// The Marketplace tiles' events go to a node of ours that shows the HUD page (as one of the
// tab's pages) and comes back to the pause menu. (A HUD page can only leave by "back": an
// event of another name, even one the game has, doesn't reach a transition.)
bool write_pause_flow() {
    const auto zip = game_mount() / L"media" / L"stateflow.zip";
    std::vector<char> x = unzip_file(zip, "pause.xml");
    std::string flow(x.begin(), x.end());
    const std::string exit = "<exit id=\"car_packs\" target=\"_marketplace_car_pack.enter\" />";
    const std::string node = "<node id=\"_marketplace_extras\"";
    const std::size_t e = flow.find(exit), n = flow.find(node);
    if (flow.empty() || e == std::string::npos || n == std::string::npos || flow.find(exit, e + 1) != std::string::npos) {
        slog("stateflow pause.xml doesn't look as expected; no MODS tab");
        return false;
    }
    flow.insert(n, "<node id=\"_fh2mod_page\" state=\"ui_screens.show_hud_options\">\r\n"
                   "      <exit id=\"back\" target=\"pause_menu.enterback\" />\r\n"
                   "    </node>\r\n\r\n    ");
    flow.replace(e, exit.size(), "<exit id=\"car_packs\" target=\"_fh2mod_page\" />");
    return set_file(&g_owner_mod, "stateflow/pause.xml", flow.data(), flow.size()) != 0;
}

// ---- the Mods tile ---------------------------------------------------------------------------
constexpr uintptr_t RVA_PAUSE_WIRE_TILES = 0x0DC8A60;   // PauseOptionsMenu: wire the tiles (page)
constexpr uintptr_t RVA_WIRE_BUTTON      = 0x0D9FC10;   // (page, const char* name, const Pmf*)
constexpr uintptr_t RVA_HUD_TILE         = 0x0DC6C40;   // PauseOptionsMenu::OnHud(page)
constexpr uintptr_t RVA_HUD_SCENE_RECORD = 0x0B3A3E0;   // SceneRecord* build(SceneRecord* out)
constexpr uintptr_t RVA_PAUSE_NAV_SIZE   = 0x0DC873E;   // mov [rbp+0x40], 2; mov [rbp+0x48], 5

struct Pmf {
    void* fn;
    int32_t adjust, pad;
};
using WireTilesFn  = void (*)(void* page);
using WireButtonFn = void (*)(void* page, const char* name, const Pmf* pmf);
using TileFn       = void (*)(void* page);
using SceneRecFn   = uint8_t* (*)(uint8_t* out);
WireTilesFn o_wire_tiles = nullptr;
SceneRecFn o_hud_scene   = nullptr;

std::atomic<uint64_t> g_mods_asked{0};   // GetTickCount64() when the tile opened the stand-in list
std::string g_asked_values;              // the values the restart popup last asked about
bool g_popup_ok = false;

// One home, two doors: Pause > Settings > MODS and Pause > MODS > YOUR MODS open the same list. Which door the
// player came in by (UI thread only): the parent page's wiring hook (d_wire_tiles, d_pause_menu_wire) asks
// about a restart as the player comes back through it, and Go back goes in again by the same door.
enum Route { kRouteNone, kRouteSettings, kRouteTab };
int g_route      = kRouteNone;
int g_route_page = kHubInstalled;        // the MODS tab's page the player was on (HubPage)
bool g_go_back   = false;                // the next page opens where the player left it (Go back)
std::atomic<bool> g_hub_ready{false};    // the MODS tab's hooks are in: the Settings door opens YOUR MODS

void hub_reset();
void prepare_hub(int which, bool resume);   // with the MODS tab's tiles, below

// The Settings page's MODS tile: YOUR MODS, through the HUD tile's handler, so B comes back to Settings. Without
// the MODS tab's hooks, the list that stands in for it.
void mark_home();

void mods_tile_pressed(void* page) {
    hub_reset();
    g_panel_obj = nullptr;   // A_ModOptions has no hint line; YOUR MODS' bindings set it again
    mark_home();
    g_route = kRouteSettings;
    if (g_hub_ready.load()) {
        prepare_hub(kHubInstalled, std::exchange(g_go_back, false));
    } else {
        g_go_back = false;
        write_mods_page();   // up to date: the mods may have changed their rows' text (set_setting_text)
        g_mods_asked = GetTickCount64();
    }
    reinterpret_cast<TileFn>(g_base + RVA_HUD_TILE)(page);
}
void settings_go_back(void* page) {
    g_go_back = true;
    mods_tile_pressed(page);
}

void wire_mods_tile(void* page) {
    static const Pmf pmf{reinterpret_cast<void*>(&mods_tile_pressed), 0, 0};
    __try {
        reinterpret_cast<WireButtonFn>(g_base + RVA_WIRE_BUTTON)(page, "PART_Button_Mods", &pmf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The Settings page's controller grid, 5 x 2, made 6 wide for the MODS tile. False when it isn't 5 x 2
// (or is already 6): the caller says so in the log (outside the __try).
bool widen_nav(void* page) {
    __try {
        uint8_t* nav = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(page) + 0x410);
        if (nav && *reinterpret_cast<int*>(nav + 0x9c) == 5 && *reinterpret_cast<int*>(nav + 0xa0) == 2) {
            *reinterpret_cast<int*>(nav + 0x9c) = 6;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// L"Anthem\A_HudOptions" -> L"Anthem\A_ModOptions" (or another 12-letter page), in place
bool point_at_page(uint8_t* rec, const wchar_t* page) {
    __try {
        const uint64_t size = *reinterpret_cast<uint64_t*>(rec + 0x38);
        const uint64_t cap  = *reinterpret_cast<uint64_t*>(rec + 0x40);
        wchar_t* p = cap > 7 ? *reinterpret_cast<wchar_t**>(rec + 0x28) : reinterpret_cast<wchar_t*>(rec + 0x28);
        if (size != 19 || !readable(p, 40) || std::wcsncmp(p, L"Anthem\\A_HudOptions", 19) != 0) return false;
        std::wmemcpy(p + 7, page, 12);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The MODS tab: the page a tile (or the Settings door) asked for. Opening a page builds its scene
// record twice, so an ask lasts for two builds (or 1.5 s).
std::atomic<int> g_hub_page{0};
std::atomic<uint64_t> g_hub_asked{0};
std::atomic<int> g_hub_builds{0};
std::atomic<int> g_hub_shown{0};         // the tab's page being opened, for wiring its rows
std::atomic<bool> g_mods_page_next{false};      // the page being opened is A_ModOptions (the stand-in list)
std::atomic<void*> g_mods_options_page{nullptr}; // the page object showing it (set as it loads; only compared)
// The mod's page (a view of YOUR MODS) and row the player last left with B, for Go back; g_resume: the YOUR
// MODS being opened shows that view, and focuses that row once it's loaded (UI thread only).
int g_last_view = 0, g_last_row = 0;
bool g_resume   = false;

void hub_reset() {
    g_hub_builds = 0;
    g_hub_shown  = 0;
}

void hub_ask(int page) {
    g_hub_page   = page;
    g_hub_builds = 2;
    g_hub_asked  = GetTickCount64();
}

uint8_t* d_hud_scene(uint8_t* out) {
    uint8_t* r = o_hud_scene(out);
    const uint64_t now = GetTickCount64(), asked = g_mods_asked.load();
    std::wstring page;
    if (g_hub_builds.load() > 0 && now - g_hub_asked.load() < 1500) {
        --g_hub_builds;
        const int p = g_hub_page.load();
        page        = hub_name(p);
        g_hub_shown = p;
    } else if (asked && now - asked < 1500) {
        page             = L"A_ModOptions";
        g_mods_page_next = true;
    }
    if (!page.empty() && !point_at_page(r, page.c_str()))
        slog("the HUD page's scene record doesn't look as expected; opened the HUD page");
    return r;
}

// ---- the restart popup -------------------------------------------------------------------
//
// The game's own message box (A_MessageBox.xaml, a PopupMenu page), the one the options pages
// use for "Unsaved changes". 0x140D09D00(page, &title, &message, items) makes it, shows it over
// `page` and returns it. Title and message are references to localised strings (CLocString,
// whose text comes from its GetText, vtable +0x38); ours are CLocStrings with a GetText of
// our own. The items are a collection (0x140C65230) of { id at +0x68, label } (0x140C65AB0),
// added through the collection's vtable +0x40 (collection, index, item); the popup takes the
// collection's interface at +0x38. 0x140DD8820(popup, 6) sets its help bar, and
// 0x140DD86F0(popup, 2, fn, true) its handler for a chosen item, an MSVC
// std::function<void()> ({ space[0x18], impl* }) taken by value. The handler reads the chosen
// item with 0x140E01A60(popup); the popup closes by itself.
constexpr uintptr_t RVA_LOCSTRING_VT   = 0x2EB0E30;   // CLocString's vtable (11 slots)
constexpr uintptr_t RVA_LOCSTRING_TEXT = 0x123D720;   // its GetText
constexpr uintptr_t RVA_MAKE_ITEMS     = 0x0C65230;   // (Ptr* out): an empty collection
constexpr uintptr_t RVA_MAKE_ITEM      = 0x0C65AB0;   // (Ptr* out, const int* id, LocRef* label)
constexpr uintptr_t RVA_SHOW_POPUP     = 0x0D09D00;   // Popup* (page, LocRef* title, LocRef* message, items)
constexpr uintptr_t RVA_POPUP_HELP     = 0x0DD8820;   // (popup, int layout)
constexpr uintptr_t RVA_POPUP_HANDLER  = 0x0DD86F0;   // (popup, int event, std::function<void()>, bool)
constexpr uintptr_t RVA_POPUP_SELECTED = 0x0E01A60;   // Item* (popup)
constexpr uintptr_t RVA_OBJECT_FREE    = 0x0257730;   // when an AVUI object's refcount reaches 0
constexpr uintptr_t RVA_SOME_TYPEINFO  = 0x0D0FBC0;   // a game lambda's _Target_type, for ours

struct FakeLocString {
    void* vt;
    uint16_t table;   // 0xFFFF: none
    uint16_t pad;
    uint32_t id;
    int32_t refs;
    int32_t pad2;
    const wchar_t* text;
};
void* g_loc_vt[11];
// in the game's language, as it's shown: a mod's text from tr() (API 10), or the platform's own
const wchar_t* fake_text(FakeLocString* self) {
    const wchar_t* t = l10n_follow_w(self->text);
    return t ? t : tr_w(self->text);
}
void** fake_clone(FakeLocString* self, void** out) {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&self->refs));
    *out = self;
    return out;
}
FakeLocString* loc_string(const wchar_t* text) {
    auto* s  = static_cast<FakeLocString*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FakeLocString)));
    s->vt    = g_loc_vt;
    s->table = 0xFFFF;
    s->refs  = 1 << 20;   // never freed
    s->text  = text;
    return s;
}

struct StdFunction {
    void* space[3];
    void* impl;
};
struct PopupFn {
    void** vt;
    void* popup;
    void* page;   // the Settings page it was shown over
};
void handle_choice(PopupFn* self);
void (*g_popup_back)(void* page) = nullptr;   // "Go back": reopen the page the popup came from
void* fn_copy(PopupFn* self, void* where) {
    auto* n = where ? static_cast<PopupFn*>(where) : static_cast<PopupFn*>(HeapAlloc(GetProcessHeap(), 0, sizeof(PopupFn)));
    *n = *self;
    return n;
}
void fn_call(PopupFn* self) { handle_choice(self); }
const void* fn_type(PopupFn*) { return reinterpret_cast<const void* (*)()>(g_base + RVA_SOME_TYPEINFO)(); }
void fn_delete(PopupFn* self, bool dealloc) {
    if (dealloc) HeapFree(GetProcessHeap(), 0, self);
}
void* fn_dtor(PopupFn* self, unsigned) { return self; }
void* fn_get(PopupFn* self) { return &self->popup; }
void* g_fn_vt[7] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_call),
                    reinterpret_cast<void*>(&fn_type), reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_dtor),
                    reinterpret_cast<void*>(&fn_get)};
void handle_title_choice(PopupFn* self);
void fn_call_title(PopupFn* self) { handle_title_choice(self); }
// The box that says the MODS menus aren't showing: its one item (OK) only closes it.
void fn_call_info(PopupFn*) {}
void* g_info_fn_vt[7] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_call_info),
                         reinterpret_cast<void*>(&fn_type), reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_dtor),
                         reinterpret_cast<void*>(&fn_get)};
void* g_title_fn_vt[7] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_call_title),
                          reinterpret_cast<void*>(&fn_type), reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_dtor),
                          reinterpret_cast<void*>(&fn_get)};
// A mod's message box (API 10): a chosen item, and B.
void handle_box_choice(PopupFn* self);
void handle_box_back(PopupFn* self);
void fn_call_box(PopupFn* self) { handle_box_choice(self); }
void fn_call_box_back(PopupFn* self) { handle_box_back(self); }
void* g_box_fn_vt[7] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_call_box),
                        reinterpret_cast<void*>(&fn_type), reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_dtor),
                        reinterpret_cast<void*>(&fn_get)};
void* g_box_back_fn_vt[7] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_call_box_back),
                             reinterpret_cast<void*>(&fn_type), reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_dtor),
                             reinterpret_cast<void*>(&fn_get)};

using MakeItemsFn     = void** (*)(void** out);
using MakeItemFn      = void** (*)(void** out, const int* id, void** label);
using ShowPopupFn     = uint8_t* (*)(void* page, void** title, void** message, void* items);
using PopupHelpFn     = void (*)(void* popup, int layout);
using PopupHandlerFn  = void (*)(void* popup, int event, StdFunction* fn, bool);
using PopupSelectedFn = uint8_t* (*)(void* popup);
using ObjectFreeFn    = void (*)(void* obj);
using AddItemFn       = void (*)(void* items, int index, void* item);

enum { kRestartNow = 0, kRestartLater = 1, kGoBack = 2 };
const wchar_t* const kTitle   = L"Restart needed";
const wchar_t* const kMessage = L"Some of the mod settings you changed take effect the next time you start the game. "
                                L"Restarting now closes the game the way Azimuth's Quit does, so unsaved progress will be lost.";
const wchar_t* const kChoices[] = {L"Restart now", L"Restart later", L"Go back"};

void release(uint8_t* obj) {
    if (obj && InterlockedDecrement(reinterpret_cast<volatile LONG*>(obj + 0x10)) == 0)
        reinterpret_cast<ObjectFreeFn>(g_base + RVA_OBJECT_FREE)(obj);
}

// The message box over `page`, with `labels` as its items (ids 0, 1, ...). `fn_vt` is the
// vtable of the handler it calls with the chosen item: g_fn_vt for the restart choices,
// g_title_fn_vt for a title-screen entry's list, g_box_fn_vt for a mod's box. `back_vt`, when
// given, is called for B (S6: events 3 to 5 all register on the box's B, key 45; registered
// after the help layout's own B, it takes its place, and the box still closes after it).
bool show_list_inner(void* page, FakeLocString* title, FakeLocString* message, FakeLocString* const* labels, int count,
                     void** fn_vt, void** back_vt, void** shown) {
    void* items = nullptr;
    reinterpret_cast<MakeItemsFn>(g_base + RVA_MAKE_ITEMS)(&items);
    if (!items) return false;
    auto* coll = static_cast<uint8_t*>(items);
    for (int i = 0; i < count; ++i) {
        void* item  = nullptr;
        void* label = labels[i];
        reinterpret_cast<MakeItemFn>(g_base + RVA_MAKE_ITEM)(&item, &i, &label);
        if (!item) continue;
        const int count = *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(coll + 0x50) + 0x30);
        void** vt       = *reinterpret_cast<void***>(coll);
        reinterpret_cast<AddItemFn>(vt[0x40 / 8])(coll, count, item);
        release(static_cast<uint8_t*>(item));
    }
    void* t        = title;
    void* m        = message;
    uint8_t* popup = reinterpret_cast<ShowPopupFn>(g_base + RVA_SHOW_POPUP)(page, &t, &m, coll + 0x38);
    if (!popup) {
        release(coll);
        return false;
    }
    reinterpret_cast<PopupHelpFn>(g_base + RVA_POPUP_HELP)(popup, 6);
    PopupFn fn{fn_vt, popup, page};
    StdFunction f{};
    f.impl = fn_copy(&fn, nullptr);   // on the heap; the callee deletes it
    reinterpret_cast<PopupHandlerFn>(g_base + RVA_POPUP_HANDLER)(popup, 2, &f, true);
    if (back_vt) {
        PopupFn back{back_vt, popup, page};
        StdFunction b{};
        b.impl = fn_copy(&back, nullptr);
        reinterpret_cast<PopupHandlerFn>(g_base + RVA_POPUP_HANDLER)(popup, 3, &b, true);
    }
    release(coll);
    if (shown) *shown = popup;
    return true;
}

// `shown`, when given, gets the game's box (its handlers' PopupFn::popup), to tell its answer from another box's.
bool show_list(void* page, FakeLocString* title, FakeLocString* message, FakeLocString* const* labels, int count, void** fn_vt,
               void** back_vt = nullptr, void** shown = nullptr) {
    __try {
        return show_list_inner(page, title, message, labels, count, fn_vt, back_vt, shown);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_popup_ok = false;
        return false;
    }
}

// When the game doesn't read the platform's menu files (Extra Options refusing its patches, EnableMediaOverrides=0,
// a layer that isn't mounted), the MODS tab, the Settings tile and the HUD notice are all missing, and only
// platform.log says why. The message box is the game's own (A_MessageBox.xaml, not one of ours), with its text
// from code, so it shows anyway.
const wchar_t* const kMissingTitle   = L"MODS menus missing";
const wchar_t* const kMissingMessage = L"The MODS tab and the mods' settings aren't showing, because the game isn't reading the platform's menu "
                                       L"files. If you use Extra Options, update it and keep EnableMediaOverrides=1. fh2-mods\\platform.log "
                                       L"has more.";
const wchar_t* const kMissingOk      = L"OK";
bool show_missing_box(void* page) {
    static FakeLocString* title    = loc_string(kMissingTitle);
    static FakeLocString* message  = loc_string(kMissingMessage);
    static FakeLocString* labels[] = {loc_string(kMissingOk)};
    return show_list(page, title, message, labels, 1, g_info_fn_vt);
}

bool show_popup(void* page) {
    static FakeLocString* title     = loc_string(kTitle);
    static FakeLocString* message   = loc_string(kMessage);
    static FakeLocString* labels[3] = {loc_string(kChoices[0]), loc_string(kChoices[1]), loc_string(kChoices[2])};
    return show_list(page, title, message, labels, 3, g_fn_vt);
}

int chosen_item(void* popup) {
    __try {
        uint8_t* item = reinterpret_cast<PopupSelectedFn>(g_base + RVA_POPUP_SELECTED)(popup);
        return item && readable(item + 0x68, 4) ? *reinterpret_cast<int*>(item + 0x68) : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- restarting the game ------------------------------------------------------------------
// "Restart now" writes fh2-mods\platform-restart.txt (this process's id, azimuth.exe's path,
// its arguments with --skip-launcher added, its folder), starts
// `rundll32 d3dcompiler_47.dll,RestartHelper`, and closes the game window as its X button
// would, so Azimuth shuts down the way it always does. The helper (below, outside the
// namespace) waits for this process to end and starts Azimuth again; if it hasn't ended
// within two minutes, the quit was cancelled and the helper gives up.
BOOL CALLBACK find_game_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

bool restart_game() {
    wchar_t exe[MAX_PATH], self[MAX_PATH];
    HMODULE me = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&restart_game), &me);
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || !me || !GetModuleFileNameW(me, self, MAX_PATH)) return false;
    const wchar_t* cl = GetCommandLineW();
    const bool quoted = *cl == L'"';
    if (quoted) ++cl;
    while (*cl && (quoted ? *cl != L'"' : *cl != L' ')) ++cl;
    if (*cl == L'"') ++cl;
    while (*cl == L' ') ++cl;
    std::wstring args = cl;
    if (args.find(L"--skip-launcher") == std::wstring::npos) args = L"--skip-launcher" + (args.empty() ? L"" : L" " + args);
    std::wstring dir = exe;
    dir.erase(dir.find_last_of(L"\\/"));
    if (FILE* f = _wfopen((azimuth_dir() / L"fh2-mods" / L"platform-restart.txt").c_str(), L"w, ccs=UTF-8")) {
        std::fwprintf(f, L"%lu\n%ls\n%ls\n%ls\n", GetCurrentProcessId(), exe, args.c_str(), dir.c_str());
        std::fclose(f);
    } else {
        return false;
    }
    std::wstring cmd = L"rundll32.exe \"" + std::wstring(self) + L"\",RestartHelper";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, dir.c_str(), &si, &pi)) {
        slog("couldn't start the restart helper (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    HWND w = nullptr;
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&w));
    if (!w) {
        slog("couldn't find the game window to close");
        return false;
    }
    slog("restarting the game: " + narrow(exe) + " " + narrow(args));
    PostMessageW(w, WM_CLOSE, 0, 0);
    return true;
}

void handle_choice(PopupFn* self) {
    switch (chosen_item(self->popup)) {
        case kRestartNow:
            slog("Restart now");
            if (!restart_game()) slog("the game wasn't restarted");
            break;
        case kRestartLater: slog("Restart later"); break;
        case kGoBack:
            slog("Go back");
            if (g_popup_back) g_popup_back(self->page);
            break;
        default: break;
    }
}

bool g_settings_tile = false;   // Pause > Settings has the Mods tile (some mod has rows)

void d_wire_tiles(void* page) {
    g_mods_asked        = 0;
    g_mods_page_next    = false;
    g_panel_obj         = nullptr;   // back on Settings: no page with the hint line is up
    g_last_hud_page     = nullptr;   // nor any page of the options class the platform wrote
    g_mods_options_page = nullptr;
    if (!g_settings_tile) return o_wire_tiles(page);
    static bool said_grid = false;
    if (!widen_nav(page) && !said_grid) {
        said_grid = true;
        slog("the Settings page's tile grid isn't 5 x 2 as expected, so the controller may not reach the MODS tile");
    }
    o_wire_tiles(page);
    wire_mods_tile(page);
    // the tile's second line, in the game's language now
    if (page && g_hub_tile_desc[kHubLineSettings] && g_hub_tile_obj[kHubLineSettings].load() == page)
        notify_changed(page, g_hub_tile_desc[kHubLineSettings]);
    // back through the Settings door with a change that needs a restart, not asked about yet?
    const bool from_mods = g_route == kRouteSettings;
    if (from_mods) g_route = kRouteNone;
    if (from_mods && g_popup_ok && restart_pending()) {
        std::string now = current_values();
        if (now != g_asked_values) {
            g_asked_values = now;
            g_popup_back   = &settings_go_back;
            if (show_popup(page)) slog("asked about restarting");
        }
    }
}

// ---- the MODS tab: its tiles ---------------------------------------------------------------
// The pause menu's tiles are wired in 0x140DC6E30 with 0x140D9F9F0(page, "PART_Button_X",
// &pmf). The Marketplace ones each fire a state-machine event (car_packs, expansion_packs) or
// open the store. All of ours go through the Car Packs handler (0x140DBD720), whose car_packs
// event pause.xml now sends to a node that shows the HUD page; the scene record hook then
// points it at the page the tile asked for. The 2x2 tile was a live store tile wired by the
// game; ours is a plain button, wired here.
constexpr uintptr_t RVA_PAUSE_MENU_WIRE = 0x0DC6E30;   // PauseMenu: wire the tiles (page)
constexpr uintptr_t RVA_PAUSE_WIRE_BTN  = 0x0D9F9F0;   // (page, const char* name, const Pmf*)
constexpr uintptr_t RVA_TILE_CAR_PACKS  = 0x0DBD720;
constexpr uintptr_t RVA_TILE_EXPANSIONS = 0x0DBDD00;
constexpr uintptr_t RVA_TILE_EXTRAS     = 0x0DBDD60;
constexpr uintptr_t RVA_TILE_VIP        = 0x0DBFE40;
// 0x140DD8FD0(page, "PART_GROUP_Marketplace", 1) hides the tab; Extra Options' EnableMarketplace
// makes these 0, and so does the platform.
constexpr uintptr_t RVA_MARKETPLACE_HIDE[] = {0x0DDACAD, 0x0DDAE1F, 0x0DDB2F5, 0x0DDB910, 0x0DDBB3E};

WireTilesFn o_pause_menu_wire = nullptr;
TileFn o_car_packs = nullptr, o_expansions = nullptr, o_extras = nullptr, o_vip = nullptr;

// Before one of the MODS tab's pages opens, from either door: up to date, and asked for. `resume`: YOUR MODS
// opens on the mod's page and row the player last left (Go back).
void prepare_hub(int which, bool resume) {
    // the mods' pages on it get to set their rows first
    if (which < kHubTilePage) g_custom_shown = -1;   // no page of a mod's is up on its own (the hint line reads it)
    g_panel_obj = nullptr;   // the page that had the hint line is going: the new page's bindings set it again if it has one
    if (which == kHubInstalled) {
        mark_home();
        pages_opening();
    } else if (which >= kHubTilePage && static_cast<std::size_t>(which - kHubTilePage) < kTileCount) {
        int j = -1;
        {
            std::scoped_lock lk{g_mu};
            j = g_tile_page[which - kHubTilePage];
            if (j >= 0 && g_pages[j].custom)
                for (auto& slot : g_pages[j].slots) slot = 0;   // slots start at 0, before on_open
        }
        g_custom_obj   = nullptr;
        g_custom_shown = j;
        for (auto& obj : g_tile_page_obj) obj = nullptr;   // the last one is gone
        if (j >= 0) pages_opening(j);
        // the crash guard (page_guard.cpp): if the game goes down loading the mod's XAML, the next session leaves it out
        FH2Mod* mod = nullptr;
        std::string key;
        {
            std::scoped_lock lk{g_mu};
            if (j >= 0 && g_pages[j].custom && !left_out(g_pages[j])) mod = g_pages[j].mod, key = g_pages[j].key;
        }
        if (mod) page_guard_opening(mod, key);
    }
    write_hub_page(which);   // up to date (update checks finish after the game starts)
    g_view_back = false;
    g_resume    = resume && which == kHubInstalled && g_last_view > 0 && view_kind(g_last_view) == kViewMod;
    if (!g_resume) g_last_view = 0;
    g_view = g_resume ? g_last_view : 0;   // before the page's bindings first read it
    hub_ask(which);
}

// A MODS tab tile: its page, through the Car Packs handler (see pause.xml), so B comes back to the tab.
void open_hub_page(void* page, int which) {
    prepare_hub(which, std::exchange(g_go_back, false));
    g_route      = kRouteTab;
    g_route_page = which;
    o_car_packs(page);
}
void tab_go_back(void* page) {
    g_go_back = true;
    open_hub_page(page, g_route_page);
}
// The menus' sounds, played as the HUD page plays SubMenuOpen when it's shown (0x140C7AD40):
// a std::string of the sound's name, made with the game's own assign (0x14022C300) and handed
// by value to the UI sound player (the object at game+0x365A298, its vtable +0x08), which
// frees it. Our view switches and the tiles that open the browser play them themselves.
constexpr uintptr_t RVA_UI_SOUNDS      = 0x365A298;
constexpr uintptr_t RVA_STRING_ASSIGN  = 0x022C300;   // std::string::assign(this, const char*, size_t)
bool g_sounds_ok = false;                             // the code above looks as expected
const char* const kSoundOpen   = "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_SubMenuOpen_01";
const char* const kSoundClose  = "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_SubMenuClose_01";
const char* const kSoundSelect = "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_Select_01";

struct GameString {   // MSVC std::string
    char buf[16];
    uint64_t size, cap;
};

// The page's B (0x140CEC3D0) also plays PopUpMenu_Back, through another player (the object at
// game+0x3684B90, got with 0x141412FE0), the same way.
constexpr uintptr_t RVA_BACK_SOUNDS = 0x3684B90;
constexpr uintptr_t RVA_BACK_SOUND_CODE = 0x0CEC418;   // checked: the page's B playing it
const char* const kSoundBack = "/Master/UI/PopUpMenu/HZ2_SFX_HUD_PopUpMenu_Back_01";

// `base`: the game's (settings.cpp's g_base, or play_ui_sound's own, which doesn't wait for the MODS tab's hooks).
// False when the player isn't up yet.
bool play_sound_at(uintptr_t base, const char* name, uintptr_t player_rva) {
    void* player = *reinterpret_cast<void**>(base + player_rva);
    if (!player) return false;
    GameString str{};
    str.cap = 15;
    reinterpret_cast<void (*)(GameString*, const char*, std::size_t)>(base + RVA_STRING_ASSIGN)(&str, name, std::strlen(name));
    void** vt = *reinterpret_cast<void***>(player);
    reinterpret_cast<void (*)(void*, GameString*)>(vt[1])(player, &str);
    return true;
}
void play_sound_inner(const char* name, uintptr_t player_rva) { play_sound_at(g_base, name, player_rva); }

void play_sound(const char* name, uintptr_t player_rva = RVA_UI_SOUNDS) {
    if (!g_sounds_ok) return;
    __try {
        play_sound_inner(name, player_rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Installed mods holds every view at once: the list (the page's OptionsList) and one list per
// mod, each collapsed unless its FH2Mod_View_<k> is true. A on the list shows the mod's view
// and focuses its first row; B there comes back to the list. The page's own A and B (Accept,
// Cancel) would leave the page, so on it they're ours. Focusing is what the page does with
// "FirstItem" on showing (0x140C7A730): FindName (0x140C63120) on an AVUI string
// (0x141663220, freed with 0x1416650A0), then 0x1414D01D0 on the element's +0xB8.
constexpr uintptr_t RVA_HUD_PAGE_LOADED  = 0x0C7AD40;
constexpr uintptr_t RVA_HUD_PAGE_VT_SLOT = 0x2DF06C0;   // HudOptions page vtable +0x488
constexpr uintptr_t RVA_FOCUS_FIRST      = 0x0C7A730;   // checked: it's the code this copies
constexpr uintptr_t RVA_AVUI_STRING      = 0x1663220;   // (String* out, const char*, int length or -1)
constexpr uintptr_t RVA_AVUI_STRING_FREE = 0x16650A0;   // (String*)
constexpr uintptr_t RVA_FIND_NAME        = 0x0C63120;   // Element* (String** name, page)
constexpr uintptr_t RVA_FOCUS            = 0x14D01D0;   // (Ptr* out, IInputElement*)
using AvuiStringFn     = void (*)(void* out, const char* text, int length);
using AvuiStringFreeFn = void (*)(void* s);
using FindNameFn       = uint8_t* (*)(void** name, void* page);
using FocusFn          = void (*)(void** out, void* element);
TileFn o_hud_page_loaded = nullptr;

bool focus_named_inner(void* page, const char* name) {
    alignas(16) uint8_t str[0x80] = {};
    reinterpret_cast<AvuiStringFn>(g_base + RVA_AVUI_STRING)(str, name, -1);
    void* ref = str;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(str + 0x10));
    uint8_t* el = reinterpret_cast<FindNameFn>(g_base + RVA_FIND_NAME)(&ref, page);
    reinterpret_cast<AvuiStringFreeFn>(g_base + RVA_AVUI_STRING_FREE)(str);
    if (!el) return false;
    void** iface = reinterpret_cast<void**>(el + 0xB8);
    reinterpret_cast<void (*)(void*)>((*reinterpret_cast<void***>(iface))[3])(iface);
    void* out[4] = {};
    reinterpret_cast<FocusFn>(g_base + RVA_FOCUS)(out, iface);
    release(static_cast<uint8_t*>(out[1]));
    return true;
}

// Whether the pause menu has a tile called `name`, looked up the way the game finds its tiles to wire them
// (0x140CBDB20, Button* (String** name, page), called at 0x140D9FA3A): 1 yes, 0 no, -1 it faulted.
constexpr uintptr_t RVA_FIND_BUTTON      = 0x0CBDB20;
constexpr uintptr_t RVA_FIND_BUTTON_CALL = 0x0D9FA3A;   // in the pause menu's wire-a-tile (0x140D9F9F0)
std::atomic<bool> g_find_button_ok{false};              // both look as expected (set once the hooks are on)
bool find_button_inner(void* page, const char* name) {
    alignas(16) uint8_t str[0x80] = {};
    reinterpret_cast<AvuiStringFn>(g_base + RVA_AVUI_STRING)(str, name, -1);
    void* ref = str;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(str + 0x10));
    uint8_t* el = reinterpret_cast<FindNameFn>(g_base + RVA_FIND_BUTTON)(&ref, page);
    reinterpret_cast<AvuiStringFreeFn>(g_base + RVA_AVUI_STRING_FREE)(str);
    return el != nullptr;
}
int page_has_button(void* page, const char* name) {
    __try {
        return find_button_inner(page, name) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool focus_named(void* page, const char* name) {
    __try {
        return focus_named_inner(page, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void notify_view(int k) {
    void* obj = g_view_obj.load();
    if (!obj || k < 0 || k >= kMaxViews) return;
    for (uint8_t* d : {g_view_desc[k], g_view_in_desc[k], g_view_back_desc[k], g_help_bar_desc[k]})
        if (d) notify_changed(obj, d);
}

std::string g_focus_again;   // focus this once more when the button comes up (UI thread only)

// Shows view k of Installed mods, forwards (A) or back (B), with the focus on its row `row`.
void switch_view(void* page, int k, bool back, int row) {
    const int old = g_view.exchange(k);
    if (old == k) return;
    play_sound(back ? kSoundClose : kSoundOpen);
    g_view_back = back;
    std::string name;
    if (k == 0) {
        // back on the mod's own row in the list; Go back comes back to the page and row left
        if (view_kind(old) == kViewMod) {
            g_last_view = old;
            g_last_row  = g_view_selected.load();
        }
        g_view_selected = 0;
        // view k's row is button k - 1, and the list's first row is FirstItem; but with no mods installed the first
        // row is "No mods installed", and the platform's line (view 1) is the second, as button 0
        name = old <= 1 && old != g_platform_view.load() ? "FirstItem" : "PART_Button_FH2Mod_" + std::to_string(old - 1);
    } else {
        const std::size_t v = static_cast<std::size_t>(k - 1);
        if (v >= g_view_acts.size() || row < 0 || static_cast<std::size_t>(row) >= g_view_acts[v].size()) row = 0;
        g_view_selected = row;   // what A acts on: focusing a row that's already selected doesn't say so
        name = v < g_view_acts.size() && !g_view_acts[v].empty() && !g_view_acts[v][row].name.empty() ? g_view_acts[v][row].name
                                                                                                    : "FH2Mod_First_" + std::to_string(k);
    }
    notify_view(old);
    notify_view(k);
    if (void* obj = g_view_obj.load())
        for (uint8_t* d : g_bar_desc)
            if (d) notify_changed(obj, d);
    notify_panel(nullptr);   // the hint, the legend or nothing
    if (!focus_named(page, name.c_str())) slog("couldn't focus " + name);
    g_focus_again = name;   // the list has only just been shown; by the button's release it's laid out
    if (k == 0) {
        slog("back to the list");
    } else if (static_cast<std::size_t>(k) < g_view_page.size() && g_view_page[k] >= 0) {
        std::scoped_lock lk{g_mu};
        const ModPage& pg = g_pages[g_view_page[k]];
        slog("showing " + pg.mod->name + "'s page " + pg.key);
    } else if (k == g_platform_view.load()) {
        slog("showing the platform's own page");
    } else if (static_cast<std::size_t>(k) <= g_entries.size()) {
        slog("showing " + g_entries[k - 1].name + "'s page");
        mark_page_opened(static_cast<std::size_t>(k - 1));
    }
}

// Go back from the restart popup: YOUR MODS has just loaded showing the mod's page the player left; the focus
// goes to the row they were on. If it can't, the list shows, as when YOUR MODS opens.
void resume_view(void* page) {
    const int k = g_view.load();
    const std::size_t v = static_cast<std::size_t>(k - 1);
    if (k <= 0 || v >= g_view_acts.size() || g_view_acts[v].empty()) return;
    const int row = g_last_row >= 0 && static_cast<std::size_t>(g_last_row) < g_view_acts[v].size() ? g_last_row : view_focus(k);
    const std::string name = !g_view_acts[v][row].name.empty() ? g_view_acts[v][row].name : "FH2Mod_First_" + std::to_string(k);
    g_view_selected = row;
    if (focus_named(page, name.c_str())) {
        g_focus_again = name;   // once more on the button's release, as switch_view does
        slog("back on the page the player left");
        return;
    }
    slog("couldn't focus " + name + "; back to the list");
    g_view = 0;
    notify_view(k);
    notify_view(0);
    if (void* obj = g_view_obj.load())
        for (uint8_t* d : g_bar_desc)
            if (d) notify_changed(obj, d);
    notify_panel(nullptr);
    focus_named(page, "FirstItem");
}

// A on a row of a mod's page that has a key: the mod's on_select. `page`: the game's page it's on.
void select_on_page(int j, const std::string& row, void* page) {
    FH2Mod* mod = nullptr;
    FH2PageSelectFn fn = nullptr;
    void* user = nullptr;
    std::string key;
    {
        std::scoped_lock lk{g_mu};
        if (j < 0 || static_cast<std::size_t>(j) >= g_pages.size()) return;
        mod  = g_pages[j].mod;
        fn   = g_pages[j].on_select;
        user = g_pages[j].user;
        key  = g_pages[j].key;
    }
    if (!fn) return;
    play_sound(kSoundSelect);
    log_line(mod->name, "A on " + row + " on page " + key);
    UiCallback cb{mod, UiCb::page, page, j};
    ui_note_callback(mod);
    if (!call_on_select(fn, user, key.c_str(), row.c_str())) crashed_in(mod, "on_select for page " + key);
}

// Which HUD page objects are Installed mods and Updates (set when one loads; only compared).
std::atomic<void*> g_installed_page{nullptr};
std::atomic<void*> g_updates_page{nullptr};
std::atomic<void*> g_about_page{nullptr};
std::atomic<void*> g_help_page{nullptr};

// Updates and About: A opens a row's page in the browser, or a folder in Explorer (shell32 only
// when asked).
void open_url(const std::string& url) {
    std::thread([url] {
        using ShellExecuteFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
        HMODULE shell = LoadLibraryW(L"shell32.dll");
        auto fn       = shell ? reinterpret_cast<ShellExecuteFn>(GetProcAddress(shell, "ShellExecuteW")) : nullptr;
        if (!fn || reinterpret_cast<INT_PTR>(fn(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            slog("couldn't open " + url);
        else
            slog("opened " + url);
    }).detach();
}


void show_pending_box(void* page, bool tile_page);   // with the mods' message boxes, below

void d_hud_page_loaded(void* page) {
    // Which page this is, before the page's own "loaded" (which focuses FirstItem, and so may set
    // a slot through a binding) runs: from then on, callbacks and focusing go to this page.
    const int was_shown = g_hub_shown.exchange(0);
    const bool mods_page = g_mods_page_next.exchange(false) && was_shown == 0;
    g_last_hud_page     = page;
    if (mods_page) g_mods_options_page = page;
    else if (g_mods_options_page.load() == page) g_mods_options_page = nullptr;
    if (was_shown == kHubUpdates) {
        g_updates_page = page;
        g_selected     = 0;
    } else if (g_updates_page.load() == page) {
        g_updates_page = nullptr;
    }
    if (was_shown == kHubAbout) {
        g_about_page = page;
        g_selected   = 0;
    } else if (g_about_page.load() == page) {
        g_about_page = nullptr;
    }
    if (was_shown == kHubHelp) g_help_page = page;
    else if (g_help_page.load() == page) g_help_page = nullptr;
    if (was_shown == kHubInstalled) {
        g_installed_page = page;
        g_selected       = 0;
        g_view_back      = false;   // g_view is as prepare_hub set it: the list, or the mod's page for Go back
    } else if (g_installed_page.load() == page) {
        g_installed_page = nullptr;
    }
    bool tile_page = false;
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (was_shown == kHubTilePage + static_cast<int>(t)) {
            g_tile_page_obj[t] = page;
            g_selected         = 0;
            tile_page          = true;
        } else if (g_tile_page_obj[t].load() == page) {
            g_tile_page_obj[t] = nullptr;
        }
    if (!tile_page) {   // another page: a page of a mod's own design isn't up
        g_custom_obj   = nullptr;
        g_custom_shown = -1;
    }
    o_hud_page_loaded(page);
    // the crash guard: a page of a mod's own design has loaded (its mark goes once it's been up 10 s), or the player has
    // gone on to another page
    if (tile_page) page_guard_loaded();
    else page_guard_closed();
    if (was_shown == kHubInstalled && std::exchange(g_resume, false)) resume_view(page);
    show_pending_box(page, tile_page);   // a box asked for from the page's on_open, now that it's there
}



// A page's buttons: vtable +0x578, (page, button, pressed) -> handled. The page gets A as 18
// and B as 45 (seen in the log); on the options pages both leave the page.
constexpr uintptr_t RVA_HUD_PAGE_BUTTON_SLOT = 0x2DF07B0;   // HudOptions page vtable +0x578
constexpr uintptr_t RVA_PAGE_BUTTON          = 0x0CEC3D0;   // the shared implementation
constexpr int kButtonA                       = 18;
constexpr int kButtonB                       = 45;
using PageButtonFn = bool (*)(void* page, int button, bool pressed);
PageButtonFn o_hud_page_button = nullptr;

bool call_on_button(FH2PageButtonFn fn, void* user, const char* key, int button, int* result) {
    __try {
        *result = fn(user, key, button);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A page of a mod's own design on tile t: A, B, X, Y, LB and RB go to its on_button first. Other
// buttons are logged once each, and go on as usual.
//
// The menus get the pad as keys (a table at 0x142254390 turns the game's controls into AVUI's
// keys, WPF's numbering): A is Space (18), B is B (45), X is X (67), Y is Y (68), the d-pad the
// arrows (23-26) and the number pad's 8/2/4/6 together, LB PageUp (19) and RB PageDown (20) (seen
// on a pad in game). The table's other pairs, the shifts (116, 117) and the controls (118, 119),
// aren't the bumpers.
bool g_custom_handled[7] = {};   // per FH2_BUTTON_: the press was handled, so its release is too
std::set<int> g_buttons_seen;

int pad_button(int key) {
    switch (key) {
        case 18: return FH2_BUTTON_A;
        case 45: return FH2_BUTTON_B;
        case 67: return FH2_BUTTON_X;
        case 68: return FH2_BUTTON_Y;
        case 19: return FH2_BUTTON_LB;
        case 20: return FH2_BUTTON_RB;
        default: return 0;
    }
}

bool custom_page_button(void* page, int j, int button, bool pressed) {
    const int code = pad_button(button);
    if (!code) {
        const bool dpad = (button >= 23 && button <= 26) || button == 76 || button == 78 || button == 80 || button == 82;
        if (pressed && !dpad && g_buttons_seen.insert(button).second) slog("button " + std::to_string(button) + " on a mod's page");
        return o_hud_page_button(page, button, pressed);
    }
    if (pressed) {
        FH2Mod* mod = nullptr;
        FH2PageButtonFn fn = nullptr;
        void* user = nullptr;
        std::string key;
        {
            std::scoped_lock lk{g_mu};
            mod  = g_pages[j].mod;
            fn   = g_pages[j].on_button;
            user = g_pages[j].user;
            key  = g_pages[j].key;
        }
        int r = FH2_PAGE_IGNORED;
        g_focus_again.clear();   // a press of its own: nothing left over to focus on its release
        if (fn) {
            InCallback in{page, -1};
            UiCallback cb{mod, UiCb::page, page, j};
            ui_note_callback(mod);
            if (!call_on_button(fn, user, key.c_str(), code, &r)) {
                crashed_in(mod, "on_button for page " + key);
                r = FH2_PAGE_HANDLED;
            }
        }
        g_custom_handled[code] = r != FH2_PAGE_IGNORED;
        if (r == FH2_PAGE_HANDLED_SELECT) play_sound(kSoundSelect);
        else if (r == FH2_PAGE_HANDLED_BACK) play_sound(kSoundBack, RVA_BACK_SOUNDS);
    } else if (!g_focus_again.empty()) {
        focus_named(page, g_focus_again.c_str());   // the element may only just have been shown
        g_focus_again.clear();
    }
    if (g_custom_handled[code] || code == FH2_BUTTON_A) return true;   // A never leaves the page
    return o_hud_page_button(page, button, pressed);
}

bool d_hud_page_button(void* page, int button, bool pressed) {
    if (page && page == g_help_page.load() && button == kButtonA) return true;   // information only: A doesn't leave it
    if (page && (page == g_updates_page.load() || page == g_about_page.load()) && button == kButtonA) {
        const std::vector<std::string>& urls = page == g_about_page.load() ? g_about_urls : g_update_urls;
        const int n = g_selected.load();
        if (pressed && n >= 0 && static_cast<std::size_t>(n) < urls.size() && !urls[n].empty()) {
            play_sound(kSoundSelect);
            open_url(urls[n]);   // a page, or the mods folder (Explorer opens it)
        }
        return true;   // not Accept (it would leave the page)
    }
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (page && page == g_tile_page_obj[t].load()) {
            int j      = -1;
            bool custom = false;
            {
                std::scoped_lock lk{g_mu};
                j      = g_tile_page[t];
                custom = j >= 0 && g_pages[j].custom && !left_out(g_pages[j]);
            }
            if (custom) return custom_page_button(page, j, button, pressed);
        }
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (page && page == g_tile_page_obj[t].load() && button == kButtonA) {
            // a mod's page on a tile: A tells the mod about a row with a key, and stays
            const int n = g_selected.load();
            if (pressed && n >= 0 && static_cast<std::size_t>(n) < g_tile_acts[t].size() && !g_tile_acts[t][n].select.empty()) {
                int j = -1;
                {
                    std::scoped_lock lk{g_mu};
                    j = g_tile_page[t];
                }
                select_on_page(j, g_tile_acts[t][n].select, page);
            }
            return true;
        }
    if (page && page == g_installed_page.load() && (button == kButtonA || button == kButtonB)) {
        if (!pressed && !g_focus_again.empty()) {
            focus_named(page, g_focus_again.c_str());
            g_focus_again.clear();
        }
        const int view = g_view.load();
        if (view == 0 && button == kButtonB) return o_hud_page_button(page, button, pressed);   // leave the page
        const int kind = view_kind(view);
        const bool on_page = kind == kViewPage || kind == kViewPageSelect;
        const RowAct* act = nullptr;
        if (view != 0) {
            const int n = g_view_selected.load();
            const std::size_t v = static_cast<std::size_t>(view - 1);
            if (v < g_view_acts.size() && n >= 0 && static_cast<std::size_t>(n) < g_view_acts[v].size()) act = &g_view_acts[v][n];
        }
        if (button == kButtonA && act) {
            if (!act->url.empty()) {   // a link row: opens it, and stays
                if (pressed) {
                    play_sound(kSoundSelect);
                    open_url(act->url);
                }
                return true;
            }
            if (act->view > 0) {   // a page's row: shows the page
                if (pressed) {
                    play_sound(kSoundSelect);
                    switch_view(page, act->view, false, 0);
                }
                return true;
            }
            if (on_page && !act->select.empty()) {   // tells the page's mod
                if (pressed && static_cast<std::size_t>(view) < g_view_page.size()) select_on_page(g_view_page[view], act->select, page);
                return true;
            }
            if (!on_page && !act->select.empty() && view == g_platform_view.load()) {   // an action on the platform's page
                if (pressed && player_select(act->select)) play_sound(kSoundSelect);
                return true;
            }
        }
        if (on_page) {
            // B: back to the mod's settings, on the page's row; A does nothing else here
            if (button == kButtonB && pressed) {
                play_sound(kSoundBack, RVA_BACK_SOUNDS);
                const int owner = static_cast<std::size_t>(view) < g_view_owner.size() ? g_view_owner[view] : 0;
                int row         = 0;
                if (owner > 0 && static_cast<std::size_t>(owner - 1) < g_view_acts.size())
                    for (std::size_t r = 0; r < g_view_acts[owner - 1].size(); ++r)
                        if (g_view_acts[owner - 1][r].view == view) row = static_cast<int>(r);
                switch_view(page, owner, true, row);
            }
            return true;
        }
        // On a mod's page A never leaves it: left and right change a row, and a change is saved as it's made.
        if (view != 0 && button == kButtonA) return true;
        if (pressed) {
            if (view == 0) {   // A: the mod's page, on its first setting
                const int n = g_selected.load();
                if (n >= 0 && static_cast<std::size_t>(n) < g_entries.size() && n < kMaxModViews) {
                    play_sound(kSoundSelect);   // A selects, as on the tiles
                    switch_view(page, n + 1, false, view_focus(n + 1));
                } else if (n >= 0 && n == g_platform_list_row.load() && g_platform_view.load() > 0) {   // the platform's page
                    play_sound(kSoundSelect);
                    switch_view(page, g_platform_view, false, view_focus(g_platform_view));
                }
            } else {   // B: back to the list, on the mod's row (the game's back, with the swoosh)
                play_sound(kSoundBack, RVA_BACK_SOUNDS);
                switch_view(page, 0, true, 0);
            }
        }
        return true;
    }
    return o_hud_page_button(page, button, pressed);
}

void d_tile_updates(void* page) { open_hub_page(page, kHubUpdates); }
// GET MODS opens the mod site in the browser rather than a page of ours
constexpr const char* kModSite = "https://fh2mods.refil.cc/mods";
void d_tile_get_mods(void*) {
    play_sound(kSoundSelect);
    open_url(kModSite);
}
void d_tile_about(void* page) { open_hub_page(page, kHubAbout); }
void d_tile_help(void* page) { open_hub_page(page, kHubHelp); }
void tile_installed(void* page) { open_hub_page(page, kHubInstalled); }

void wire_installed_tile(void* page) {
    static const Pmf pmf{reinterpret_cast<void*>(&tile_installed), 0, 0};
    __try {
        reinterpret_cast<WireButtonFn>(g_base + RVA_PAUSE_WIRE_BTN)(page, "PART_Button_FH2Mod_Installed", &pmf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- the game's tiles that mods' pages take over (API 8) ----------------------------------
// See kTiles. Each CALL to 0x140CB47B0(page, ObjectPtr* out, int command) -> out goes to a
// detour of ours (one per call site, for its `original`) that opens the page the way the MODS
// tab's tiles do, through the Car Packs handler; the pause menu it's on is the same page.
using UiCommandFn = void** (*)(void* page, void** out, int command);
UiCommandFn o_tile_command[kTileCount][2] = {};

template <int T, int S>
void** d_tile_command(void* page, void** out, int command) {
    int j = -1;
    {
        std::scoped_lock lk{g_mu};
        j = g_tile_page[T];
    }
    if (j < 0 || !o_car_packs || command != 0xFE) return o_tile_command[T][S](page, out, command);
    if (out) *out = nullptr;   // the caller releases what it gets back
    slog(std::string("the ") + kTiles[T].name + " tile: opening a mod's page");
    open_hub_page(page, kHubTilePage + T);
    return out;
}
void* const kTileDetours[kTileCount][2] = {
    {reinterpret_cast<void*>(&d_tile_command<0, 0>), reinterpret_cast<void*>(&d_tile_command<0, 1>)},
};

// Hooks the calls of the tiles that have a page. Where the code isn't as expected, the tile
// keeps doing what it did, and the page is only under Installed mods.
void hook_tiles(FH2Mod* m) {
    for (std::size_t t = 0; t < kTileCount; ++t) {
        {
            std::scoped_lock lk{g_mu};
            if (g_tile_page[t] < 0) continue;
        }
        // mov r8d, 0xFE; mov rcx, rbx; call 0x140CB47B0
        bool ok = true;
        for (uintptr_t call : kTiles[t].calls) {
            const int32_t rel = static_cast<int32_t>(0x0CB47B0 - (call + 5));
            uint8_t expect[14] = {0x41, 0xB8, 0xFE, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0xE8};
            std::memcpy(expect + 10, &rel, 4);
            ok = ok && code_matches(call - 9, expect, sizeof expect);
        }
        std::vector<FH2Hook*> made;
        for (int c = 0; c < 2 && ok; ++c) {
            const std::string name = std::string(kTiles[t].name) + " tile" + (c ? " (PORSCHE tab)" : "");
            FH2Hook* h = g_api->hook_call(m, name.c_str(), reinterpret_cast<void*>(g_base + kTiles[t].calls[c]), kTileDetours[t][c],
                                          reinterpret_cast<void**>(&o_tile_command[t][c]));
            ok = h != nullptr;
            if (h) made.push_back(h);
        }
        for (FH2Hook* h : made) ok = ok && g_api->enable_hooks(m, h);
        if (!ok) {
            for (FH2Hook* h : made) g_api->disable_hooks(m, h);
            slog(std::string("the ") + kTiles[t].name + " tile's code doesn't look as expected; its page is only on its mod's page under YOUR MODS");
            std::scoped_lock lk{g_mu};
            g_tile_page[t] = -1;
            continue;
        }
        slog(std::string("the ") + kTiles[t].name + " tile opens a mod's page");
    }
}

void hook_hud_shown(FH2Mod* m);
bool g_hud_notice = false;   // A_Hud.xaml has the notice
void d_pause_menu_wire(void* page) {
    g_custom_obj        = nullptr;   // back on the pause menu: no page of a mod's own design is up
    g_custom_shown      = -1;
    g_panel_obj         = nullptr;   // nor one with the hint line
    g_last_hud_page     = nullptr;   // nor any page of the options class the platform wrote
    g_mods_options_page = nullptr;
    page_guard_closed();
    for (auto& obj : g_tile_page_obj) obj = nullptr;
    g_focus_again.clear();
    g_hub_asked  = 0;
    g_hub_builds = 0;
    o_pause_menu_wire(page);
    wire_installed_tile(page);
    // the MODS tab's tiles aren't there: the game isn't reading the platform's files. Said once a session.
    static bool looked = false;
    if (!looked && page && g_find_button_ok && g_popup_ok) {
        looked = true;
        const int has = page_has_button(page, "PART_Button_FH2Mod_Installed");
        if (has < 0) {
            slog("couldn't look for the MODS tab's tiles on the pause menu");
        } else if (!has) {
            slog("the pause menu has no MODS tab: the game isn't reading the platform's menu files (for the mediaoverride folder, "
                 "Extra Options needs EnableMediaOverrides=1 and its patches on). Telling the player once this session");
            if (show_missing_box(page)) return;   // no restart question under it
        }
    }
    // a page's tile text may have changed since this page's bindings read it
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (page && g_tile_obj[t].load() == page)
            for (int prop = 0; prop < kTileProps; ++prop)
                if (g_tile_desc[prop][t]) notify_changed(page, g_tile_desc[prop][t]);
    // and the MODS tab's own tiles' names and lines: the game's language, the number of updates; and the badges
    for (int k = 0; k < kHubTexts; ++k)
        if (page && g_hub_tile_desc[k] && g_hub_tile_obj[k].load() == page) notify_changed(page, g_hub_tile_desc[k]);
    if (page && g_badge_obj.load() == page)
        for (uint8_t* d : g_badge_descs)
            if (d) notify_changed(page, d);
    // back from the MODS tab with a change that needs a restart?
    const bool from_hub = g_route == kRouteTab;
    if (from_hub) g_route = kRouteNone;
    if (from_hub && g_popup_ok && restart_pending()) {
        std::string now = current_values();
        if (now != g_asked_values) {
            g_asked_values = now;
            g_popup_back   = &tab_go_back;   // the page (and row) the player left
            if (show_popup(page)) slog("asked about restarting");
        }
    }
}

// The pause menu hides the Marketplace tab (the MODS tab's place) with five "mov r8d, 1"s; each
// becomes "mov r8d, 0", as Extra Options' EnableMarketplace does. Byte patches through the hook
// system (API 7), so they're checked, logged and listed in crash reports like hooks; where
// Extra Options has already done it, there's nothing to do.
bool show_marketplace_tab(FH2Mod* m) {
    static const uint8_t hide[] = {0x41, 0xB8, 0x01, 0x00, 0x00, 0x00}, show[] = {0x41, 0xB8, 0x00, 0x00, 0x00, 0x00};
    for (uintptr_t rva : RVA_MARKETPLACE_HIDE)
        if (!code_matches(rva, hide, sizeof hide) && !code_matches(rva, show, sizeof show)) return false;
    bool ok = true;
    for (uintptr_t rva : RVA_MARKETPLACE_HIDE) {
        if (code_matches(rva, show, sizeof show)) continue;   // Extra Options' EnableMarketplace
        FH2Hook* p = g_api->patch_bytes(m, "Marketplace tab: show", reinterpret_cast<void*>(g_base + rva), hide, show, sizeof show);
        ok = p && g_api->enable_hooks(m, p) && ok;
    }
    return ok;
}

bool g_hub_files = false;   // the tab's files were written

void hook_hub() {
    struct Check {
        uintptr_t rva;
        std::vector<uint8_t> bytes;
    };
    const Check checks[] = {
        {RVA_PAUSE_MENU_WIRE, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC}},
        {RVA_PAUSE_WIRE_BTN, {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x40, 0xA0, 0xFE}},
        {RVA_TILE_CAR_PACKS, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x72, 0x82, 0x4D, 0xFF, 0x80, 0xB8}},
        {RVA_TILE_EXPANSIONS, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0x92, 0x7C, 0x4D, 0xFF, 0x80, 0xB8}},
        {RVA_TILE_EXTRAS, {0x40, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x45, 0xB0, 0xFE}},
        {RVA_TILE_VIP, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48}},
        // the HUD page's "loaded", which plays SubMenuOpen (play_sound does the same)
        {RVA_HUD_PAGE_LOADED, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xE8, 0x35, 0xE0, 0x07, 0x00, 0x48, 0x8B, 0x15, 0x7E, 0x42, 0xFE, 0x01, 0x48, 0x8B, 0x1D, 0x3F, 0xF5, 0x9D, 0x02, 0x45, 0x33, 0xC0, 0x48, 0xC7, 0x44, 0x24, 0x38, 0x0F, 0x00, 0x00, 0x00, 0x4C, 0x89, 0x44, 0x24, 0x30, 0x44, 0x88, 0x44, 0x24, 0x20, 0x44, 0x38, 0x02, 0x74, 0x16, 0x49, 0x83, 0xC8, 0xFF, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x49, 0xFF, 0xC0, 0x42, 0x80, 0x3C, 0x02, 0x00, 0x75, 0xF6, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0xE8, 0x6C, 0x15, 0x5B, 0xFF, 0x48, 0x8B, 0x03, 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48, 0x8B, 0xCB, 0xFF, 0x50, 0x08, 0x48, 0x83, 0xC4, 0x40, 0x5B, 0xC3}},
        // the page's B playing PopUpMenu_Back (play_sound with RVA_BACK_SOUNDS does the same)
        {RVA_BACK_SOUND_CODE, {0xE8, 0xC3, 0x6B, 0x72, 0x00, 0x48, 0x8B, 0x15, 0xD4, 0x2B, 0xF7, 0x01, 0x48, 0x8D, 0x4C, 0x24, 0x20, 0x48,
                               0x8B, 0xD8, 0xE8, 0xEF, 0x97, 0x52, 0xFF, 0x4C, 0x8B, 0x03, 0x48, 0x8B, 0xD0, 0x48, 0x8B, 0xCB, 0x41, 0xFF,
                               0x50, 0x08}},
        // the page's own "focus FirstItem", down to its calls (focus_named does the same)
        {RVA_FOCUS_FIRST, {0x40, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x9C, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xD9, 0x41, 0x83, 0xC8, 0xFF, 0x48, 0x8D, 0x15, 0x70, 0xE3, 0x2A, 0x01, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0xE8, 0xBE, 0x8A, 0x9E, 0x00, 0x90, 0x48, 0x8D, 0x44, 0x24, 0x40, 0x48, 0x89, 0x84, 0x24, 0x98, 0x00, 0x00, 0x00, 0xF0, 0xFF, 0x44, 0x24, 0x50, 0x48, 0x8B, 0xD3, 0x48, 0x8D, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00, 0xE8, 0x9B, 0x89, 0xFE, 0xFF, 0x48, 0x8B, 0xD8, 0x48, 0x8D, 0x4C, 0x24, 0x40, 0xE8, 0x0E, 0xA9, 0x9E, 0x00, 0x48, 0x85, 0xDB, 0x74, 0x37, 0x48, 0x8B, 0x83, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x8B, 0xB8, 0x00, 0x00, 0x00, 0xFF, 0x50, 0x18, 0x48, 0x8D, 0x93, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4C, 0x24, 0x28, 0xE8, 0x17, 0x5A, 0x85, 0x00}},
    };
    auto undo_files = [] {
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml");
        clear_file(&g_owner_mod, "stateflow/pause.xml");
        clear_file(&g_owner_mod, "stateflow/ui_screens.xml");
        if (g_title_on_tab)
            slog("the title screen's line points at the MODS tab, which isn't there this time; Pause > Settings > MODS "
                 "opens the list that stands in for it");
    };
    if (!g_hub_files) return;
    for (const Check& c : checks)
        if (!code_matches(c.rva, c.bytes.data(), c.bytes.size())) {
            char b[16];
            std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(c.rva));
            slog(std::string("the game's code at game+") + b + " doesn't look as expected; no MODS tab");
            undo_files();
            return;
        }
    FH2Mod* m = &g_owner_mod;
    struct H {
        const char* name;
        uintptr_t rva;
        void* detour;
        TileFn* original;
    };
    const H hooks[] = {
        {"Marketplace: Car Packs tile", RVA_TILE_CAR_PACKS, reinterpret_cast<void*>(&d_tile_updates), &o_car_packs},
        {"Marketplace: Expansion Packs tile", RVA_TILE_EXPANSIONS, reinterpret_cast<void*>(&d_tile_get_mods), &o_expansions},
        {"Marketplace: Extras tile", RVA_TILE_EXTRAS, reinterpret_cast<void*>(&d_tile_about), &o_extras},
        {"Marketplace: VIP tile", RVA_TILE_VIP, reinterpret_cast<void*>(&d_tile_help), &o_vip},
    };
    std::vector<FH2Hook*> made;
    bool ok = true;
    for (const H& h : hooks) {
        FH2Hook* k = g_api->hook_function(m, h.name, reinterpret_cast<void*>(g_base + h.rva), h.detour, reinterpret_cast<void**>(h.original));
        ok         = ok && k;
        if (k) made.push_back(k);
    }
    void** slot = reinterpret_cast<void**>(g_base + RVA_HUD_PAGE_VT_SLOT);
    if (!readable(slot, 8) || *slot != reinterpret_cast<void*>(g_base + RVA_HUD_PAGE_LOADED)) {
        slog("the HUD page's vtable doesn't look as expected; no MODS tab");
        undo_files();
        return;
    }
    FH2Hook* loaded = g_api->hook_pointer(m, "HUD options page: loaded", slot, reinterpret_cast<void*>(&d_hud_page_loaded),
                                          reinterpret_cast<void**>(&o_hud_page_loaded));
    ok = ok && loaded;
    if (loaded) made.push_back(loaded);
    void** bslot = reinterpret_cast<void**>(g_base + RVA_HUD_PAGE_BUTTON_SLOT);
    if (!readable(bslot, 8) || *bslot != reinterpret_cast<void*>(g_base + RVA_PAGE_BUTTON)) {
        slog("the HUD page's vtable doesn't look as expected; no MODS tab");
        for (FH2Hook* k : made) g_api->disable_hooks(m, k);
        undo_files();
        return;
    }
    FH2Hook* button = g_api->hook_pointer(m, "HUD options page: buttons", bslot, reinterpret_cast<void*>(&d_hud_page_button),
                                          reinterpret_cast<void**>(&o_hud_page_button));
    ok = ok && button;
    if (button) made.push_back(button);
    FH2Hook* wire = g_api->hook_function(m, "PauseMenu: wire the tiles", reinterpret_cast<void*>(g_base + RVA_PAUSE_MENU_WIRE),
                                         reinterpret_cast<void*>(&d_pause_menu_wire), reinterpret_cast<void**>(&o_pause_menu_wire));
    ok = ok && wire;
    if (wire) made.push_back(wire);
    for (FH2Hook* k : made) ok = ok && g_api->enable_hooks(m, k);
    if (!ok) {
        slog("hooking the pause menu failed; no MODS tab");
        for (FH2Hook* k : made) g_api->disable_hooks(m, k);
        undo_files();
        return;
    }
    if (!show_marketplace_tab(m)) slog("the code that hides the Marketplace tab doesn't look as expected; MODS shows only with Extra Options' EnableMarketplace");
    g_sounds_ok = true;
    g_hub_ready = true;   // Pause > Settings > MODS opens YOUR MODS from now on
    {
        // the lookup the pause menu's own wiring uses, to tell when its MODS tiles are missing (d_pause_menu_wire)
        static const uint8_t find_button[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE,
                                              0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x48, 0x48, 0x89, 0x74, 0x24, 0x58, 0x48, 0x8B, 0xC2,
                                              0x48, 0x8B, 0xF9, 0x4C, 0x8D, 0x44, 0x24, 0x50, 0x48, 0x8B, 0x11, 0x48, 0x8B, 0xC8, 0xE8};
        static const uint8_t call[] = {0xE8, 0xE1, 0xE0, 0xF1, 0xFF};   // call 0x140CBDB20
        g_find_button_ok = code_matches(RVA_FIND_BUTTON, find_button, sizeof find_button) && code_matches(RVA_FIND_BUTTON_CALL, call, sizeof call);
        if (!g_find_button_ok) slog("the pause menu's tile lookup doesn't look as expected; a missing MODS tab won't be told to the player");
    }
    hook_tiles(m);
    if (g_hud_notice) {
        mod_notice_descriptor();   // made now, so the HUD hook knows there's a notice
        hook_hud_shown(m);
    }
    slog("MODS tab ready");
}

// The freeroam HUD page (Hud, vtable 0x142DEF9C8): +0x488 runs each time it's shown (in
// freeroam, and again after every menu), +0x4F8 when it's hidden. The first time, the notice
// about mods that didn't load plays (FH2Mod_ModNotice); the next, it's done.
constexpr uintptr_t RVA_HUD_SHOWN_SLOT = 0x2DEFE50;   // Hud vtable +0x488
constexpr uintptr_t RVA_HUD_SHOWN      = 0x0D38DD0;
using HudShownFn = uint64_t (*)(void*, void*, void*, void*);
HudShownFn o_hud_shown = nullptr;
uint64_t d_hud_shown(void* page, void* b, void* c, void* d) {
    const uint64_t r = o_hud_shown(page, b, c, d);
    if (!g_mod_notice_desc) return r;
    ULONGLONG none = 0;
    if (g_mod_notice_at.compare_exchange_strong(none, GetTickCount64())) {   // first shown: it plays
        if (g_notice_kind == kNoticeWelcome) {   // one of its sessions
            const int n = std::atoi(seen_get(kSeenPlatform, "welcome.sessions").c_str()) + 1;
            seen_set(kSeenPlatform, "welcome.sessions", std::to_string(n));
            slog("showing the HUD's welcome (session " + std::to_string(n) + " of " + std::to_string(kWelcomeSessions) + " at most)");
        } else {
            slog(g_notice_kind == kNoticeFailed ? "showing the HUD's notice about mods that didn't load" : "showing the HUD's notice about new mods");
        }
        // the new mods it names are told about now (the welcome names the running ones)
        if (g_notice_kind == kNoticeWelcome || g_notice_kind == kNoticeNewMods)
            for (const FreshMod& f : g_fresh)
                if (g_notice_kind == kNoticeNewMods || f.loaded) seen_set(f.who, "mod.announced");
        // the game has been played past the title screen that said mod settings have moved: once is enough
        if (g_title_moved && seen_set(kSeenPlatform, "moved.told"))
            slog("the title screen has said mod settings have moved; from the next start it says where they are");
    } else if (g_mod_notice_done.exchange(true))
        return r;                                                       // over already
    notify_changed(page, g_mod_notice_desc);
    return r;
}
// The HUD layer and the pop-ups (their page tracking, API 10) hook this slot too, and their hooks can go
// in first: the slot then holds a platform stub, and hook_pointer joins that hook's site. So, when a mod
// has a layer or the pop-ups track the page, the slot is checked in the exe on disk as well; a change made
// outside the platform is still refused by hook_pointer's own check. Without either the check is 2.8.0's.
bool hud_shown_slot_ok(void** slot) {
    const uint64_t original = 0x140000000ull + RVA_HUD_SHOWN;
    const bool as_built     = readable(slot, 8) && *slot == reinterpret_cast<void*>(g_base + RVA_HUD_SHOWN);
    if (!hud_layer_active() && !popups_track_page()) return as_built;
    return as_built || (readable(slot, 8) && disk_bytes_equal(slot, &original, 8));
}
void hook_hud_shown(FH2Mod* m) {
    void** slot = reinterpret_cast<void**>(g_base + RVA_HUD_SHOWN_SLOT);
    if (!hud_shown_slot_ok(slot)) {
        slog("the freeroam HUD's vtable doesn't look as expected; no notice about mods that didn't load");
        return;
    }
    if (FH2Hook* k = g_api->hook_pointer(m, "freeroam HUD: shown", slot, reinterpret_cast<void*>(&d_hud_shown),
                                         reinterpret_cast<void**>(&o_hud_shown)))
        g_api->enable_hooks(m, k);
}

// ---- title-screen entries (API 9) -----------------------------------------------------------
// The title screen is the Splash page (UI.zip Scenes\Anthem\A_Splash.xaml; C++ Splash, vtable
// 0x142E533C8): the attract video, the logo and "PRESS (A) TO START" (Main.str IDS_Press and
// IDS_To_Start, which also carries the notices), with no control to focus. The pad reaches the
// top page's vtable +0x578 (page, key, pressed) as keys (see pad_button); Splash's is the shared
// 0x140CEC3D0, which runs the key handlers a page registered. Splash registers only A (key 18,
// at 0x140E8F9AD, when it's shown): its handler 0x140EA0D40 returns while the state at
// page+0x448 is 3 or 4 (0 waiting, 1 A pressed, 2 a popup up, 3/4 on its way to loading), and
// otherwise sets 1 and starts signing in. So X, Y, LB and RB do nothing there: each entry gets
// one. Its line is written into the page under the Grid holding HelpButton (fading with it in
// the five state storyboards), and our +0x578 runs its on_press while the state is 0;
// FH2_TITLE_START then presses A through the original, so OnStart's own checks and the
// sign-in run as ever. An entry with a list shows the message box (show_list) over Splash
// instead, and the pick does the same through on_choose. A list longer than one box holds
// (kTitleListFits, as up to 2.8.0's FH2_TITLE_MAX_CHOICES) goes in pages of kTitlePageSize, with
// Previous and Next items that show the box again with the page before or after.
constexpr uintptr_t RVA_SPLASH_BUTTON_SLOT = 0x2E53940;   // Splash vtable +0x578 (holds RVA_PAGE_BUTTON)
constexpr uintptr_t RVA_SPLASH_ON_START    = 0x0EA0D40;   // Splash::OnStart (A)
constexpr uintptr_t RVA_SPLASH_A_HANDLER   = 0x0E8F9A5;   // Splash shown: registers A (key 18)
constexpr std::size_t kSplashState         = 0x448;       // int, see above
constexpr std::size_t kMaxTitleLabel       = 32;          // characters (text_chars); the panel is 326 px wide (about 24 capitals)
constexpr std::size_t kTitleListFits       = 10;          // choices one box shows, with Back
constexpr std::size_t kTitlePageSize       = 8;           // choices on a page of a longer list, with Previous, Next and Back
const char* const kTitleFile               = "UI/Scenes/Anthem/A_Splash.xaml";
struct TitleEntry {
    FH2Mod* mod;
    std::string key, label;
    FH2TitlePressFn on_press;
    void* user;
    int button;                              // FH2_BUTTON_
    FH2TitleChooseFn on_choose = nullptr;    // an entry with a list: its choices, then Back
    std::vector<std::string> choices;
    std::wstring list_title, list_message;
    std::vector<std::wstring> wchoices;
    std::vector<FakeLocString*> loc;         // title, message, the choices, Back (made once it's in g_titles)
    std::vector<std::wstring> page_titles;   // a list in pages: the title with "2/5" after it, per page
    std::vector<FakeLocString*> page_loc;
};
std::deque<TitleEntry> g_titles;             // guarded by g_mu (entries are never removed or moved)
std::atomic<int> g_title_started{-1};        // the entry whose on_press started the game, -1 for none
std::atomic<int> g_title_list{-1};           // the entry whose list is up, -1 for none
// What each item of the list that's up is (UI thread only): a choice's position in the whole
// list, or one of these.
constexpr int kItemBack = -1, kItemPrevious = -2, kItemNext = -3;
std::vector<int> g_title_items;
int g_title_list_page = 0;
FakeLocString* g_loc_previous = nullptr;   // made with the first list in pages
FakeLocString* g_loc_next     = nullptr;
bool show_title_list(void* page, int j, int p);
bool g_title_page = false;                   // A_Splash.xaml is written with the entries
bool g_title_handled[7] = {};                // per FH2_BUTTON_: the press was an entry's (UI thread only)
const int kTitleButtons[] = {FH2_BUTTON_Y, FH2_BUTTON_X, FH2_BUTTON_RB, FH2_BUTTON_LB};

const char* button_letters(int b) {
    switch (b) {
        case FH2_BUTTON_X: return "X";
        case FH2_BUTTON_Y: return "Y";
        case FH2_BUTTON_LB: return "LB";
        case FH2_BUTTON_RB: return "RB";
        default: return nullptr;
    }
}

// A_Splash.xaml with every entry's line in it, or remove_leftover_file when there are none.
bool write_title_page() {
    std::vector<std::pair<int, std::string>> lines;
    {
        std::scoped_lock lk{g_mu};
        for (TitleEntry& e : g_titles) {
            l10n_follow(e.mod, "title." + e.key + ".label", e.label);   // tr()'s text, in the game's language now (API 10)
            if (text_chars(e.label) > kMaxTitleLabel) e.label = cut_text(e.label, kMaxTitleLabel);
            lines.emplace_back(e.button, e.label);
        }
    }
    if (lines.empty()) {
        remove_leftover_file(kTitleFile);
        return false;
    }
    std::string page = read_ui("Scenes/Anthem/A_Splash.xaml");
    const std::string fade  = "<DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.Opacity)\" Storyboard.TargetName=\"HelpButton\">";
    const std::string close = "</DoubleAnimationUsingKeyFrames>";
    auto count = [&page](const std::string& what) {
        std::size_t n = 0;
        for (std::size_t at = page.find(what); at != std::string::npos; at = page.find(what, at + 1)) ++n;
        return n;
    };
    if (page.empty() || count(fade) != 5 || count("x:Name=\"HelpButton\"") != 1 || count("x:Name=\"LoadingControl\"") != 1 ||
        count("</ResourceDictionary>") != 1) {
        slog("A_Splash.xaml doesn't look as expected; no title-screen entries");
        remove_leftover_file(kTitleFile);
        return false;
    }
    // each state's storyboard: the entries take HelpButton's opacity (a copy of its block)
    std::string out;
    std::size_t at = 0;
    for (std::size_t f = page.find(fade); f != std::string::npos; f = page.find(fade, f + 1)) {
        const std::size_t e = page.find(close, f);
        if (e == std::string::npos) {
            slog("A_Splash.xaml doesn't look as expected; no title-screen entries");
            remove_leftover_file(kTitleFile);
            return false;
        }
        const std::size_t end  = e + close.size();
        const std::size_t line = page.rfind('\n', f) + 1;
        std::string copy       = page.substr(f, end - f);
        swap_all(copy, "Storyboard.TargetName=\"HelpButton\"", "Storyboard.TargetName=\"FH2Mod_TitleEntries\"");
        out += page.substr(at, end - at) + "\r\n" + page.substr(line, f - line) + copy;
        at = end;
    }
    out += page.substr(at);
    page = out;
    // the lines, under the Grid that holds HelpButton and LoadingControl
    std::string panel = "\r\n                    <!-- FH2 Mod Platform: title-screen entries -->\r\n"
                        "                    <StackPanel x:Name=\"FH2Mod_TitleEntries\" Style=\"{StaticResource FH2Mod_Style_TitleEntries}\" "
                        "HorizontalAlignment=\"Center\" Margin=\"0, 8, 0, 0\">";
    for (const auto& [button, label] : lines)
        panel += std::string("\r\n                        <StackPanel Orientation=\"Horizontal\" HorizontalAlignment=\"Center\" Margin=\"0, 4, 0, 0\">\r\n"
                             "                            <local:FZMaterial ImageSource=\"{StaticResource A_BitmapImage_ControllerButton_") +
                 button_letters(button) + "}\" Width=\"32\" Height=\"32\" />\r\n"
                 "                            <local:FZTextBlock Text=\"" + xml_escape(upper_game(label)) +
                 "\" Style=\"{StaticResource A_Style_Text_LoadingSmall}\" Margin=\"8, 0, 0, 0\" />\r\n"
                 "                        </StackPanel>";
    panel += "\r\n                    </StackPanel>";
    const std::size_t grid = page.find("</Grid>", page.find("x:Name=\"LoadingControl\""));
    if (grid == std::string::npos) {
        slog("A_Splash.xaml doesn't look as expected; no title-screen entries");
        remove_leftover_file(kTitleFile);
        return false;
    }
    page.insert(grid + 7, panel);
    // collapsed unless the title screen is hooked
    page.insert(page.find("</ResourceDictionary>"),
                "    <Style x:Key=\"FH2Mod_Style_TitleEntries\" TargetType=\"StackPanel\">\r\n"
                "                <Setter Property=\"Visibility\" Value=\"Collapsed\" />\r\n"
                "                <Style.Triggers>\r\n"
                "                    <DataTrigger Binding=\"{Binding FH2Mod_TitleReady}\" Value=\"True\">\r\n"
                "                        <Setter Property=\"Visibility\" Value=\"Visible\" />\r\n"
                "                    </DataTrigger>\r\n"
                "                </Style.Triggers>\r\n"
                "            </Style>\r\n        ");
    g_title_page = set_file(&g_owner_mod, kTitleFile, page.data(), page.size()) != 0;
    return g_title_page;
}

PageButtonFn o_splash_button = nullptr;

bool call_on_press(FH2TitlePressFn fn, void* user, const char* key, int* result) {
    __try {
        *result = fn(user, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_on_choose(FH2TitleChooseFn fn, void* user, const char* key, uint32_t choice, int* result) {
    __try {
        *result = fn(user, key, choice);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int splash_state(void* page) {
    __try {
        return *reinterpret_cast<int*>(static_cast<uint8_t*>(page) + kSplashState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool d_splash_button(void* page, int button, bool pressed) {
    const int code = pad_button(button);
    if (code == FH2_BUTTON_A && pressed && splash_state(page) == 0) g_title_started = -1;   // the player's own start
    if (!g_title_ready.load() || !button_letters(code)) return o_splash_button(page, button, pressed);
    int j               = -1;
    FH2Mod* mod         = nullptr;
    FH2TitlePressFn fn  = nullptr;
    void* user          = nullptr;
    std::string key;
    std::vector<FakeLocString*> loc;
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t i = 0; i < g_titles.size(); ++i)
            if (g_titles[i].button == code) {
                j    = static_cast<int>(i);
                mod  = g_titles[i].mod;
                fn   = g_titles[i].on_press;
                user = g_titles[i].user;
                key  = g_titles[i].key;
                loc  = g_titles[i].loc;
            }
    }
    if (j < 0) return o_splash_button(page, button, pressed);
    if (!pressed) {   // the release of an entry's press is the entry's too
        const bool ours        = g_title_handled[code];
        g_title_handled[code] = false;
        return ours || o_splash_button(page, button, pressed);
    }
    if (splash_state(page) != 0) return o_splash_button(page, button, pressed);   // already signing in
    if (stopped_online(mod)) return o_splash_button(page, button, pressed);   // stopped for online play (go_online)
    g_title_handled[code] = true;
    if (!loc.empty()) {   // the list (the message box plays its own sound)
        if (!show_title_list(page, j, 0)) log_line(mod->name, "couldn't show the list for title entry " + key);
        return true;
    }
    play_sound(kSoundSelect);
    int r = FH2_TITLE_HANDLED;
    UiCallback cb{mod, UiCb::title, page};
    ui_note_callback(mod);
    if (!call_on_press(fn, user, key.c_str(), &r)) {
        crashed_in(mod, "on_press for title entry " + key);
        r = FH2_TITLE_HANDLED;
    }
    if (r == FH2_TITLE_START && cb.state.box_shown) {   // the box would stay up over a page that's going away
        log_line(mod->name, "title entry " + key + " showed a message box and asked to start the game; the box stays and the game doesn't start");
        r = FH2_TITLE_HANDLED;
    }
    if (r == FH2_TITLE_START) {
        log_line(mod->name, "title entry " + key + " starts the game");
        g_title_started = j;
        o_splash_button(page, kButtonA, true);   // A, as the player would press it
    }
    return true;
}

// Shows page `p` of entry `j`'s list (UI thread): a list that fits in one box as it is, a longer
// one as kTitlePageSize choices with Previous before them (after the first page) and Next after
// them (before the last), each page titled with its number. Back is always the last item.
bool show_title_list(void* page, int j, int p) {
    std::vector<FakeLocString*> labels;
    std::vector<int> items;
    FakeLocString *title = nullptr, *message = nullptr;
    {
        std::scoped_lock lk{g_mu};
        if (j < 0 || j >= static_cast<int>(g_titles.size()) || g_titles[j].loc.size() < 3) return false;
        const TitleEntry& e = g_titles[j];
        const int n         = static_cast<int>(e.choices.size());
        title               = e.loc[0];
        message             = e.loc[1];
        int from = 0, to = n;
        if (n > static_cast<int>(kTitleListFits)) {
            const int pages = static_cast<int>(e.page_loc.size());
            p               = p < 0 ? 0 : p >= pages ? pages - 1 : p;
            from            = p * static_cast<int>(kTitlePageSize);
            to              = std::min(n, from + static_cast<int>(kTitlePageSize));
            title           = e.page_loc[p];
            if (p > 0) {
                labels.push_back(g_loc_previous);
                items.push_back(kItemPrevious);
            }
        }
        for (int i = from; i < to; ++i) {
            labels.push_back(e.loc[2 + i]);
            items.push_back(i);
        }
        if (to < n) {
            labels.push_back(g_loc_next);
            items.push_back(kItemNext);
        }
        labels.push_back(e.loc[2 + n]);   // Back
        items.push_back(kItemBack);
    }
    g_title_items     = std::move(items);
    g_title_list_page = p;
    g_title_list      = j;
    if (g_popup_ok && show_list(page, title, message, labels.data(), static_cast<int>(labels.size()), g_title_fn_vt)) return true;
    g_title_list = -1;
    return false;
}

// A pick from a title entry's list (the message box's chosen-item handler, on the UI thread).
// The box closes by itself; Back does nothing else, and Previous or Next show the box again with
// the page before or after.
void handle_title_choice(PopupFn* self) {
    const int j    = g_title_list.exchange(-1);
    const int item = chosen_item(self->popup);
    const int what = item >= 0 && item < static_cast<int>(g_title_items.size()) ? g_title_items[item] : kItemBack;
    if (what == kItemPrevious || what == kItemNext) {
        if (!show_title_list(self->page, j, g_title_list_page + (what == kItemNext ? 1 : -1))) {
            std::scoped_lock lk{g_mu};
            if (j >= 0 && j < static_cast<int>(g_titles.size()))
                log_line(g_titles[j].mod->name, "couldn't show the next page of the list for title entry " + g_titles[j].key);
        }
        return;
    }
    const int choice    = what;
    FH2Mod* mod         = nullptr;
    FH2TitleChooseFn fn = nullptr;
    void* user          = nullptr;
    std::string key, label;
    {
        std::scoped_lock lk{g_mu};
        if (j < 0 || j >= static_cast<int>(g_titles.size())) return;
        const TitleEntry& e = g_titles[j];
        if (choice < 0 || choice >= static_cast<int>(e.choices.size())) return;   // Back
        mod   = e.mod;
        fn    = e.on_choose;
        user  = e.user;
        key   = e.key;
        label = e.choices[choice];
    }
    if (stopped_online(mod)) return;   // stopped for online play (go_online) while its list was open
    int r = FH2_TITLE_HANDLED;
    UiCallback cb{mod, UiCb::title, self->page};
    ui_note_callback(mod);
    if (!call_on_choose(fn, user, key.c_str(), static_cast<uint32_t>(choice), &r)) {
        crashed_in(mod, "on_choose for title entry " + key);
        r = FH2_TITLE_HANDLED;
    }
    if (r == FH2_TITLE_START && cb.state.box_shown) {   // the box would stay up over a page that's going away
        log_line(mod->name, "title entry " + key + " (" + label + ") showed a message box and asked to start the game; the box stays and the game doesn't start");
        r = FH2_TITLE_HANDLED;
    }
    if (r == FH2_TITLE_START) {
        log_line(mod->name, "title entry " + key + " (" + label + ") starts the game");
        g_title_started = j;
        if (splash_state(self->page) == 0) o_splash_button(self->page, kButtonA, true);   // A, as the player would press it
    }
}

// ---- the mods' message boxes (API 10) ------------------------------------------------------
// show_message_box: the game's own box, as the restart box and the title-screen lists show it (show_list), over the
// page the player is on, from one of the mod's page, setting or title-screen callbacks (ui_callback_now) and nowhere
// else: the platform has no other way to know which page is on top, or to be on the UI thread. The rules for its
// text and pages are ui_rules.cpp's. Back is always the last item (the game's word, as title lists have it), and B
// calls on_choose with FH2_BOX_BACK too when the box's B handler is as S6 found it (g_box_back_ok). A box from a
// tile page's on_open waits for that page to load (show_pending_box). Everything here is on the game's UI thread.
//
// The game's box keeps pointers to its text, so a box's strings are copied once into memory that's never freed
// (g_boxes isn't either): a few hundred bytes a box the player opens.
struct ModBox {
    FH2Mod* mod = nullptr;
    BoxSpec spec;
    std::string shown_title;               // in capitals, as shown (the log)
    FakeLocString* title   = nullptr;
    FakeLocString* message = nullptr;
    FakeLocString* back    = nullptr;
    std::vector<FakeLocString*> choices;
    std::vector<FakeLocString*> page_titles;   // a box in pages: the title with "2/5" after it
    int page_index  = -1;                  // the mod's page it was shown from (show_page_hint from on_choose), or -1
    uint32_t faults = 0;                   // ui_mod_faults when it was shown
};
std::deque<ModBox> g_boxes;
int g_box_up          = -1;    // the box showing, by index, or -1
int g_box_up_page     = 0;     // its page
void* g_box_up_popup  = nullptr; // the game's box it is: an answer from any other box goes nowhere
ULONGLONG g_box_shown_at = 0;  // when it was shown (GetTickCount64)
std::vector<int> g_box_items;  // what each of its items is (a choice, or kBoxItem...)
int g_box_pending     = -1;    // a box from a tile page's on_open, waiting for that page to load
int g_box_pending_for = -1;    // that page (g_pages)
ULONGLONG g_box_pending_at = 0; // when it was asked for
bool g_box_back_ok    = false; // B can be wired (S6), checked against the exe on disk
// One box at a time (S6 3.2): two shown in the same frame, or while the game is still bringing one up, make the
// second its pending box, and a third replaces that without its handlers. So no second box from the same chain of
// callbacks, none while one is waiting for its page, and none in the moment after one was shown (no player answers
// a box that fast). A box that closes without telling the platform (B with g_box_back_ok false) then blocks nothing.
constexpr ULONGLONG kBoxSettleMs  = 500;
constexpr ULONGLONG kBoxPendingMs = 10000;   // a page loads well within this; a box older than this is dropped

// UTF-16 copy of `s` that's never freed (the game's box points at it).
const wchar_t* keep_w(const std::string& s) {
    const std::wstring w = widen(s);
    auto* p = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, (w.size() + 1) * sizeof(wchar_t)));
    if (!p) return L"";
    std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    return p;
}

bool show_box_page(void* page, int idx, int p) {
    if (idx < 0 || static_cast<std::size_t>(idx) >= g_boxes.size()) return false;
    const ModBox& b       = g_boxes[idx];
    const std::size_t n   = b.spec.choices.size();
    const int pages       = box_pages(n);
    p                     = std::clamp(p, 0, pages - 1);
    std::vector<int> items = box_page_items(n, p);
    std::vector<FakeLocString*> labels;
    for (int it : items)
        labels.push_back(it >= 0 ? b.choices[it] : it == kBoxItemBack ? b.back : it == kBoxItemPrevious ? g_loc_previous : g_loc_next);
    FakeLocString* title = b.page_titles.empty() ? b.title : b.page_titles[p];
    g_box_items          = std::move(items);
    g_box_up             = idx;
    g_box_up_page        = p;
    void* popup          = nullptr;
    if (show_list(page, title, b.message, labels.data(), static_cast<int>(labels.size()), g_box_fn_vt, g_box_back_ok ? g_box_back_fn_vt : nullptr,
                  &popup)) {
        g_box_up_popup = popup;
        g_box_shown_at = GetTickCount64();
        return true;
    }
    g_box_up       = -1;
    g_box_up_popup = nullptr;
    return false;
}

bool call_box_choose(FH2BoxChooseFn fn, void* user, const char* key, uint32_t choice) {
    __try {
        fn(user, key, choice);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player answered box `idx` (a choice, or FH2_BOX_BACK): logged, and the mod's on_choose called.
void box_answered(void* page, int idx, uint32_t choice) {
    const ModBox& b = g_boxes[idx];
    FH2Mod* mod     = b.mod;
    const std::string answer = choice == FH2_BOX_BACK ? std::string("Back") : b.spec.choices[choice];
    log_line(mod->name, "message box answered: " + b.shown_title + " -> " + answer);
    if (!b.spec.on_choose || stopped_online(mod)) return;
    if (ui_mod_faults(mod) != b.faults) {
        log_line(mod->name, "message box " + b.spec.key + ": the mod faulted while it was open, so its on_choose isn't called");
        return;
    }
    UiCallback cb{mod, UiCb::box, page, b.page_index};
    ui_note_callback(mod);
    if (!call_box_choose(b.spec.on_choose, b.spec.user, b.spec.key.c_str(), choice)) crashed_in(mod, "on_choose for message box " + b.spec.key);
}

// An answer from a box that isn't the one the platform has up (the game showing a stray second box) goes nowhere,
// rather than to the wrong mod's on_choose or with the wrong item map.
bool box_is_up(const PopupFn* self) {
    if (g_box_up >= 0 && self->popup == g_box_up_popup) return true;
    slog("a message box answered that isn't the mod's box the platform has up; nothing is called for it");
    return false;
}

// A chosen item (the box closes by itself after this): Previous and Next show the box again with the page before or
// after; Back and the choices go to the mod.
void handle_box_choice(PopupFn* self) {
    if (!box_is_up(self)) return;
    const int idx = std::exchange(g_box_up, -1);
    if (idx < 0 || static_cast<std::size_t>(idx) >= g_boxes.size()) return;
    const int item = chosen_item(self->popup);
    const int what = item >= 0 && static_cast<std::size_t>(item) < g_box_items.size() ? g_box_items[item] : kBoxItemBack;
    if (what == kBoxItemPrevious || what == kBoxItemNext) {
        if (!show_box_page(self->page, idx, g_box_up_page + (what == kBoxItemNext ? 1 : -1)))
            log_line(g_boxes[idx].mod->name, "couldn't show the next page of message box " + g_boxes[idx].spec.key);
        return;
    }
    box_answered(self->page, idx, what >= 0 ? static_cast<uint32_t>(what) : FH2_BOX_BACK);
}

// B (the box closes by itself after this).
void handle_box_back(PopupFn* self) {
    if (!box_is_up(self)) return;
    const int idx = std::exchange(g_box_up, -1);
    if (idx >= 0 && static_cast<std::size_t>(idx) < g_boxes.size()) box_answered(self->page, idx, FH2_BOX_BACK);
}

// d_hud_page_loaded: the box a tile page's on_open asked for, now that the page is there (or dropped if it isn't).
void show_pending_box(void* page, bool tile_page) {
    const int idx = std::exchange(g_box_pending, -1);
    if (idx < 0 || static_cast<std::size_t>(idx) >= g_boxes.size()) return;
    const ModBox& b = g_boxes[idx];
    if (!tile_page || g_custom_shown.load() != g_box_pending_for || GetTickCount64() - g_box_pending_at >= kBoxPendingMs) {
        log_line(b.mod->name, "message box " + b.spec.key + " wasn't shown: the page whose on_open asked for it didn't open");
        return;
    }
    if (show_box_page(page, idx, box_page_of(b.spec.choices.size(), b.spec.focus)))
        log_line(b.mod->name, "message box shown: " + b.shown_title + " (key " + b.spec.key + ", over its page as it opened)");
    else
        log_line(b.mod->name, "message box " + b.spec.key + " couldn't be shown over its page");
}

// Checks the title screen's code and makes (doesn't enable) the hook on its buttons; null if
// it can't, with the page's lines taken back out.
FH2Hook* hook_title(FH2Mod* m) {
    const uint8_t on_start[]  = {0x8B, 0x81, 0x48, 0x04, 0x00, 0x00, 0x83, 0xE8, 0x03, 0x83, 0xF8, 0x01};   // the state at +0x448, 3 and 4
    const uint8_t a_handler[] = {0xBA, 0x12, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xCE, 0xE8, 0xDE, 0xB6, 0x58, 0x00};   // A (18) registered
    void** slot = reinterpret_cast<void**>(g_base + RVA_SPLASH_BUTTON_SLOT);
    if (!code_matches(RVA_SPLASH_ON_START + 0x27, on_start, sizeof on_start) ||
        !code_matches(RVA_SPLASH_A_HANDLER, a_handler, sizeof a_handler) || !readable(slot, 8) ||
        *slot != reinterpret_cast<void*>(g_base + RVA_PAGE_BUTTON)) {
        slog("the title screen's code doesn't look as expected; no title-screen entries");
        clear_file(&g_owner_mod, kTitleFile);
        return nullptr;
    }
    FH2Hook* h = g_api->hook_pointer(m, "Title screen: buttons", slot, reinterpret_cast<void*>(&d_splash_button),
                                     reinterpret_cast<void**>(&o_splash_button));
    if (!h) {
        slog("hooking the title screen failed; no title-screen entries");
        clear_file(&g_owner_mod, kTitleFile);
    }
    return h;
}

// ---- hooking up ----------------------------------------------------------------------------
// What the HUD layer waits for (settings_properties_state): 0 not yet, 1 FindProperty hooked, -1 it never will be.
std::atomic<int> g_props_state{0};

void hook_game() {
    void* base = g_api->wait_for_code(0xFFFFFFFFu);
    if (!base) {
        g_props_state = -1;
        return;
    }
    g_base = reinterpret_cast<uintptr_t>(base);
    struct Check {
        uintptr_t rva;
        std::vector<uint8_t> bytes;
    };
    const Check checks[] = {
        {RVA_FIND_PROPERTY, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48}},
        {RVA_BOX_INT, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9}},
        {RVA_OBJECT_CTOR, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20}},
        {RVA_MODEL_PROPERTY, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF}},
        {RVA_BOX_BOOL, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x80, 0x3A, 0x00}},
        {RVA_BOOL_TYPE, {0x80, 0x3D, 0x0D, 0xB8, 0x0E, 0x02, 0x00, 0x75, 0x5F}},
        {RVA_NOTIFY_PROP, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF}},
        {RVA_PAUSE_WIRE_TILES, {0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0x68, 0xD8, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00}},
        {RVA_WIRE_BUTTON, {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00}},
        {RVA_HUD_TILE, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89, 0x10, 0x04, 0x00, 0x00}},
        {RVA_HUD_SCENE_RECORD, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70}},
        {RVA_PAUSE_NAV_SIZE, {0xC7, 0x45, 0x40, 0x02, 0x00, 0x00, 0x00, 0xC7, 0x45, 0x48, 0x05, 0x00, 0x00, 0x00}},
    };
    for (const Check& c : checks)
        if (!code_matches(c.rva, c.bytes.data(), c.bytes.size())) {
            char b[16];
            std::snprintf(b, sizeof b, "%llX", static_cast<unsigned long long>(c.rva));
            slog(std::string("the game's code at game+") + b + " doesn't look as expected; no Mods tile");
            clear_file(&g_owner_mod, nullptr);
            g_props_state = -1;
            return;
        }
    const Check slider_checks[] = {
        // a float property's descriptor accessor, down to the call for the float type
        {RVA_FLOAT_PROPERTY, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89,
                              0x5C, 0x24, 0x40, 0x8B, 0x05, 0x3E, 0x16, 0x67, 0x02, 0xA8, 0x01, 0x0F, 0x85, 0x85, 0x00, 0x00, 0x00,
                              0x83, 0xC8, 0x01, 0x89, 0x05, 0x2D, 0x16, 0x67, 0x02, 0x33, 0xC9, 0xE8, 0x3E, 0x1D, 0x6F, 0x00}},
        {RVA_BOX_FLOAT, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9, 0xC7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00, 0xE8,
                         0x1A, 0x11, 0xF4, 0xFF}},
    };
    g_sliders_ok = true;
    for (const Check& c : slider_checks) g_sliders_ok = g_sliders_ok && code_matches(c.rva, c.bytes.data(), c.bytes.size());
    if (!g_sliders_ok) slog("the game's float properties don't look as expected; sliders and choice rows won't work");
    const Check popup_checks[] = {
        {RVA_MAKE_ITEMS, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24}},
        {RVA_MAKE_ITEM, {0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40}},
        {RVA_SHOW_POPUP, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10}},
        {RVA_POPUP_HELP, {0x40, 0x55, 0x56, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x40, 0xFE}},
        {RVA_POPUP_HANDLER, {0x4C, 0x89, 0x44, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48}},
        {RVA_POPUP_SELECTED, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x89, 0xA0}},
        {RVA_SOME_TYPEINFO, {0x48, 0x8D, 0x05, 0x09, 0x53, 0x63, 0x02, 0xC3}},
        {RVA_OBJECT_FREE, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8}},
    };
    g_popup_ok = true;
    for (const Check& c : popup_checks) g_popup_ok = g_popup_ok && code_matches(c.rva, c.bytes.data(), c.bytes.size());
    void** loc_vt = reinterpret_cast<void**>(g_base + RVA_LOCSTRING_VT);
    g_popup_ok    = g_popup_ok && readable(loc_vt, sizeof g_loc_vt) &&
                 reinterpret_cast<uintptr_t>(loc_vt[7]) == g_base + RVA_LOCSTRING_TEXT;
    if (g_popup_ok) {
        std::memcpy(g_loc_vt, loc_vt, sizeof g_loc_vt);
        g_loc_vt[7] = reinterpret_cast<void*>(&fake_text);
        g_loc_vt[8] = reinterpret_cast<void*>(&fake_clone);
    } else {
        slog("the game's message box code doesn't look as expected; no restart popup");
    }
    // A mod's box (API 10) calls its on_choose for B too: the box's handler registration, its event 3 on B (key 45),
    // and the handler's call then close (S6), as the exe on disk has them.
    {
        static const uint8_t events[]  = {0x83, 0xFA, 0x07, 0x77, 0x74, 0x48, 0x63, 0xC2, 0x48};   // events above 7 register nothing
        static const uint8_t event3[]  = {0x3B, 0x87, 0xDD, 0x00};                                 // event 3's jump-table entry: slot 1
        static const uint8_t slot_b[]  = {0xB8, 0x2D, 0x00, 0x00, 0x00, 0xC3};                     // slot 1 -> key 45 (B)
        static const uint8_t invoke[]  = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x49, 0x20, 0x48, 0x85,
                                          0xC9, 0x74, 0x06, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10, 0x80, 0x7B, 0x28, 0x00, 0x74, 0x0D};
        auto at = [](uintptr_t rva) { return reinterpret_cast<const void*>(g_base + rva); };
        g_box_back_ok = g_popup_ok && disk_bytes_equal(at(0xDD871A), events, sizeof events) && disk_bytes_equal(at(0xDD87E8), event3, sizeof event3) &&
                        disk_bytes_equal(at(0xB04A21), slot_b, sizeof slot_b) && disk_bytes_equal(at(0xDE7280), invoke, sizeof invoke);
        if (g_popup_ok && !g_box_back_ok)
            slog("the message box's B button can't be wired: the game's code isn't as expected; B closes mods' boxes without telling the mod");
    }
    {
        std::scoped_lock lk{g_mu};
        g_asked_values = current_values();
    }

    FH2Mod* m = &g_owner_mod;
    FH2Hook* title = g_title_page ? hook_title(m) : nullptr;
    if (!g_api->hook_function(m, "AVUI::Type::FindProperty", reinterpret_cast<void*>(g_base + RVA_FIND_PROPERTY),
                              reinterpret_cast<void*>(&d_find_property), reinterpret_cast<void**>(&o_find_property)) ||
        !g_api->hook_function(m, "PauseOptionsMenu: wire the tiles", reinterpret_cast<void*>(g_base + RVA_PAUSE_WIRE_TILES),
                              reinterpret_cast<void*>(&d_wire_tiles), reinterpret_cast<void**>(&o_wire_tiles)) ||
        !g_api->hook_function(m, "HUD options scene record", reinterpret_cast<void*>(g_base + RVA_HUD_SCENE_RECORD),
                              reinterpret_cast<void*>(&d_hud_scene), reinterpret_cast<void**>(&o_hud_scene)) ||
        !g_api->enable_hooks(m, nullptr)) {
        slog("hooking the Settings page failed; no Mods tile or MODS tab");
        g_api->disable_hooks(m, nullptr);
        clear_file(m, nullptr);
        g_props_state = -1;
        return;
    }
    slog("ready");
    g_props_state = 1;
    if (title) {
        g_title_ready = true;
        slog("title-screen entries ready");
    }
    hook_hub();
}

} // namespace

// ---- the API ---------------------------------------------------------------------------------

int settings_add(FH2Mod* m, const FH2Setting* s) {
    if (!m || !s || s->size < offsetof(FH2Setting, user) + sizeof(void*)) return 0;
    if (!valid_key(s->key)) {
        log_line(m->name, std::string("add_setting: \"") + (s->key ? s->key : "") + "\" isn't a usable key (letters, digits, _)");
        return 0;
    }
    std::scoped_lock lk{g_mu};
    if (find_row(m, s->key) || has_link(m, s->key)) {
        log_line(m->name, std::string("add_setting: there's already a row called ") + s->key);
        return 0;
    }
    if (s->size >= offsetof(FH2Setting, url) + sizeof(const char*) && s->kind == FH2_SETTING_LINK) {
        const std::string url = s->url ? s->url : "";
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
            log_line(m->name, std::string("add_setting: ") + s->key + " is a link but its url isn't an http:// or https:// address");
            return 0;
        }
        g_links.push_back({m, s->key, s->label && *s->label ? s->label : s->key, s->description ? s->description : "", url});
        l10n_note(m, std::string("row.") + s->key + ".label", s->label);   // tr()'s text follows the game's language (API 10)
        l10n_note(m, std::string("row.") + s->key + ".description", s->description);
        log_line(m->name, "MODS tab link: " + g_links.back().label + " (" + url + ")" +
                              (g_written ? "; the page is already written, so it shows from the next launch" : ""));
        return 1;
    }
    Row& r       = g_rows.emplace_back();
    r.mod        = m;
    r.key        = s->key;
    r.label      = s->label && *s->label ? s->label : s->key;
    r.description = s->description ? s->description : "";
    l10n_note(m, "row." + r.key + ".label", s->label);   // tr()'s text follows the game's language (API 10)
    l10n_note(m, "row." + r.key + ".description", s->description);
    r.prop       = "FH2Mod_" + std::to_string(g_rows.size() - 1);
    r.value      = s->value ? 1 : 0;
    r.boot       = r.value;
    r.restart    = s->needs_restart != 0;
    r.on_change  = s->on_change;
    r.user       = s->user;
    if (s->size >= offsetof(FH2Setting, on_slide) + sizeof(void*) && s->kind == FH2_SETTING_SLIDER) {
        if (!(std::isfinite(s->min) && std::isfinite(s->max) && s->max > s->min)) {
            log_line(m->name, std::string("add_setting: ") + s->key + " needs max above min");
            g_rows.pop_back();
            return 0;
        }
        r.slider   = true;
        r.fmin     = s->min;
        r.fmax     = s->max;
        r.fstep    = s->step > 0 && std::isfinite(s->step) ? s->step : (s->max - s->min) / 100.0f;
        r.scale    = s->display_scale > 0 && std::isfinite(s->display_scale) ? s->display_scale : 1.0f;
        r.decimals = s->decimals < 0 ? 0 : s->decimals > 3 ? 3 : s->decimals;
        r.on_slide = s->on_slide;
        r.fvalue   = snap(r, std::isfinite(s->slider_value) ? s->slider_value : s->min);
        r.fboot    = r.fvalue;
        r.value    = 0;
        r.boot     = 0;
    }
    if (s->size >= offsetof(FH2Setting, choice_count) + sizeof(uint32_t) && s->kind == FH2_SETTING_CHOICE) {
        if (!s->choices || s->choice_count < 2 || s->choice_count > FH2_SETTING_MAX_CHOICES) {
            log_line(m->name, std::string("add_setting: ") + s->key + " is a choice row but needs 2 to " +
                                  std::to_string(FH2_SETTING_MAX_CHOICES) + " choices");
            g_rows.pop_back();
            return 0;
        }
        // shown like the game's own option rows; underneath, a slider over the choices' numbers
        for (uint32_t c = 0; c < s->choice_count; ++c) {
            r.choices.push_back(s->choices[c] && *s->choices[c] ? s->choices[c] : "?");
            l10n_note(m, "row." + r.key + ".choice." + std::to_string(c), s->choices[c]);
        }
        const int last = static_cast<int>(s->choice_count) - 1;
        const int v    = s->value < 0 ? 0 : s->value > last ? last : s->value;
        r.slider       = true;
        r.fmin         = 0;
        r.fmax         = static_cast<float>(last);
        r.fstep        = 1;
        r.scale        = 1;
        r.decimals     = 0;
        r.value        = v;
        r.boot         = v;
        r.fvalue       = static_cast<float>(v);
        r.fboot        = r.fvalue;
    }
    log_line(m->name, "MODS row: " + r.label + (r.restart ? " (after a restart)" : "") +
                          (g_written ? "; the page is already written, so it shows from the next launch" : ""));
    return 1;
}

// A hotkey's row (hotkeys.cpp): shown under "Keys and buttons" on its mod's page, by how it was added rather than
// by its key, which a mod's own row could share the look of.
void settings_mark_hotkey(FH2Mod* m, const char* key) {
    if (!m || !key) return;
    std::scoped_lock lk{g_mu};
    if (Row* r = find_row(m, key)) r->hotkey = true;
}

// New text for a row (API 10): the pages are written as they're opened, so it shows the next time.
int settings_set_text(FH2Mod* m, const char* key, const char* label, const char* description, const char* const* choices,
                      uint32_t choice_count) {
    if (!m || !key) return 0;
    std::scoped_lock lk{g_mu};
    if (Row* r = find_row(m, key)) {
        if (choices && choice_count != r->choices.size()) {
            log_line(m->name, std::string("set_setting_text: ") + key + " has " + std::to_string(r->choices.size()) +
                                  " choices, not " + std::to_string(choice_count));
            return 0;
        }
        if (label && *label) r->label = label;
        if (description) r->description = description;
        if (choices)
            for (uint32_t c = 0; c < choice_count; ++c) r->choices[c] = choices[c] && *choices[c] ? choices[c] : "?";
        // tr()'s text follows the game's language from here (API 10); other text stays as given
        if (label && *label) l10n_note(m, std::string("row.") + key + ".label", label);
        if (description) l10n_note(m, std::string("row.") + key + ".description", description);
        if (choices)
            for (uint32_t c = 0; c < choice_count; ++c) l10n_note(m, std::string("row.") + key + ".choice." + std::to_string(c), choices[c]);
        return 1;
    }
    for (LinkRow& l : g_links)
        if (l.mod == m && l.key == key) {
            if (label && *label) l.label = label;
            if (description) l.description = description;
            if (label && *label) l10n_note(m, std::string("row.") + key + ".label", label);
            if (description) l10n_note(m, std::string("row.") + key + ".description", description);
            return 1;
        }
    log_line(m->name, std::string("set_setting_text: there's no row called ") + key);
    return 0;
}

int pages_add(FH2Mod* m, const FH2Page* p) {
    if (!m || !p) return 0;
    if (p->size < offsetof(FH2Page, user) + sizeof(void*)) {
        log_line(m->name, "add_page: the FH2Page's size is too small (set size = sizeof(FH2Page))");
        return 0;
    }
    if (!valid_key(p->key)) {
        log_line(m->name, std::string("add_page: \"") + (p->key ? p->key : "") + "\" isn't a usable key (letters, digits, _)");
        return 0;
    }
    if (!p->title || !*p->title) {
        log_line(m->name, std::string("add_page: ") + p->key + " needs a title");
        return 0;
    }
    std::scoped_lock lk{g_mu};
    for (const ModPage& pg : g_pages)
        if (pg.mod == m && pg.key == p->key) {
            log_line(m->name, std::string("add_page: there's already a page called ") + p->key);
            return 0;
        }
    if (g_pages.size() >= kMaxPages) {
        log_line(m->name, std::string("add_page: no room for ") + p->key + " (" + std::to_string(kMaxPages) + " pages at most)");
        return 0;
    }
    int tile = -1;
    if (p->tile && *p->tile && g_written) {
        log_line(m->name, std::string("add_page: ") + p->key + " was added after the pages were written; its tile only works from the next launch "
                                                               "(add pages from fh2mod_init)");
    } else if (p->tile && *p->tile) {
        for (std::size_t t = 0; t < kTileCount; ++t)
            if (_stricmp(kTiles[t].name, p->tile) == 0) tile = static_cast<int>(t);
        if (tile < 0) {
            log_line(m->name, std::string("add_page: there's no tile called ") + p->tile + "; " + p->key + " is only on its mod's page under MODS > YOUR MODS");
        } else if (g_tile_page[tile] >= 0) {
            const ModPage& other = g_pages[g_tile_page[tile]];
            log_line(m->name, std::string("add_page: the ") + kTiles[tile].name + " tile already opens " + other.mod->name + "'s page; " + p->key +
                                  " is only on its mod's page under MODS > YOUR MODS");
            tile = -1;
        }
    }
    ModPage& pg    = g_pages.emplace_back();
    pg.mod         = m;
    pg.key         = p->key;
    pg.title       = p->title;
    pg.label       = p->label && *p->label ? p->label : p->title;
    pg.description = p->description ? p->description : "";
    l10n_note(m, "page." + pg.key + ".title", p->title);   // tr()'s text follows the game's language (API 10)
    l10n_note(m, "page." + pg.key + ".label", p->label && *p->label ? p->label : p->title);
    l10n_note(m, "page." + pg.key + ".description", p->description);
    pg.tile        = tile;
    pg.on_open     = p->on_open;
    pg.on_select   = p->on_select;
    pg.user        = p->user;
    if (p->size >= offsetof(FH2Page, on_slot) + sizeof(void*)) {
        pg.on_button = p->on_button;
        pg.on_slot   = p->on_slot;
    }
    if (tile >= 0) g_tile_page[tile] = static_cast<int>(g_pages.size() - 1);
    log_line(m->name, "page: " + pg.title + (tile >= 0 ? std::string(", on the ") + kTiles[tile].name + " tile" : std::string()) +
                          (g_written ? "; the pages are already written, so it shows from the next launch" : ""));
    return 1;
}

int pages_set_rows(FH2Mod* m, const char* key, const FH2PageRow* rows, uint32_t count) {
    if (!m || !key || (count && !rows)) return 0;
    if (count > kMaxPageRows) {
        log_line(m->name, std::string("set_page_rows: ") + key + " gets its first " + std::to_string(kMaxPageRows) + " rows of " +
                              std::to_string(count));
        count = kMaxPageRows;
    }
    std::vector<PageRowData> data;
    data.reserve(count);
    // The rows are an array of the mod's FH2PageRow, which may be bigger (newer) or smaller (older)
    // than the platform's: step by the first row's size, and read only what it covers.
    const uint32_t stride = count ? rows[0].size : 0;
    if (count && (stride < offsetof(FH2PageRow, style) || stride % alignof(FH2PageRow) != 0)) {
        log_line(m->name, std::string("set_page_rows: ") + key + "'s rows have no usable size (set size = sizeof(FH2PageRow)); the rows weren't changed");
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const FH2PageRow& r = *reinterpret_cast<const FH2PageRow*>(reinterpret_cast<const uint8_t*>(rows) + std::size_t{i} * stride);
        if (r.size != stride) {
            log_line(m->name, std::string("set_page_rows: row ") + std::to_string(i) + " of " + key + " isn't the size of the first; the rows weren't changed");
            return 0;
        }
        PageRowData& d = data.emplace_back();
        d.key          = r.key ? r.key : "";
        d.label        = r.label ? r.label : "";
        d.value        = r.value ? r.value : "";
        d.description  = r.description ? r.description : "";
        d.footer       = r.footer ? r.footer : "";
        if (r.size >= offsetof(FH2PageRow, style) + sizeof(int) && r.style >= FH2_ROW_NORMAL && r.style <= FH2_ROW_HEADING) d.style = r.style;
        if (r.size >= offsetof(FH2PageRow, value_alert) + sizeof(int)) d.value_alert = r.value_alert != 0;
    }
    std::scoped_lock lk{g_mu};
    for (ModPage& pg : g_pages)
        if (pg.mod == m && pg.key == key) {
            // tr()'s text follows the game's language (API 10)
            for (uint32_t i = 0; i < count; ++i) {
                const FH2PageRow& r = *reinterpret_cast<const FH2PageRow*>(reinterpret_cast<const uint8_t*>(rows) + std::size_t{i} * stride);
                const std::string s = "page." + pg.key + ".row." + std::to_string(i);
                l10n_note(m, s + ".label", r.label);
                l10n_note(m, s + ".value", r.value);
                l10n_note(m, s + ".description", r.description);
                l10n_note(m, s + ".footer", r.footer);
            }
            pg.rows = std::move(data);
            return 1;
        }
    log_line(m->name, std::string("set_page_rows: there's no page called ") + key);
    return 0;
}

namespace {
// Whether `xaml` has a Button, Slider or SpinnerControl named `name`.
bool focusable_in(const std::string& xaml, const std::string& name) {
    const std::string attr = "x:Name=\"" + name + "\"";
    for (std::size_t at = xaml.find(attr); at != std::string::npos; at = xaml.find(attr, at + 1)) {
        const std::size_t lt = xaml.rfind('<', at);
        if (lt == std::string::npos || xaml.find('>', lt) < at) continue;   // not in a tag
        std::size_t e = lt + 1;
        while (e < xaml.size() && !std::isspace(static_cast<unsigned char>(xaml[e])) && xaml[e] != '>' && xaml[e] != '/') ++e;
        const std::string tag = xaml.substr(lt + 1, e - lt - 1);
        if (tag == "Button" || tag == "Slider" || tag == "local:SpinnerControl") return true;
    }
    return false;
}

// Whether a XAML fragment's tags are balanced (quotes, comments and <?...?> allowed for): what the
// game's reader would choke on worst, caught before it gets there.
bool balanced_xml(const std::string& x, std::string& why) {
    std::vector<std::string> open;
    for (std::size_t i = 0; i < x.size();) {
        if (x[i] != '<') {
            ++i;
            continue;
        }
        if (x.compare(i, 4, "<!--") == 0) {
            const std::size_t e = x.find("-->", i + 4);
            if (e == std::string::npos) return why = "a comment isn't closed", false;
            i = e + 3;
            continue;
        }
        if (x.compare(i, 2, "<?") == 0 || x.compare(i, 2, "<!") == 0) {
            const std::size_t e = x.find('>', i);
            if (e == std::string::npos) return why = "a declaration isn't closed", false;
            i = e + 1;
            continue;
        }
        // the tag's end, past quoted attribute values
        std::size_t e = i + 1;
        char quote    = 0;
        for (; e < x.size(); ++e) {
            if (quote) {
                if (x[e] == quote) quote = 0;
            } else if (x[e] == '"' || x[e] == '\'') {
                quote = x[e];
            } else if (x[e] == '>') {
                break;
            } else if (x[e] == '<') {
                return why = "a '<' inside a tag", false;
            }
        }
        if (e >= x.size()) return why = "a tag isn't closed", false;
        const bool closing = x[i + 1] == '/', self = x[e - 1] == '/';
        std::size_t n0 = i + (closing ? 2 : 1), n1 = n0;
        while (n1 < e && !std::isspace(static_cast<unsigned char>(x[n1])) && x[n1] != '/' && x[n1] != '>') ++n1;
        const std::string name = x.substr(n0, n1 - n0);
        if (name.empty()) return why = "a tag without a name", false;
        if (closing) {
            if (open.empty() || open.back() != name) return why = "</" + name + "> doesn't match " + (open.empty() ? "anything" : "<" + open.back() + ">"), false;
            open.pop_back();
        } else if (!self) {
            open.push_back(name);
        }
        i = e + 1;
    }
    if (!open.empty()) return why = "<" + open.back() + "> isn't closed", false;
    return true;
}

int page_index(FH2Mod* m, const char* key) {   // under g_mu
    if (!m || !key) return -1;
    for (std::size_t j = 0; j < g_pages.size(); ++j)
        if (g_pages[j].mod == m && g_pages[j].key == key) return static_cast<int>(j);
    return -1;
}
} // namespace

int pages_set_xaml(FH2Mod* m, const char* key, const char* resources, const char* content, const char* help) {
    if (!m || !key) return 0;
    if (!content || !std::strstr(content, "x:Name=\"FirstItem\"")) {
        log_line(m->name, std::string("set_page_xaml: ") + key + "'s content has no element named FirstItem; the page wasn't changed");
        return 0;
    }
    for (const auto& [what, text] : {std::pair{"resources", resources}, {"content", content}, {"help", help}}) {
        std::string why;
        if (text && !balanced_xml(text, why)) {
            log_line(m->name, std::string("set_page_xaml: ") + key + "'s " + what + " isn't well-formed (" + why + "); the page wasn't changed");
            return 0;
        }
    }
    // every resource key and image the game would look up as the page loads (page_guard.cpp): one it can't find
    // crashes the game, so the page says it couldn't be shown instead
    std::string why;
    const bool fits = page_fragment_ok(resources, content, help, why);
    std::scoped_lock lk{g_mu};
    const int j = page_index(m, key);
    if (j < 0) {
        log_line(m->name, std::string("set_page_xaml: there's no page called ") + key);
        return 0;
    }
    ModPage& pg = g_pages[j];
    if (!pg.custom && pg.tile < 0) log_line(m->name, std::string("set_page_xaml: ") + key + " has no tile, so nothing opens it");
    pg.custom = true;
    if (!fits) {
        // as with a missing FirstItem: the page keeps the last XAML that passed, and only a page that never had one
        // says it couldn't be shown
        if (!pg.xaml_content.empty()) {
            log_line(m->name, std::string("set_page_xaml: ") + key + " was refused: " + why + ". The page keeps the XAML it had");
        } else {
            pg.xaml_refused = true;
            log_line(m->name, std::string("set_page_xaml: ") + key + " was refused: " + why +
                                  ". The page shows \"This page couldn't be shown\" until its XAML is set again");
        }
        return 0;
    }
    pg.xaml_refused   = false;
    pg.xaml_resources = resources ? resources : "";
    pg.xaml_content   = content;
    pg.xaml_help      = help ? help : "";
    return 1;
}

int pages_get_slot(FH2Mod* m, const char* key, int slot) {
    if (slot < 0 || slot >= FH2_PAGE_SLOTS) return 0;
    std::scoped_lock lk{g_mu};
    const int j = page_index(m, key);
    return j < 0 ? 0 : g_pages[j].slots[slot].load();
}

int pages_set_slot(FH2Mod* m, const char* key, int slot, int value) {
    if (slot < 0 || slot >= FH2_PAGE_SLOTS) return 0;
    int j = -1, old = 0;
    {
        std::scoped_lock lk{g_mu};
        j = page_index(m, key);
        if (j < 0) return 0;
        old = g_pages[j].slots[slot].exchange(value);
    }
    // on the UI thread, in a callback for the page that's up: its bindings see it now (not the
    // slot a binding is setting right now, from on_slot: that binding is in the middle of it)
    if (old != value && g_in_callback && g_custom_shown.load() == j) notify_slot(slot, slot != g_setting_slot);
    return 1;
}

int pages_set_tile_colors(FH2Mod* m, const char* key, uint32_t background, uint32_t text) {
    std::scoped_lock lk{g_mu};
    const int j = page_index(m, key);
    if (j < 0) return 0;
    ModPage& pg = g_pages[j];
    if (pg.tile < 0 || g_tile_page[pg.tile] != j) {
        log_line(m->name, std::string("set_page_tile_colors: ") + key + " isn't on a tile");
        return 0;
    }
    pg.tile_back = background;
    pg.tile_fore = text;
    return 1;
}

int pages_set_tile_text(FH2Mod* m, const char* key, const char* text) {
    std::string clean;
    for (const char* c = text ? text : ""; *c && clean.size() < 40; ++c)
        clean += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
    std::scoped_lock lk{g_mu};
    const int j = page_index(m, key);
    if (j < 0) return 0;
    ModPage& pg = g_pages[j];
    if (pg.tile < 0 || g_tile_page[pg.tile] != j) {
        log_line(m->name, std::string("set_page_tile_text: ") + key + " isn't on a tile");
        return 0;
    }
    pg.tile_text = clean;
    return 1;
}

int pages_focus(FH2Mod* m, const char* key, const char* name) {
    if (!name || !*name || !g_in_callback || !g_callback_page) return 0;
    {
        std::scoped_lock lk{g_mu};
        const int j = page_index(m, key);
        if (j < 0 || j != g_custom_shown.load()) return 0;
        // only a control the page's XAML (as written for this opening) names: focusing goes
        // through the element's input interface, which other things named there don't have
        if (!focusable_in(g_pages[j].xaml_written, name)) {
            log_line(m->name, std::string("focus_page_element: ") + name + " isn't a Button, Slider or SpinnerControl on page " + key);
            return 0;
        }
    }
    if (!focus_named(g_callback_page, name)) {
        log_line(m->name, std::string("focus_page_element: no element called ") + name + " on page " + key);
        return 0;
    }
    g_focus_again = name;   // again when the button comes up: a list that has only just shown may not have taken it
    return 1;
}

// ---- message boxes, page help and hint lines (API 10) -------------------------------------------

namespace {

// The mod's page whose hint line is up (the panel's bindings read it): a page on its tile, or a page view on YOUR
// MODS; -1 for none. On the UI thread.
int hint_page_now() {
    if (const int c = g_custom_shown.load(); c >= 0) return c;
    const int v    = g_view.load();
    const int kind = view_kind(v);
    if ((kind == kViewPage || kind == kViewPageSelect) && v >= 0 && static_cast<std::size_t>(v) < g_view_page.size()) return g_view_page[v];
    return -1;
}

// Whether `page` is one of the platform's pages that's up: YOUR MODS, a mod's page on its tile, or the stand-in
// list (a setting's on_change and a slot's on_slot name the options-class page loaded last, g_last_hud_page).
bool page_known_up(void* page) {
    if (!page) return false;
    if (page == g_installed_page.load() || page == g_mods_options_page.load()) return true;
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (page == g_tile_page_obj[t].load()) return true;
    return false;
}

// Whether page j is up now with the hint line, its bindings having read it (g_panel_obj, cleared as each page is
// prepared or the menus behind are wired): a view of YOUR MODS always has it; a page on its tile only when it was
// written with hints. Otherwise there's no live object to tell, and the page reads the hint the next time it opens.
bool hint_panel_live(int j) {
    if (j < 0 || hint_page_now() != j || !g_panel_obj.load()) return false;
    if (g_custom_shown.load() != j) return g_installed_page.load() != nullptr;
    std::scoped_lock lk{g_mu};
    for (std::size_t t = 0; t < kTileCount; ++t)
        if (g_tile_page[t] == j) return g_tile_has_panel[t].load() && g_tile_page_obj[t].load() != nullptr;
    return false;
}

// Page j's hint now, "" for none; its kind into `kind` when there's one.
std::string page_hint_text(int j, uint32_t* kind) {
    std::scoped_lock lk{g_mu};
    if (j < 0 || static_cast<std::size_t>(j) >= g_pages.size()) return "";
    const ModPage& pg = g_pages[j];
    if (pg.hint < 0 || static_cast<std::size_t>(pg.hint) >= pg.hints.size()) return "";
    if (kind) *kind = pg.hint_kind;
    return pg.hints[pg.hint];
}

const char* hint_kind_name(uint32_t kind) {
    switch (kind) {
        case FH2_HINT_ATTENTION: return "attention";
        case FH2_HINT_FAULT: return "fault";
        default: return "info";
    }
}

std::string box_refusal(const UiCallbackState* cb, FH2Mod* m) {
    if (!cb || cb->mod != m)
        return "boxes show only from a callback of this mod's pages, settings or title-screen entries (on_open, on_select, on_button, "
               "on_slot, on_change, a title entry's on_press or on_choose); send the player to its page instead";
    if (cb->kind == UiCb::box) return "a box isn't shown from another box's on_choose";
    if (cb->kind == UiCb::page_open_all)
        return "this on_open runs as YOUR MODS opens, for every page at once, before the page itself is shown; show the box from the page's on_select instead";
    for (const UiCallbackState* s = cb; s; s = s->outer)
        if (s->box_shown) return "one box at a time, and this callback (or one it was called from or called) has shown one already";
    const ULONGLONG now = GetTickCount64();
    if (g_box_pending >= 0 && now - g_box_pending_at < kBoxPendingMs) return "one box at a time, and another is waiting for its page to open";
    if (g_box_up >= 0 && now - g_box_shown_at < kBoxSettleMs) return "one box at a time, and another was shown a moment ago";
    if (stopped_online(m)) return "this mod was stopped for online play";
    if (!g_popup_ok) return "the game's message box code isn't as expected this session";
    if (cb->kind != UiCb::page_open && !cb->page) return "the platform can't tell which of the game's pages is up";
    if ((cb->kind == UiCb::setting || cb->kind == UiCb::page) && !page_known_up(cb->page))
        return "the page this callback came from isn't one the platform knows is up";
    return "";
}

} // namespace

int settings_show_box(FH2Mod* m, const FH2MessageBox* box) {
    if (!m) return 0;
    BoxSpec spec;
    std::string why;
    if (!read_box(box, spec, why)) {
        log_line(m->name, "show_message_box: " + why + ", so it isn't shown");
        return 0;
    }
    UiCallbackState* cb = ui_callback_now();
    if (const std::string no = box_refusal(cb, m); !no.empty()) {
        log_line(m->name, "show_message_box: " + no + ", so " + (spec.key.empty() ? std::string("the box") : "box " + spec.key) + " isn't shown");
        return 0;
    }
    if (!spec.cut.empty()) {
        std::string what;
        for (const std::string& c : spec.cut) what += (what.empty() ? "" : ", ") + c;
        log_line(m->name, "show_message_box: " + spec.key + ": " + what + " cut to fit, with an ellipsis");
    }
    if (!g_loc_previous) {
        g_loc_previous = loc_string(L"Previous");   // in the game's language as it's shown (tr_w)
        g_loc_next     = loc_string(L"Next");
    }
    ModBox& b       = g_boxes.emplace_back();
    const int idx   = static_cast<int>(g_boxes.size() - 1);
    b.mod           = m;
    b.spec          = spec;
    b.shown_title   = upper_game(spec.title);   // the game makes capitals too (A_StringToUpper), but not in every language
    b.title         = loc_string(keep_w(b.shown_title));
    b.message       = loc_string(keep_w(spec.message));
    b.back          = loc_string(L"Back");     // in the game's language as it's shown (tr_w), as title lists' Back
    for (const std::string& c : spec.choices) b.choices.push_back(loc_string(keep_w(c)));
    const int pages = box_pages(spec.choices.size());
    for (int p = 0; pages > 1 && p < pages; ++p) {   // "PICK ONE  2/3", as title lists' pages
        const std::string numbered = b.shown_title + "  " + std::to_string(p + 1) + "/" + std::to_string(pages);
        b.page_titles.push_back(loc_string(keep_w(numbered)));
    }
    b.page_index = cb->page_index;
    b.faults     = ui_mod_faults(m);
    for (UiCallbackState* s = cb; s; s = s->outer) s->box_shown = true;   // the callbacks it's inside can't show one either
    const std::string what = " (key " + spec.key + ", " + std::to_string(spec.choices.size()) + (spec.choices.size() == 1 ? " choice" : " choices") +
                             " and Back" + (pages > 1 ? ", in " + std::to_string(pages) + " pages" : std::string()) + ")";
    if (spec.focus) log_line(m->name, "show_message_box: " + spec.key + " opens on the page with choice " + std::to_string(spec.focus) +
                                          "; the game's box puts the focus on that page's first item");
    if (cb->kind == UiCb::page_open) {   // the page isn't there yet: shown once it has loaded
        g_box_pending     = idx;
        g_box_pending_for = cb->page_index;
        g_box_pending_at  = GetTickCount64();
        log_line(m->name, "message box waiting for its page to open: " + b.shown_title + what);
        return 1;
    }
    if (!show_box_page(cb->page, idx, box_page_of(spec.choices.size(), spec.focus))) {
        log_line(m->name, "show_message_box: the game's box couldn't be shown, so " + spec.key + " isn't");
        return 0;
    }
    log_line(m->name, "message box shown: " + b.shown_title + what);
    return 1;
}

int pages_set_help(FH2Mod* m, const char* key, const FH2Prompt* prompts, uint32_t count) {
    if (!m) return 0;
    std::vector<std::string> refused, notes;
    std::vector<HelpPrompt> help;
    std::string bar;
    {
        std::scoped_lock lk{g_mu};
        const int j = page_index(m, key);
        if (j < 0) {
            log_line(m->name, std::string("set_page_help: there's no page called ") + (key ? key : ""));
            return 0;
        }
        ModPage& pg = g_pages[j];
        help        = accept_prompts(prompts, count, {pg.custom, pg.on_select != nullptr, pg.on_button != nullptr}, refused, notes);
        for (std::size_t i = 0; prompts && i < count; ++i)   // tr()'s text follows the game's language (API 10)
            if (prompts[i].glyph >= FH2_GLYPH_A && prompts[i].glyph <= FH2_GLYPH_RB && !prompts[i].word)
                l10n_note(m, "page." + pg.key + ".help." + glyph_name(prompts[i].glyph), prompts[i].label);
        if (!help.empty() && pg.custom && !pg.xaml_help.empty()) notes.push_back("its help fragment from set_page_xaml isn't used while it has these");
        pg.help = help;
    }
    for (const std::string& r : refused) log_line(m->name, std::string("set_page_help: ") + key + "'s " + r + ", so it's left out");
    for (const std::string& n : notes) log_line(m->name, std::string("set_page_help: ") + key + ": " + n);
    static const char* const kWords[] = {"", "Select", "Back", "Accept", "Toggle", "On", "Off", "Previous", "Next"};
    for (const HelpPrompt& h : help)
        bar += std::string(bar.empty() ? "" : ", ") + "(" + glyph_name(h.glyph) + ") " + (h.word ? kWords[h.word] + std::string(" (the game's word)") : h.label);
    log_line(m->name, std::string("set_page_help: ") + key + "'s help bar: " + (bar.empty() ? "the platform's own" : bar) + ", from the next time it opens");
    return static_cast<int>(help.size());
}

int pages_set_hints(FH2Mod* m, const char* key, const char* const* texts, uint32_t count) {
    if (!m) return 0;
    std::vector<std::string> notes;
    const std::vector<std::string> hints = read_hints(texts, count, notes);
    {
        std::scoped_lock lk{g_mu};
        const int j = page_index(m, key);
        if (j < 0) {
            log_line(m->name, std::string("set_page_hints: there's no page called ") + (key ? key : ""));
            return 0;
        }
        ModPage& pg = g_pages[j];
        for (std::size_t i = 0; i < hints.size(); ++i) l10n_note(m, "page." + pg.key + ".hint." + std::to_string(i), texts[i]);
        pg.hints = hints;
        if (pg.hint >= static_cast<int>(hints.size())) pg.hint = -1;
    }
    for (const std::string& n : notes) log_line(m->name, std::string("set_page_hints: ") + key + ": " + n);
    log_line(m->name, std::string("set_page_hints: ") + key + " has " + std::to_string(hints.size()) + (hints.size() == 1 ? " hint" : " hints"));
    return 1;
}

int pages_show_hint(FH2Mod* m, const char* key, int index, uint32_t kind) {
    if (!m) return 0;
    if (kind > FH2_HINT_FAULT) {
        log_line(m->name, "show_page_hint: " + std::to_string(kind) + " isn't a hint kind (FH2_HINT_INFO, _ATTENTION or _FAULT)");
        return 0;
    }
    int j = -1;
    std::string text;
    {
        std::scoped_lock lk{g_mu};
        j = page_index(m, key);
        if (j < 0) {
            log_line(m->name, std::string("show_page_hint: there's no page called ") + (key ? key : ""));
            return 0;
        }
        ModPage& pg = g_pages[j];
        if (index < -1 || index >= static_cast<int>(pg.hints.size())) {
            log_line(m->name, std::string("show_page_hint: ") + key + " has no hint " + std::to_string(index) + " (it has " + std::to_string(pg.hints.size()) +
                                  "; set them with set_page_hints)");
            return 0;
        }
        pg.hint      = index;
        pg.hint_kind = kind;
        if (index >= 0) text = pg.hints[index];
    }
    // At once from the page's own callbacks while it's up (a box's on_choose over it included): its bindings read the
    // panel again. Otherwise it shows the next time the page opens, when they're read anyway.
    const UiCallbackState* cb = ui_callback_now();
    const bool now = cb && cb->mod == m && cb->page_index == j && cb->kind != UiCb::page_open && cb->kind != UiCb::page_open_all && hint_panel_live(j);
    // (from its on_open the page is about to read it anyway)
    const bool opening = cb && cb->mod == m && cb->page_index == j && (cb->kind == UiCb::page_open || cb->kind == UiCb::page_open_all);
    log_line(m->name, std::string("page ") + key + "'s hint line: " + (index < 0 ? std::string("none") : text + " (" + hint_kind_name(kind) + ")") +
                          (now || opening ? "" : ", from the next time it opens"));
    if (now) notify_panel(nullptr);
    return 1;
}

int title_add(FH2Mod* m, const FH2TitleEntry* e) {
    if (!m || !e) return 0;
    if (e->size < offsetof(FH2TitleEntry, button) + sizeof(int)) {
        log_line(m->name, "add_title_entry: the FH2TitleEntry's size is too small (set size = sizeof(FH2TitleEntry))");
        return 0;
    }
    if (!valid_key(e->key)) {
        log_line(m->name, std::string("add_title_entry: \"") + (e->key ? e->key : "") + "\" isn't a usable key (letters, digits, _)");
        return 0;
    }
    if (!e->label || !*e->label) {
        log_line(m->name, std::string("add_title_entry: ") + e->key + " needs a label");
        return 0;
    }
    // a list: choices (from the struct's newer part), each with a label, and on_choose
    const bool has_list = e->size >= offsetof(FH2TitleEntry, list_message) + sizeof(const char*) && e->choices;
    std::vector<std::string> choices;
    if (has_list) {
        if (e->choice_count < 1 || e->choice_count > FH2_TITLE_MAX_CHOICES) {
            log_line(m->name, std::string("add_title_entry: ") + e->key + " can have 1 to " + std::to_string(FH2_TITLE_MAX_CHOICES) +
                                  " choices, not " + std::to_string(e->choice_count));
            return 0;
        }
        for (uint32_t i = 0; i < e->choice_count; ++i) {
            if (!e->choices[i] || !*e->choices[i]) {
                log_line(m->name, std::string("add_title_entry: ") + e->key + "'s choice " + std::to_string(i) + " has no label");
                return 0;
            }
            choices.emplace_back(e->choices[i]);
        }
        if (!e->on_choose) {
            log_line(m->name, std::string("add_title_entry: ") + e->key + " has choices but no on_choose");
            return 0;
        }
    } else if (!e->on_press) {
        log_line(m->name, std::string("add_title_entry: ") + e->key + " needs an on_press");
        return 0;
    }
    if (g_written) {
        log_line(m->name, std::string("add_title_entry: ") + e->key + " came after the title screen was written; add entries from fh2mod_init");
        return 0;
    }
    if (e->button && !button_letters(e->button)) {
        log_line(m->name, std::string("add_title_entry: ") + e->key + "'s button has to be FH2_BUTTON_Y, _X, _RB or _LB (or 0 for the next free one)");
        return 0;
    }
    std::scoped_lock lk{g_mu};
    auto owner = [](int b) -> const TitleEntry* {
        for (const TitleEntry& t : g_titles)
            if (t.button == b) return &t;
        return nullptr;
    };
    for (const TitleEntry& t : g_titles)
        if (t.mod == m && t.key == e->key) {
            log_line(m->name, std::string("add_title_entry: there's already an entry called ") + e->key);
            return 0;
        }
    int button = 0;
    if (e->button && !owner(e->button)) button = e->button;
    for (int b : kTitleButtons)
        if (!button && !owner(b)) button = b;
    if (!button) {
        log_line(m->name, std::string("add_title_entry: no button left for ") + e->key + " (four entries at most, from every mod)");
        return 0;
    }
    if (e->button && button != e->button) {
        const TitleEntry* o = owner(e->button);
        log_line(m->name, std::string("add_title_entry: ") + button_letters(e->button) + " is " + o->mod->name + "'s " + o->key + "; " + e->key +
                              " gets " + button_letters(button));
    }
    std::string label = e->label;
    if (text_chars(label) > kMaxTitleLabel) {   // characters, not bytes: a Polish letter is one, as an English one
        label = cut_text(label, kMaxTitleLabel);
        log_line(m->name, std::string("add_title_entry: ") + e->key + "'s label is cut to \"" + label + "\"");
    }
    l10n_note(m, std::string("title.") + e->key + ".label", e->label);   // tr()'s text follows the game's language (API 10)
    TitleEntry& t = g_titles.emplace_back();
    t.mod         = m;
    t.key         = e->key;
    t.label       = label;
    t.on_press    = e->on_press;
    t.user        = e->user;
    t.button      = button;
    if (has_list) {
        // the strings stay where they are: the entry doesn't move once it's in g_titles
        t.on_choose    = e->on_choose;
        t.choices      = std::move(choices);
        t.list_title   = widen(e->list_title && *e->list_title ? e->list_title : label.c_str());
        t.list_message = widen(e->list_message ? e->list_message : "");
        for (const std::string& c : t.choices) t.wchoices.push_back(widen(c));
        t.wchoices.push_back(L"Back");   // in the game's language as it's shown (tr_w)
        t.loc.push_back(loc_string(t.list_title.c_str()));
        t.loc.push_back(loc_string(t.list_message.c_str()));
        for (const std::wstring& c : t.wchoices) t.loc.push_back(loc_string(c.c_str()));
        // tr()'s text, in the game's language as the list shows (API 10)
        // without a list_title it's the label, cut to kMaxTitleLabel as the label is
        const bool own_title    = e->list_title && *e->list_title;
        const char* given_title = own_title ? e->list_title : e->label;
        const std::size_t cut   = own_title ? 0 : kMaxTitleLabel;
        l10n_note_w(m, t.list_title.c_str(), given_title, {}, cut);
        l10n_note_w(m, t.list_message.c_str(), e->list_message);
        for (std::size_t c = 0; c < t.choices.size(); ++c) l10n_note_w(m, t.wchoices[c].c_str(), e->choices[c]);
        if (t.choices.size() > kTitleListFits) {
            const std::size_t pages = (t.choices.size() + kTitlePageSize - 1) / kTitlePageSize;
            for (std::size_t p = 0; p < pages; ++p)
                t.page_titles.push_back(t.list_title + L"  " + std::to_wstring(p + 1) + L"/" + std::to_wstring(pages));
            for (const std::wstring& pt : t.page_titles) {
                t.page_loc.push_back(loc_string(pt.c_str()));
                l10n_note_w(m, pt.c_str(), given_title, pt.substr(t.list_title.size()), cut);
            }
            if (!g_loc_previous) {
                g_loc_previous = loc_string(L"Previous");
                g_loc_next     = loc_string(L"Next");
            }
        }
    }
    std::string list;
    if (has_list) {
        list = " (a list of " + std::to_string(t.choices.size());
        if (!t.page_loc.empty()) list += ", in " + std::to_string(t.page_loc.size()) + " pages of " + std::to_string(kTitlePageSize);
        list += ")";
    }
    log_line(m->name, std::string("title-screen entry: (") + button_letters(button) + ") " + upper_game(label) + list);
    return 1;
}

const char* title_started_by(FH2Mod* m) {
    const int j = g_title_started.load();
    if (!m || j < 0) return nullptr;
    std::scoped_lock lk{g_mu};
    return j < static_cast<int>(g_titles.size()) && g_titles[j].mod == m ? g_titles[j].key.c_str() : nullptr;
}

int settings_get(FH2Mod* m, const char* key) {
    if (!m || !key) return -1;
    std::scoped_lock lk{g_mu};
    const Row* r = find_row(m, key);
    return r ? r->value.load() : -1;
}

void settings_set(FH2Mod* m, const char* key, int value) {
    if (!m || !key) return;
    std::scoped_lock lk{g_mu};
    Row* r = find_row(m, key);
    if (r && !r->choices.empty()) {   // a choice row: the index
        const int last = static_cast<int>(r->choices.size()) - 1;
        r->value       = value < 0 ? 0 : value > last ? last : value;
        r->fvalue      = static_cast<float>(r->value.load());
    } else if (r && !r->slider) {
        r->value = value ? 1 : 0;
    }
}

float settings_get_slider(FH2Mod* m, const char* key) {
    if (!m || !key) return 0;
    std::scoped_lock lk{g_mu};
    const Row* r = find_row(m, key);
    return r && r->slider ? r->fvalue.load() : 0;
}

void settings_set_slider(FH2Mod* m, const char* key, float value) {
    if (!m || !key || !std::isfinite(value)) return;
    std::scoped_lock lk{g_mu};
    if (Row* r = find_row(m, key); r && r->slider) r->fvalue = snap(*r, value);
}

// The freeroam HUD (A_Hud.xaml): when a mod didn't load or start, a notice like the game's
// own small ones (HUDMinorNotificationControl's look, but high up, where the big ones go, and
// with a bigger title) saying which, and to
// look under MODS. It fades in a couple of seconds after the HUD first shows, stays nine
// seconds and goes, once a session (FH2Mod_ModNotice). With nothing wrong, the game's HUD.
// The names in a sentence: "A", "A and B", "A, B and C", "A, B, C and 2 more" (mods' names: not translated).
std::string names_text(const std::vector<std::string>& names) {
    std::string which;
    for (std::size_t i = 0; i + 1 < names.size() && i < 2; ++i) which += (i ? ", " : "") + names[i];
    if (names.size() <= 1) return names.empty() ? "" : names[0];
    if (names.size() <= 3) return trf("{0} and {1}", {which, names.back()});
    return trf("{0} and {1} more", {which + ", " + names[2], std::to_string(names.size() - 3)});
}

bool write_hud_notice() {
    std::vector<std::string> names, loaded;
    for (const ModEntry& e : g_entries) {
        if (!mod_problem(e).empty()) names.push_back(e.name);
        if (e.enabled && e.mod) loaded.push_back(e.name);
    }
    // What the notice says this session, if anything: a mod that didn't load wins; then, until YOUR MODS has been
    // opened, a welcome naming the mods (in three sessions at most); then the mods new since the launch before.
    std::string title, text;
    const int sessions = std::atoi(seen_get(kSeenPlatform, "welcome.sessions").c_str());
    if (!names.empty()) {
        g_notice_kind = kNoticeFailed;
        title = names.size() == 1 ? tr("A MOD DIDN'T LOAD") : trf("{0} MODS DIDN'T LOAD", {std::to_string(names.size())});
        text  = trf(names.size() == 1 ? "{0} isn't working. See Pause > MODS > YOUR MODS." : "{0} aren't working. See Pause > MODS > YOUR MODS.",
                    {names_text(names)});
    } else if (!loaded.empty() && !home_seen() && sessions < kWelcomeSessions) {
        g_notice_kind = kNoticeWelcome;
        title = tr("YOUR MODS ARE READY");
        text  = trf(loaded.size() == 1 ? "{0} is loaded. Pause, then MODS (the last tab), to change its settings."
                                       : "{0} are loaded. Pause, then MODS (the last tab), to change their settings.",
                    {names_text(loaded)});
    } else if (!g_fresh_names.empty()) {
        g_notice_kind = kNoticeNewMods;
        title = g_fresh_names.size() == 1 ? trf("NEW MOD: {0}", {upper_game(g_fresh_names[0])}) : trf("{0} NEW MODS", {std::to_string(g_fresh_names.size())});
        text  = g_fresh_names.size() == 1 ? tr("Its settings are under Pause > MODS > YOUR MODS.")
                                          : trf("{0}: their settings are under Pause > MODS > YOUR MODS.", {names_text(g_fresh_names)});
    }
    if (g_notice_kind == kNoticeNone && !hud_layer_wants_page()) {
        remove_leftover_file("UI/Scenes/Anthem/A_Hud.xaml");   // last session's notice
        return true;
    }
    std::string page = read_ui("Scenes/Anthem/A_Hud.xaml");
    const std::string res_end = "</ResourceDictionary>", root_end = "</local:ZPanel>";
    const std::size_t r = page.find(res_end), z = page.rfind(root_end);
    const std::size_t g = z == std::string::npos ? z : page.rfind("</Grid>", z);
    if (page.empty() || r == std::string::npos || g == std::string::npos) {
        slog("A_Hud.xaml doesn't look as expected; no notice on the freeroam HUD");
        hud_layer_splice(page);   // says why the HUD layer is left out too
        return false;
    }
    if (g_notice_kind != kNoticeNone) {   // the notice only when there's one (a HUD layer may need the page anyway)
        auto key = [](double v, double t, bool ease_in) {
            char b[256];
            std::snprintf(b, sizeof b,
                          "                                    <EasingDoubleKeyFrame KeyTime=\"0:0:%.2f\" Value=\"%.2f\">"
                          "<EasingDoubleKeyFrame.EasingFunction><CircleEase EasingMode=\"%s\" /></EasingDoubleKeyFrame.EasingFunction>"
                          "</EasingDoubleKeyFrame>\r\n",
                          t, v, ease_in ? "EaseIn" : "EaseOut");
            return std::string(b);
        };
        // in at 2s (0.5s, as the game's), held 9s, out (0.5s)
        std::string fade = "                                <DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.Opacity)\">\r\n" +
                           key(0, 0, false) + key(0, 2.0, false) + key(1, 2.5, false) + key(1, 11.5, true) + key(0, 12.0, true) +
                           "                                </DoubleAnimationUsingKeyFrames>\r\n";
        std::string scale;
        for (const char* axis : {"ScaleX", "ScaleY"})
            scale += std::string("                                <DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.") +
                     axis + ")\">\r\n" + key(0.8, 0, false) + key(0.8, 2.0, false) + key(1, 2.5, false) + key(1, 11.5, true) +
                     key(0.96, 12.0, true) + "                                </DoubleAnimationUsingKeyFrames>\r\n";
        const std::string style =
            "            <Style x:Key=\"FH2Mod_Style_ModNotice\" TargetType=\"StackPanel\">\r\n"
            "                <Style.Triggers>\r\n"
            "                    <DataTrigger Binding=\"{Binding FH2Mod_ModNotice}\" Value=\"True\">\r\n"
            "                        <DataTrigger.EnterActions>\r\n"
            "                            <BeginStoryboard>\r\n"
            "                            <Storyboard>\r\n" + fade + scale +
            "                            </Storyboard>\r\n"
            "                            </BeginStoryboard>\r\n"
            "                        </DataTrigger.EnterActions>\r\n"
            "                    </DataTrigger>\r\n"
            "                </Style.Triggers>\r\n"
            "            </Style>\r\n        ";
        const std::string notice =
            "\r\n            <!-- FH2 Mod Platform: a notice about the mods -->\r\n"
            "            <StackPanel Style=\"{StaticResource FH2Mod_Style_ModNotice}\" Opacity=\"0\" HorizontalAlignment=\"Center\" "
            "VerticalAlignment=\"Top\" Margin=\"0,220,0,0\" RenderTransformOrigin=\"0.5,0.5\">\r\n"
            "                <StackPanel.RenderTransform>\r\n"
            "                    <TransformGroup>\r\n"
            "                        <ScaleTransform ScaleX=\"0.8\" ScaleY=\"0.8\" />\r\n"
            "                    </TransformGroup>\r\n"
            "                </StackPanel.RenderTransform>\r\n"
            "                <local:FZMaterial Preset=\"{DynamicResource A_FZMaterialPresetGlassRoundtangle_HudItem}\" Color=\"#99101418\">\r\n"
            "                    <Border BorderThickness=\"2\" BorderBrush=\"#16FFFFFF\" CornerRadius=\"4\">\r\n"
            "                        <StackPanel Orientation=\"Vertical\" HorizontalAlignment=\"Center\" Margin=\"16,8,16,8\">\r\n"
            "                            <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_HudLarge}\" Text=\"" + xml_escape(title) +
            "\" Effect=\"{StaticResource A_DropShadowEffect_DefaultHud}\" HorizontalAlignment=\"Center\" TextAlignment=\"Center\" Margin=\"0,0,0,-3\" />\r\n"
            "                            <local:FZTextBlock Style=\"{StaticResource A_Style_FZTextBlock_HudMedium}\" Text=\"" + xml_escape(text) +
            "\" Effect=\"{StaticResource A_DropShadowEffect_DefaultHud}\" HorizontalAlignment=\"Center\" TextAlignment=\"Center\" />\r\n"
            "                        </StackPanel>\r\n"
            "                    </Border>\r\n"
            "                </local:FZMaterial>\r\n"
            "            </StackPanel>\r\n        ";
        page.insert(g, notice);
        page.insert(r, style);
    }
    // the HUD layer finds its own insertion points again, after the notice's
    if (!hud_layer_splice(page) && g_notice_kind == kNoticeNone) {
        remove_leftover_file("UI/Scenes/Anthem/A_Hud.xaml");   // nothing to write: the layer was left out
        hud_layer_page_written(false, page);
        return true;
    }
    const bool written = set_file(&g_owner_mod, "UI/Scenes/Anthem/A_Hud.xaml", page.data(), page.size()) != 0;
    g_hud_notice       = written && g_notice_kind != kNoticeNone;
    hud_layer_page_written(written, page);
    return written;
}

// After every mod's fh2mod_init: write the pages (the MODS tab's, and Settings > Mods if any
// mod added a row) and hook the game.
void settings_start(const FH2ModApi* api) {
    g_api = api;
    g_owner_mod.name  = kWho;
    g_owner_mod.wname = widen(kWho);
    g_hub_owner.name  = "MODS";
    g_hub_owner.wname = widen(g_hub_owner.name);
    std::size_t n = 0;
    {
        std::scoped_lock lk{g_mu};
        n = g_rows.size();
    }
    add_hub_rows();
    page_guard_start();   // before the pages are written: last session's crash marks
    g_written = true;
    const char* const files[] = {"UI/Scenes/Anthem/A_PauseOptions.xaml", "UI/Scenes/Anthem/A_ModOptions.xaml",
                                 "UI/Scenes/Anthem/A_PauseMenu.xaml",    "stateflow/pause.xml", "stateflow/ui_screens.xml",
                                 kTitleFile};
    if (game_mount().empty()) {
        slog("the game folder wasn't found; no Mods page or MODS tab");
        for (const char* f : files) remove_leftover_file(f);
        hud_layer_start();
        return;
    }
    // the MODS tab
    g_hub_files = write_pause_menu() && write_pause_flow();
    remove_leftover_file("stateflow/ui_screens.xml");
    for (int p : {kHubInstalled, kHubUpdates, kHubAbout, kHubHelp})
        if (g_hub_files) g_hub_files = write_hub_page(p);
    // the mods' pages on tiles (written now as well as when they open, as the others are)
    for (std::size_t t = 0; t < kTileCount; ++t) {
        const int p = kHubTilePage + static_cast<int>(t);
        bool has = false;
        {
            std::scoped_lock lk{g_mu};
            has = g_tile_page[t] >= 0;
        }
        if (!(g_hub_files && has && write_hub_page(p))) {
            remove_leftover_file(("UI/Scenes/Anthem/" + hub_file(p)).c_str());
            std::scoped_lock lk{g_mu};
            g_tile_page[t] = -1;
        }
    }
    remove_leftover_file("UI/Scenes/Anthem/A_ModPage_03.xaml");   // Get mods, before it opened the mod site
    remove_leftover_file("UI/Scenes/Anthem/A_ModPage_05.xaml");   // Making mods, before 2.5.0's release
    if (!g_hub_files) {
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml");
        clear_file(&g_owner_mod, "stateflow/pause.xml");
        clear_file(&g_owner_mod, "stateflow/ui_screens.xml");
    }
    // Pause > Settings > MODS, a door to YOUR MODS whenever a mod is installed, and the list that stands in for the
    // MODS tab (written now, in case the tab's hooks don't go in)
    bool mods_page = !g_entries.empty() && write_mods_page();
    g_settings_tile = mods_page && write_settings_page();
    if (!g_settings_tile) {
        // No mods this time (they were removed): take the tile back out before the game reads its
        // menus, rather than with the clean-up later.
        remove_leftover_file("UI/Scenes/Anthem/A_PauseOptions.xaml");
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseOptions.xaml");
    }
    if (!mods_page) remove_leftover_file("UI/Scenes/Anthem/A_ModOptions.xaml");
    first_run_cues(g_hub_files, g_settings_tile);
    if (!write_hud_notice()) remove_leftover_file("UI/Scenes/Anthem/A_Hud.xaml");
    write_title_page();
    slog(std::to_string(n) + " row(s) from mods, " + std::to_string(g_entries.size()) + " mod(s) on the MODS tab" +
         (g_hub_files ? "" : " (not shown)"));
    if (g_hub_files || g_settings_tile || g_title_page || hud_layer_wants_page()) std::thread(hook_game).detach();
    hud_layer_start();
}

FH2Mod* settings_owner() { return &g_owner_mod; }

// ---- the HUD layer's way in (hud_layer.hpp) -----------------------------------------------------
namespace {
void __cdecl hud_set(void*, void*, uint8_t*) {}
__attribute__((noinline)) void hud_play_sound_inner(const char* name) { play_sound_inner(name, RVA_UI_SOUNDS); }
__attribute__((noinline)) bool ui_play_sound_inner(uintptr_t base, const char* name) { return play_sound_at(base, name, RVA_UI_SOUNDS); }
} // namespace

uint8_t* settings_hud_descriptor(const char* name, void* getter, int kind) {
    static const uint8_t string_type[] = {0x48, 0x8D, 0x05, 0x59, 0xB0, 0x02, 0x02, 0xC3};
    std::scoped_lock lk{g_desc_mu};
    switch (kind) {
        case 0: return new_descriptor(name, getter, reinterpret_cast<void*>(&hud_set), nullptr);
        case 1: return new_descriptor(name, getter, reinterpret_cast<void*>(&hud_set), reinterpret_cast<TypeFn>(g_base + RVA_BOOL_TYPE)(0));
        case 2:
            if (!g_sliders_ok) return nullptr;
            return new_descriptor(name, getter, reinterpret_cast<void*>(&hud_set), nullptr, RVA_FLOAT_PROPERTY);
        case 3:
            if (!code_matches(RVA_BADGE_TYPE, string_type, sizeof string_type)) return nullptr;
            return new_descriptor(name, getter, reinterpret_cast<void*>(&hud_set), reinterpret_cast<TypeFn>(g_base + RVA_BADGE_TYPE)(0));
        default: return nullptr;
    }
}

// notify_changed and play_sound swallow a fault; these report it, so the HUD layer can log it and turn
// itself off. Only calls are inside the __try.
bool settings_notify(void* obj, void* desc, uint32_t* code) {
    __try {
        reinterpret_cast<NotifyFn>(g_base + RVA_NOTIFY_PROP)(obj, desc);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool settings_sounds_ok() { return g_sounds_ok; }
bool settings_play_sound(const char* name, uint32_t* code) {
    *code = 0;
    if (!g_sounds_ok) return false;
    __try {
        hud_play_sound_inner(name);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// play_ui_sound's way in (ui_sounds.hpp): its own checks are made, so not the MODS tab's, and it brings its own base,
// so it plays in a session where the MODS tab's hooks never went in.
bool settings_play_sound_unchecked(uintptr_t base, const char* name, uint32_t* code) {
    *code = 0;
    if (!base) return false;
    __try {
        return ui_play_sound_inner(base, name);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool settings_balanced(const std::string& xml, std::string& why) { return balanced_xml(xml, why); }
std::string settings_read_ui(const char* name) { return read_ui(name); }
int settings_properties_state() { return g_props_state.load(); }

} // namespace fh2platform

// rundll32 d3dcompiler_47.dll,RestartHelper: see restart_game.
extern "C" __declspec(dllexport) void CALLBACK RestartHelper(HWND, HINSTANCE, LPSTR, int) {
    HMODULE self = nullptr;
    wchar_t path[MAX_PATH];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&RestartHelper), &self);
    if (!self || !GetModuleFileNameW(self, path, MAX_PATH)) return;
    std::wstring file = path;
    file.erase(file.find_last_of(L"\\/") + 1);
    file += L"fh2-mods\\platform-restart.txt";
    FILE* f = _wfopen(file.c_str(), L"r, ccs=UTF-8");
    if (!f) return;
    static wchar_t line[4][2048];
    for (auto& l : line) {
        l[0] = 0;
        if (!std::fgetws(l, 2048, f)) break;
        std::size_t n = std::wcslen(l);
        while (n && (l[n - 1] == L'\n' || l[n - 1] == L'\r')) l[--n] = 0;
    }
    std::fclose(f);
    DeleteFileW(file.c_str());
    const DWORD pid = std::wcstoul(line[0], nullptr, 10);
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        const DWORD r = WaitForSingleObject(h, 120000);
        CloseHandle(h);
        if (r != WAIT_OBJECT_0) return;   // still running: the quit was cancelled
    }
    Sleep(1000);
    std::wstring cmd = L"\"" + std::wstring(line[1]) + L"\" " + line[2];
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(line[1], cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, line[3], &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// In-game settings (API 4, sliders API 5): On/Off and slider rows for mods on a Mods page of
// the game's own.
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
#include "platform.hpp"
#include "strings.hpp"
#include "updates.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fh2platform {
namespace {

const char* const kWho = "Mods page";
FH2Mod g_owner_mod;                 // owns the platform's files and hooks for this
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
};
std::mutex g_mu;                     // guards the list (rows are never removed)
std::deque<Row> g_rows;
std::atomic<bool> g_written{false};  // the pages have been written

bool valid_key(const char* k) {
    if (!k || !*k) return false;
    for (; *k; ++k)
        if (!(std::isalnum(static_cast<unsigned char>(*k)) || *k == '_')) return false;
    return true;
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

bool restart_pending() {
    for (Row& r : g_rows)
        if (r.restart && (r.slider ? std::fabs(r.fvalue.load() - r.fboot) > r.fstep * 0.5f : r.value.load() != r.boot)) return true;
    return false;
}

std::string current_values() {
    std::string v;
    for (Row& r : g_rows) v += (r.slider ? std::to_string(r.fvalue.load()) : std::to_string(r.value.load())) + ",";
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

void slider_set(Row* r, void* obj, uint8_t* value) {
    float f = 0;
    std::memcpy(&f, value + 0x28, sizeof f);
    if (!std::isfinite(f)) return;
    f                        = snap(*r, f);
    const bool pending_before = restart_pending();
    const float old          = r->fvalue.exchange(f);
    if (std::fabs(old - f) < r->fstep * 0.01f) return;
    log_line(r->mod->name, r->key + " set to " + show(*r, f) + " on the Mods page");
    if (r->on_slide && !call_on_slide(r->on_slide, r->user, r->key.c_str(), f))
        log_line(r->mod->name, "crashed in its on_slide for " + r->key);
    if (restart_pending() != pending_before && obj && g_notice_desc) notify_changed(obj, g_notice_desc);
}

void __cdecl row_set(void* desc, void* obj, uint8_t* value) {
    Row* r = row_by_desc(desc);
    if (!r || !value || !readable(value + 0x28, 4)) return;
    if (r->slider) return slider_set(r, obj, value);
    const int v              = *reinterpret_cast<int*>(value + 0x28);
    const bool pending_before = restart_pending();
    const int old            = r->value.exchange(v);
    if (old == v) return;
    log_line(r->mod->name, r->key + " set to " + std::to_string(v) + " on the Mods page");
    if (r->on_change && !call_on_change(r->on_change, r->user, r->key.c_str(), v))
        log_line(r->mod->name, "crashed in its on_change for " + r->key);
    if (restart_pending() != pending_before && obj && g_notice_desc) notify_changed(obj, g_notice_desc);
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
uint8_t* g_selected_desc = nullptr;
void** __cdecl selected_get(void** out, void*, void*) {
    int v = g_selected.load();
    *out  = nullptr;
    reinterpret_cast<BoxIntFn>(g_base + RVA_BOX_INT)(out, &v);
    return out;
}
void __cdecl selected_set(void*, void*, uint8_t* value) {
    if (value && readable(value + 0x28, 4)) {
        g_selected = *reinterpret_cast<int*>(value + 0x28);
        slog("debug: row " + std::to_string(g_selected.load()) + " selected");
    }
}
uint8_t* selected_descriptor() {
    std::scoped_lock lk{g_desc_mu};
    if (!g_selected_desc)
        g_selected_desc = new_descriptor("FH2Mod_Selected", reinterpret_cast<void*>(&selected_get), reinterpret_cast<void*>(&selected_set), nullptr);
    return g_selected_desc;
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
    if (std::wcscmp(n, L"FH2Mod_Selected") == 0) return selected_descriptor();
    Row* r = row_by_prop(n);
    return r ? descriptor_for(r) : nullptr;
}

// ---- the pages ----------------------------------------------------------------------------

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    return o;
}

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

// A page laid out like the HUD options page (A_HUDOptions.xaml), with a list of our own. An item
// is one of our rows, or a line of information: a row with a label and a description and no
// value to change.
struct PageItem {
    int row = -1;                    // index in g_rows, or -1 for a line of information
    std::string label, description;
    std::string value;               // a line of information: shown on the right
    int button = -1;                 // a line of information that opens something: its button number
};

// A line of information is a Button laid out like a row: label left, value right. The first
// row is always x:Name="FirstItem" (the page focuses it); so is a button there.
std::string button_name(const PageItem& it, bool first) {
    if (first) return "FirstItem";
    return it.button >= 0 ? "PART_Button_FH2Mod_" + std::to_string(it.button) : "";
}

FH2Mod g_hub_owner;                  // the MODS tab's own rows (a switch per mod)

bool write_list_page(const char* file, const std::string& title, const std::vector<PageItem>& items, bool track_selection = false) {
    std::string page = read_ui("Scenes/Anthem/A_HUDOptions.xaml");
    if (page.empty()) return false;
    std::string res = "\r\n            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowNotice\" UseHidden=\"true\" InvertResult=\"false\" />\r\n"
                      "            <ControlTemplate x:Key=\"FH2Mod_InfoRow\" TargetType=\"{x:Type Button}\">\r\n"
                      "                <Grid VerticalAlignment=\"Center\">\r\n"
                      "                    <Grid.ColumnDefinitions>\r\n"
                      "                        <ColumnDefinition />\r\n"
                      "                        <ColumnDefinition />\r\n"
                      "                    </Grid.ColumnDefinitions>\r\n"
                      "                    <Grid Grid.Column=\"0\">\r\n"
                      "                        <local:FZTextBlock Text=\"{Binding Tag.Heading, RelativeSource={RelativeSource TemplatedParent}}\" "
                      "Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Left\" />\r\n"
                      "                    </Grid>\r\n"
                      "                    <Grid Grid.Column=\"1\">\r\n"
                      "                        <local:FZTextBlock Text=\"{Binding Tag.Icon, RelativeSource={RelativeSource TemplatedParent}}\" "
                      "Style=\"{StaticResource A_Style_SliderControlText}\" HorizontalAlignment=\"Center\" />\r\n"
                      "                    </Grid>\r\n"
                      "                </Grid>\r\n"
                      "            </ControlTemplate>\r\n";
    std::string rows = "\r\n";
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t n = 0; n < items.size(); ++n) {
            const PageItem& it = items[n];
            const std::string first = n == 0 ? " x:Name=\"FirstItem\"" : "";
            const std::string key   = "FH2Mod_Item_" + std::to_string(n);
            if (it.row < 0 || static_cast<std::size_t>(it.row) >= g_rows.size()) {
                // the row's value rides in the data item's Icon, a string the template shows
                res += "            <local:TileMenuDataItem x:Key=\"" + key + "\" Heading=\"" + xml_escape(it.label) + "\" Description=\"" +
                       xml_escape(it.description) + "\" Icon=\"" + xml_escape(it.value) + "\" />\r\n";
                const std::string name = button_name(it, n == 0);
                rows += "                                        <Button Tag=\"{StaticResource " + key +
                        "}\" Template=\"{StaticResource FH2Mod_InfoRow}\"" + (name.empty() ? "" : " x:Name=\"" + name + "\"") +
                        " />\r\n";
                continue;
            }
            const Row& r     = g_rows[it.row];
            const std::string label = it.label.empty() ? r.label : it.label;
            std::string desc = it.description.empty() ? r.description : it.description;
            if (r.restart) desc += (desc.empty() ? "" : " ") + std::string("Takes effect the next time you start the game.");
            if (r.mod != &g_hub_owner) desc += (desc.empty() ? "" : " ") + std::string("(") + r.mod->name + ")";
            res += "            <local:TileMenuDataItem x:Key=\"" + key + "\" Heading=\"" + xml_escape(label) + "\" Description=\"" +
                   xml_escape(desc) + "\" />\r\n";
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
    res += "        ";
    rows += "                                ";
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
    const std::size_t t = page.find(title_attr);
    page.replace(t, title_attr.size(), "Content=\"" + xml_escape(title) + "\"");
    // Extra Options adds its rows to pages declared x:Class="ForzaUI.A_HudOptions" as the game
    // loads them; a space before the '=' is the same XML but keeps them on the HUD page.
    const std::string cls = "x:Class=\"ForzaUI.A_HudOptions\"";
    if (const std::size_t c = page.find(cls); c != std::string::npos) page.replace(c, cls.size(), "x:Class =\"ForzaUI.A_HudOptions\"");
    // no Y (reset to defaults) on this page
    const std::size_t y = page.find(help_y), y_end = page.find('"', page.find('"', y) + 1);
    page.erase(y, y_end + 1 - y);
    // the restart notice, under the list and the description; hidden (not collapsed) while
    // there's nothing to say, so the page doesn't move when it shows
    const std::size_t row_end = page.find("</StackPanel>", page.find(desc_panel)) + std::string("</StackPanel>").size();
    page.insert(row_end,
                "\r\n                    <local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_Panel_Background}\" Margin=\"0,6,0,0\"\r\n"
                "                                      Visibility=\"{Binding FH2Mod_RestartNotice, Converter={StaticResource FH2Mod_ShowNotice}}\">\r\n"
                "                        <Border Margin=\"20,14,20,14\">\r\n"
                "                            <local:FZTextBlock Text=\"Some of these changes take effect the next time you start the game.\"\r\n"
                "                                               Color=\"{StaticResource A_Color_SubHeader_Text}\"\r\n"
                "                                               Style=\"{StaticResource A_Style_FZTextBlock_Body_Tight}\" TextWrapping=\"Wrap\" />\r\n"
                "                        </Border>\r\n"
                "                    </local:FZMaterial>");
    return set_file(&g_owner_mod, (std::string("UI/Scenes/Anthem/") + file).c_str(), page.data(), page.size()) != 0;
}

// The Mods page (Settings > Mods, and the MODS tab's All settings): every mod's rows.
bool write_mods_page() {
    std::vector<PageItem> items;
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t i = 0; i < g_rows.size(); ++i)
            if (g_rows[i].mod != &g_hub_owner) items.push_back({static_cast<int>(i), "", ""});
    }
    return write_list_page("A_ModOptions.xaml", "MODS", items);
}

// Pause > Settings: a sixth column with a tall MODS tile.
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
    page.insert(c,
                "\r\n                                <Button x:Name=\"PART_Button_Mods\"\r\n"
                "                                        local:TileMenuNavigationManager.TileState=\"Enabled\"\r\n"
                "                                        DataContext=\"{StaticResource FH2Mod_MenuDataItem_Mods}\"\r\n"
                "                                        local:TileMenuNavigationManager.GridX=\"5\"\r\n"
                "                                        local:TileMenuNavigationManager.GridY=\"0\"\r\n"
                "                                        Style=\"{DynamicResource A_Style_Button_Tile_1x2}\"\r\n"
                "                                        Canvas.Left=\"{StaticResource A_Double_TileCoord_X_5}\"\r\n"
                "                                        Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_0}\"\r\n"
                "                                        Template=\"{StaticResource A_ControlTemplate_VerticalIconTile}\"\r\n"
                "                                        local:UIPageTransitionManager.TransitionIndex=\"6\"\r\n"
                "                                        local:BindingProxy.C=\"DisableMotionBlur\"/>\r\n"
                "                            ");
    page.insert(i_end + 1,
                "            <local:TileMenuDataItem x:Key=\"FH2Mod_MenuDataItem_Mods\" Heading=\"MODS\" "
                "Icon=\"/ForzaUI;component/Textures/Anthem/Icons/TiledMenus/Tuning1x1.png\" />\r\n");
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
std::mutex g_page_mu;                // one page written at a time

enum HubPage { kHubInstalled = 1, kHubUpdates, kHubGetMods, kHubAbout, kHubMaking, kHubAllSettings, kHubModPage = 10 };
constexpr std::size_t kMaxModPages = 64;   // A_ModPage_10 .. A_ModPage_73: mod n's page is 10 + n

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

void add_hub_rows() {
    g_entries = mod_entries();
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
    }
}

std::string with_v(const std::string& version) { return version.empty() ? "" : " " + version; }

// "Up to date.", "Version 1.3.0 is out.", ...
std::string update_status(const ModEntry& e) {
    if (!e.enabled) return "";
    if (!e.mod) return e.platform_mod ? "" : "It doesn't check for updates.";
    if (e.mod->repo.empty() || !e.mod->check_updates) return "It doesn't check for updates.";
    const std::string latest = latest_known(e.mod);
    if (latest.empty()) return "Not checked for updates yet.";
    if (compare_versions(latest, e.version) > 0) return "Version " + latest + " is out: github.com/" + e.mod->repo + "/releases";
    return "Up to date.";
}

std::string alnum_lower(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

const ModEntry* installed(const char* key) {
    for (const ModEntry& e : g_entries) {
        std::string stem = narrow(e.dll);
        if (stem.size() > 4) stem.resize(stem.size() - 4);
        if (alnum_lower(e.name).find(key) != std::string::npos || alnum_lower(stem).find(key) != std::string::npos) return &e;
    }
    return nullptr;
}

// Extra Options' log starts with "<name> <version> - ..."; empty when it isn't loaded from the
// game's folder.
std::string extra_options_line() {
    HMODULE w = GetModuleHandleW(L"winmm.dll");
    wchar_t path[MAX_PATH], sys[MAX_PATH];
    if (!w || !GetModuleFileNameW(w, path, MAX_PATH)) return "";
    const UINT s = GetSystemDirectoryW(sys, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path{path}.parent_path();
    if (s && s < MAX_PATH && lstrcmpiW(dir.c_str(), sys) == 0) return "";
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

// one line about a mod: what it does, whether it's loaded, its update status
std::string about_mod(const ModEntry& e) {
    std::string d = e.description.empty() ? "" : e.description + " ";
    if (!e.enabled) d += "Turned off: the game doesn't load it. ";
    else if (!e.platform_mod) d += "A mod from before the platform: it has no settings here. ";
    if (const std::string u = update_status(e); !u.empty()) d += u + " ";
    if (!d.empty()) d.pop_back();
    return d;
}

// Installed mods: one row per mod; A opens its page.
std::vector<PageItem> installed_items() {
    std::vector<PageItem> items;
    for (std::size_t i = 0; i < g_entries.size(); ++i) {
        const ModEntry& e = g_entries[i];
        PageItem it   = info(e.name, about_mod(e) + (about_mod(e).empty() ? "" : " ") + "Press A for its settings.");
        it.value      = (e.enabled ? "" : "Off  ") + (e.version.empty() ? std::string() : e.version);
        it.button     = i < kMaxModPages ? static_cast<int>(i) : -1;
        items.push_back(it);
    }
    if (g_entries.empty())
        items.push_back(info("No mods installed", "Mods go in the fh2-mods folder next to azimuth.exe. See GET MODS for some to try."));
    return items;
}

// A mod's own page: its switch, then its rows.
std::vector<PageItem> mod_page_items(std::size_t i) {
    std::vector<PageItem> items;
    const ModEntry& e = g_entries[i];
    std::string d = about_mod(e);
    d += (d.empty() ? "" : " ") + std::string("Off leaves it in fh2-mods but the game doesn't load it.");
    items.push_back({g_entry_row[i], "Load " + e.name + with_v(e.version), d});
    std::size_t own = 0;
    if (e.mod) {
        std::scoped_lock lk{g_mu};
        for (std::size_t r = 0; r < g_rows.size(); ++r)
            if (g_rows[r].mod == e.mod) {
                items.push_back({static_cast<int>(r), "", ""});
                ++own;
            }
    }
    if (!own)
        items.push_back(info("No settings", !e.enabled ? "Its settings show here once it's turned on and the game has restarted."
                                            : e.platform_mod ? "This mod has no settings of its own."
                                                             : "A mod from before the platform can't add settings here."));
    return items;
}

std::vector<PageItem> updates_items() {
    std::vector<PageItem> items;
    items.push_back(info("FH2 Mod Platform " FH2MOD_PLATFORM_VERSION,
                         "The platform comes with every mod: installing a mod's newest release brings the newest platform too."));
    for (const ModEntry& e : g_entries) {
        std::string label = e.name + with_v(e.version);
        std::string d;
        if (!e.enabled) d = "Turned off, so not checked.";
        else d = update_status(e);
        if (e.mod && !e.mod->repo.empty() && e.mod->check_updates) {
            const std::string latest = latest_known(e.mod);
            if (!latest.empty() && compare_versions(latest, e.version) > 0) label += ": " + latest + " is out";
            else if (!latest.empty()) label += ": up to date";
            d += " Release notes: github.com/" + e.mod->repo + "/releases";
        }
        items.push_back(info(label, d.empty() ? "No update information for this mod." : d));
    }
    items.push_back(info("How update checks work",
                         "Mods that name their GitHub page check it for new releases at most every 6 hours. A mod with a new "
                         "release also says so on the title screen, under PRESS (A) TO START."));
    return items;
}

std::vector<PageItem> get_mods_items() {
    struct Known {
        const char *key, *name, *repo, *what;
    };
    static const Known known[] = {
        {"universalradio", "FH2 Universal Radio", "Trinculo54/FH2-Universal-Radio", "Plays Spotify and more on the in-game radio."},
        {"wheresmybuddy", "WheresMyBuddy", "Trinculo54/WheresMyBuddy", "Puts your Xbox friends in the game as Drivatars."},
        {"wreckingball", "WreckingBall", "Trinculo54/WreckingBall",
         "Makes trees, fences, railings, roadside clutter and telephone poles break when you drive into them."},
    };
    std::vector<PageItem> items;
    items.push_back(info("Installing a mod",
                         "Download the zip from the mod's GitHub releases page and extract it into your Azimuth folder, next to "
                         "azimuth.exe, then start the game again. Its DLL goes into fh2-mods by itself."));
    for (const Known& k : known) {
        const ModEntry* e = installed(k.key);
        items.push_back(info(std::string(k.name) + (e ? " (installed)" : ""),
                             std::string(k.what) + " github.com/" + k.repo + "/releases"));
    }
    const std::string eo = extra_options_line();
    items.push_back(info(std::string("Forza Horizon 2 Extra Options") + (eo.empty() ? "" : " (installed)"),
                         "Not a platform mod: it loads through winmm.dll and works alongside the platform, adding options of its "
                         "own to the game's menus. github.com/RokkuDayo/forzahorizon2_extraoptions"));
    return items;
}

std::vector<PageItem> about_items() {
    std::size_t on = 0, off = 0;
    for (const ModEntry& e : g_entries) (e.enabled ? on : off)++;
    std::vector<PageItem> items;
    items.push_back(info("FH2 Mod Platform " FH2MOD_PLATFORM_VERSION,
                         "API " + std::to_string(FH2MOD_API_VERSION) +
                             ". It's the d3dcompiler_47.dll next to azimuth.exe, and it loads the mods in fh2-mods. "
                             "github.com/Trinculo54/fh2-mod-platform"));
    items.push_back(info(std::to_string(on) + " mod(s) loaded" + (off ? ", " + std::to_string(off) + " turned off" : ""),
                         "Turn a mod off or on under INSTALLED MODS. A mod that's turned off stays in fh2-mods, so turning it "
                         "back on needs nothing reinstalled."));
    const std::string eo = extra_options_line();
    items.push_back(info("Extra Options: " + (eo.empty() ? std::string("not installed") : eo),
                         eo.empty() ? "Forza Horizon 2 Extra Options isn't loaded. See GET MODS."
                                    : "Extra Options is loaded through winmm.dll. Its own options are on the game's settings pages."));
    items.push_back(info("Log: fh2-mods\\platform.log",
                         "What the platform and every mod did this time. If something isn't right, it's the file to send along "
                         "when you report it."));
    return items;
}

std::vector<PageItem> making_items() {
    std::vector<PageItem> items;
    items.push_back(info("Making a mod",
                         "A mod is a DLL in fh2-mods with an fh2mod_init function, built against fh2mod.h. "
                         "github.com/Trinculo54/fh2-mod-platform has the header, an example mod and docs/writing-a-mod.md."));
    items.push_back(info("Settings on this tab",
                         "add_setting puts On/Off and slider rows under your mod on INSTALLED MODS (and on Settings > Mods). "
                         "FH2ModInfo.description (API 6) is the line shown with your mod."));
    items.push_back(info("Update checks",
                         "Give FH2ModInfo.github_repo as \"Owner/Repo\" and the platform checks your releases, shows them under "
                         "UPDATES and on the title screen."));
    items.push_back(info("Game files and strings",
                         "set_file, add_sql and set_string change the game's files, database and text for as long as your mod "
                         "is installed, and the platform puts everything back when it isn't. See docs/api.md."));
    return items;
}

bool write_hub_page(int p) {
    std::scoped_lock lk{g_page_mu};
    switch (p) {
        case kHubInstalled: return write_list_page(hub_file(p).c_str(), "INSTALLED MODS", installed_items(), true);
        case kHubUpdates: return write_list_page(hub_file(p).c_str(), "UPDATES", updates_items());
        case kHubGetMods: return write_list_page(hub_file(p).c_str(), "GET MODS", get_mods_items());
        case kHubAbout: return write_list_page(hub_file(p).c_str(), "ABOUT", about_items());
        case kHubMaking: return write_list_page(hub_file(p).c_str(), "MAKING MODS", making_items());
        case kHubAllSettings: return write_mods_page();
        default: break;
    }
    if (p >= kHubModPage && static_cast<std::size_t>(p - kHubModPage) < g_entries.size()) {
        const ModEntry& e = g_entries[p - kHubModPage];
        std::string title = e.name;
        for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return write_list_page(hub_file(p).c_str(), title, mod_page_items(p - kHubModPage));
    }
    return false;
}

// Pause > MARKETPLACE -> MODS, with the tab's tiles where the store's were.
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
        const char *name, *item, *heading, *icon, *size, *tmpl;
        int x, y;
    };
    static const Tile tiles[] = {
        {"PART_Button_CarPacks", "FH2Mod_Tile_Updates", "UPDATES", "Icons/PauseMenu/Restart.png", "2x1", "HorizontalIconTile", 0, 0},
        {"PART_Button_ExpansionPacks", "FH2Mod_Tile_GetMods", "GET MODS", "Icons/PauseMenu/Extras.png", "2x1", "HorizontalIconTile", 0, 1},
        {"PART_Button_SeasonPass", "FH2Mod_Tile_Making", "MAKING MODS", "Icons/PauseMenu/TuningSetup.png", "1x2", "VerticalIconTile", 2, 0},
        {"PART_Button_FH2Mod_Installed", "FH2Mod_Tile_Installed", "INSTALLED MODS", "Icons/TiledMenus/Tuning1x1.png", "2x2", "VerticalIconTile", 3, 0},
        {"PART_Button_Extras", "FH2Mod_Tile_About", "ABOUT", "Icons/PauseMenu/Stats.png", "1x1", "IconTile", 5, 0},
        {"PART_Button_VIPMembership", "FH2Mod_Tile_AllSettings", "ALL SETTINGS", "Icons/PauseMenu/TuneCar.png", "1x1", "IconTile", 5, 1},
    };
    std::string res, buttons = "\r\n";
    int index = 1;
    for (const Tile& t : tiles) {
        res += std::string("\r\n            <local:TileMenuDataItem x:Key=\"") + t.item + "\" Heading=\"" + t.heading +
               "\" Icon=\"/ForzaUI;component/Textures/Anthem/" + t.icon + "\" />";
        buttons += std::string("                        <Button Canvas.Left=\"{StaticResource A_Double_TileCoord_X_") + std::to_string(t.x) +
                   "}\"\r\n                                Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_" + std::to_string(t.y) +
                   "}\"\r\n                                DataContext=\"{StaticResource " + t.item +
                   "}\"\r\n                                Style=\"{StaticResource A_Style_Button_Tile_" + t.size +
                   "}\"\r\n                                Template=\"{StaticResource A_ControlTemplate_" + t.tmpl +
                   "}\"\r\n                                local:TileMenuNavigationManager.TileState=\"Enabled\""
                   "\r\n                                local:TileMenuNavigationManager.GridX=\"" + std::to_string(t.x) +
                   "\"\r\n                                local:TileMenuNavigationManager.GridY=\"" + std::to_string(t.y) +
                   "\"\r\n                                local:UIPageTransitionManager.TransitionIndex=\"" + std::to_string(index++) +
                   "\"\r\n                                x:Name=\"" + t.name + "\" />\r\n";
    }
    buttons += "                    ";
    if (!replace_between(page, scope, "</Canvas>", buttons, false)) return false;
    page.insert(page.find(items_at) + items_at.size(), res);
    page.replace(page.find(tab), tab.size(), "Content=\"MODS\"");
    if (const std::size_t b = page.find(badge); b != std::string::npos) page.erase(b, badge.size());
    return set_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml", page.data(), page.size()) != 0;
}

// The Marketplace tiles' events go to a node of ours that shows the HUD page (as one of the
// tab's pages) and comes back to the pause menu. On it, a mod's row fires car_packs (see
// open_mod_page), which goes to a second HUD page (the mod's) that comes back to the first.
// ui_screens.xml gets a copy of show_hud_options with that extra way out.
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
    flow.insert(n, "<node id=\"_fh2mod_page\" state=\"ui_screens.fh2mod_show_page\">\r\n"
                   "      <entry id=\"enter\" />\r\n"
                   "      <entry id=\"enterback\" />\r\n"
                   "      <exit id=\"back\" target=\"pause_menu.enterback\" />\r\n"
                   "      <exit id=\"open\" target=\"_fh2mod_mod_page\" />\r\n"
                   "    </node>\r\n\r\n"
                   "    <node id=\"_fh2mod_mod_page\" state=\"ui_screens.show_hud_options\">\r\n"
                   "      <exit id=\"back\" target=\"_fh2mod_page.enterback\" />\r\n"
                   "    </node>\r\n\r\n    ");
    flow.replace(e, exit.size(), "<exit id=\"car_packs\" target=\"_fh2mod_page.enter\" />");
    return set_file(&g_owner_mod, "stateflow/pause.xml", flow.data(), flow.size()) != 0;
}

bool write_ui_screens_flow() {
    const auto zip = game_mount() / L"media" / L"stateflow.zip";
    std::vector<char> x = unzip_file(zip, "ui_screens.xml");
    std::string flow(x.begin(), x.end());
    const std::string at = "<state id=\"show_hud_options\">";
    const std::size_t a = flow.find(at);
    if (flow.empty() || a == std::string::npos || flow.find(at, a + 1) != std::string::npos ||
        flow.find("fh2mod_show_page") != std::string::npos) {
        slog("stateflow ui_screens.xml doesn't look as expected; no MODS tab");
        return false;
    }
    flow.insert(a, "<state id=\"fh2mod_show_page\">\r\n"
                   "    <entry id=\"enter\" target=\".\" />\r\n"
                   "    <entry id=\"enterback\" target=\".\" />\r\n"
                   "    <exit id=\"back\" />\r\n"
                   "    <exit id=\"open\" />\r\n\r\n"
                   "    <behavior id=\"UIPageHudOptions\">\r\n"
                   "      <transition id=\"back\" target=\".back\" />\r\n"
                   "      <transition id=\"car_packs\" target=\".open\" />\r\n"
                   "    </behavior>\r\n\r\n"
                   "    <behavior id=\"OnParentTransition\">\r\n"
                   "      <transition id=\"ParentTransition\" target=\".back\" />\r\n"
                   "    </behavior>\r\n"
                   "  </state>\r\n\r\n  ");
    return set_file(&g_owner_mod, "stateflow/ui_screens.xml", flow.data(), flow.size()) != 0;
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

std::atomic<uint64_t> g_mods_asked{0};   // GetTickCount64() when the tile was pressed
bool g_in_mods = false;                  // on the Mods page (UI thread only)
std::string g_asked_values;              // the values the restart popup last asked about
bool g_popup_ok = false;

void hub_reset();

void mods_tile_pressed(void* page) {
    hub_reset();
    g_in_mods    = true;
    g_mods_asked = GetTickCount64();
    reinterpret_cast<TileFn>(g_base + RVA_HUD_TILE)(page);
}

void wire_mods_tile(void* page) {
    static const Pmf pmf{reinterpret_cast<void*>(&mods_tile_pressed), 0, 0};
    __try {
        reinterpret_cast<WireButtonFn>(g_base + RVA_WIRE_BUTTON)(page, "PART_Button_Mods", &pmf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void widen_nav(void* page) {
    __try {
        uint8_t* nav = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(page) + 0x410);
        if (nav && *reinterpret_cast<int*>(nav + 0x9c) == 5 && *reinterpret_cast<int*>(nav + 0xa0) == 2)
            *reinterpret_cast<int*>(nav + 0x9c) = 6;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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

// The MODS tab: the page a tile (or a mod's row) asked for. Opening a page builds its scene
// record twice, so an ask lasts for two builds (or 1.5 s). After a mod's page, the next page
// to open without an ask is the Installed list it came back to.
std::atomic<int> g_hub_page{0};
std::atomic<uint64_t> g_hub_asked{0};
std::atomic<int> g_hub_builds{0};
std::atomic<bool> g_hub_return{false};   // on a mod's page: B comes back to Installed mods
std::atomic<int> g_hub_shown{0};         // the tab's page being opened, for wiring its rows
bool g_in_hub = false;                   // on one of the MODS tab's pages (UI thread only)

void hub_reset() {
    g_hub_builds = 0;
    g_hub_return = false;
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
    if (g_hub_builds.load() <= 0 && g_hub_return.exchange(false)) hub_ask(kHubInstalled);
    if (g_hub_builds.load() > 0 && now - g_hub_asked.load() < 1500) {
        --g_hub_builds;
        const int p = g_hub_page.load();
        page        = p == kHubAllSettings ? L"A_ModOptions" : hub_name(p);
        g_hub_shown = p;
    } else if (asked && now - asked < 1500) {
        page = L"A_ModOptions";
    }
    if (!page.empty()) slog("debug: scene record -> " + narrow(page));
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
const wchar_t* fake_text(FakeLocString* self) { return self->text; }
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

bool show_popup_inner(void* page) {
    static FakeLocString* title   = loc_string(kTitle);
    static FakeLocString* message = loc_string(kMessage);
    static FakeLocString* labels[3] = {loc_string(kChoices[0]), loc_string(kChoices[1]), loc_string(kChoices[2])};
    void* items = nullptr;
    reinterpret_cast<MakeItemsFn>(g_base + RVA_MAKE_ITEMS)(&items);
    if (!items) return false;
    auto* coll = static_cast<uint8_t*>(items);
    for (int i = 0; i < 3; ++i) {
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
    PopupFn fn{g_fn_vt, popup, page};
    StdFunction f{};
    f.impl = fn_copy(&fn, nullptr);   // on the heap; the callee deletes it
    reinterpret_cast<PopupHandlerFn>(g_base + RVA_POPUP_HANDLER)(popup, 2, &f, true);
    release(coll);
    return true;
}

bool show_popup(void* page) {
    __try {
        return show_popup_inner(page);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_popup_ok = false;
        return false;
    }
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
    g_mods_asked = 0;
    g_hub_return = false;
    if (!g_settings_tile) return o_wire_tiles(page);
    widen_nav(page);
    o_wire_tiles(page);
    wire_mods_tile(page);
    // back from the Mods page with a change that needs a restart, not asked about yet?
    const bool from_mods = g_in_mods;
    g_in_mods            = false;
    if (from_mods && g_popup_ok && restart_pending()) {
        std::string now = current_values();
        if (now != g_asked_values) {
            g_asked_values = now;
            g_popup_back   = &mods_tile_pressed;
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
constexpr uintptr_t RVA_TILE_SEASONPASS = 0x0DBF8F0;
constexpr uintptr_t RVA_TILE_VIP        = 0x0DBFE40;
// 0x140DD8FD0(page, "PART_GROUP_Marketplace", 1) hides the tab; Extra Options' EnableMarketplace
// makes these 0, and so does the platform.
constexpr uintptr_t RVA_MARKETPLACE_HIDE[] = {0x0DDACAD, 0x0DDAE1F, 0x0DDB2F5, 0x0DDB910, 0x0DDBB3E};

WireTilesFn o_pause_menu_wire = nullptr;
TileFn o_car_packs = nullptr, o_expansions = nullptr, o_extras = nullptr, o_season_pass = nullptr, o_vip = nullptr;

void open_hub_page(void* page, int which) {
    write_hub_page(which);   // up to date (update checks finish after the game starts)
    g_in_hub     = true;
    g_hub_return = false;
    hub_ask(which);
    o_car_packs(page);
}
// On Installed mods, mod n's row is a button (FirstItem, or PART_Button_FH2Mod_<n>), wired
// once the page is loaded (the HUD page class's vtable +0x488, shared with one other page). A
// press fires car_packs (an event name the game already has; a new one isn't taken) the way the Marketplace tiles fire theirs: 0x141422F10(page,
// &std::string name), which needs the page's state-machine link at +0x310.
constexpr uintptr_t RVA_HUD_PAGE_LOADED = 0x0C7AD40;
constexpr uintptr_t RVA_HUD_PAGE_VT_SLOT = 0x2DF06C0;   // HudOptions page vtable +0x488
constexpr uintptr_t RVA_PAGE_FIRE_EVENT = 0x1422F10;
using FireEventFn = void (*)(void* page, const void* name);
TileFn o_hud_page_loaded = nullptr;

struct MsvcString {
    char buf[16];
    uint64_t size, cap;
};

bool fire_open(void* page) {
    static const MsvcString name{"car_packs", 9, 15};
    __try {
        if (!*reinterpret_cast<void**>(static_cast<uint8_t*>(page) + 0x310)) return false;
        reinterpret_cast<FireEventFn>(g_base + RVA_PAGE_FIRE_EVENT)(page, &name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void open_mod_page(void* page, std::size_t n) {
    if (n >= g_entries.size()) return;
    const int p = kHubModPage + static_cast<int>(n);
    write_hub_page(p);
    hub_ask(p);
    g_hub_return = true;
    if (!fire_open(page)) {
        g_hub_builds = 0;
        g_hub_return = false;
        slog("couldn't open " + g_entries[n].name + "'s page");
    }
}

// Which HUD page object is Installed mods (set when one loads; only compared, never used).
std::atomic<void*> g_installed_page{nullptr};

std::string hexp(const void* p) {
    char b[24];
    std::snprintf(b, sizeof b, "%p", p);
    return b;
}

void d_hud_page_loaded(void* page) {
    o_hud_page_loaded(page);
    const int shown = g_hub_shown.load();
    slog("debug: HUD page " + hexp(page) + " loaded, MODS tab page " + std::to_string(shown));
    if (g_hub_shown.exchange(0) == kHubInstalled) {
        g_installed_page = page;
        g_selected       = 0;
    } else if (g_installed_page.load() == page) {
        g_installed_page = nullptr;
    }
}

// A page's buttons: vtable +0x578, (page, button, pressed) -> handled. On the options pages A
// (18) is Accept, which saves and goes back; on Installed mods it opens the mod's page.
constexpr uintptr_t RVA_HUD_PAGE_BUTTON_SLOT = 0x2DF07B0;   // HudOptions page vtable +0x578
constexpr uintptr_t RVA_PAGE_BUTTON          = 0x0CEC3D0;   // the shared implementation
constexpr int kButtonA                       = 18;     // what the page gets for A (seen in the log)
using PageButtonFn = bool (*)(void* page, int button, bool pressed);
PageButtonFn o_hud_page_button = nullptr;

bool d_hud_page_button(void* page, int button, bool pressed) {
    if (g_in_hub)
        slog("debug: button " + std::to_string(button) + (pressed ? " down" : " up") + " on " + hexp(page) +
             (page == g_installed_page.load() ? " (Installed mods)" : "") + ", row " + std::to_string(g_selected.load()));
    if (button == kButtonA && page && page == g_installed_page.load()) {
        if (pressed) {
            const int n = g_selected.load();
            if (n >= 0 && static_cast<std::size_t>(n) < g_entries.size() && static_cast<std::size_t>(n) < kMaxModPages) open_mod_page(page, n);
        }
        return true;
    }
    return o_hud_page_button(page, button, pressed);
}

void d_tile_updates(void* page) { open_hub_page(page, kHubUpdates); }
void d_tile_get_mods(void* page) { open_hub_page(page, kHubGetMods); }
void d_tile_about(void* page) { open_hub_page(page, kHubAbout); }
void d_tile_making(void* page) { open_hub_page(page, kHubMaking); }
void d_tile_all_settings(void* page) { open_hub_page(page, kHubAllSettings); }
void tile_installed(void* page) { open_hub_page(page, kHubInstalled); }

void wire_installed_tile(void* page) {
    static const Pmf pmf{reinterpret_cast<void*>(&tile_installed), 0, 0};
    __try {
        reinterpret_cast<WireButtonFn>(g_base + RVA_PAUSE_WIRE_BTN)(page, "PART_Button_FH2Mod_Installed", &pmf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void d_pause_menu_wire(void* page) {
    g_hub_asked  = 0;
    g_hub_builds = 0;
    g_hub_return = false;
    o_pause_menu_wire(page);
    wire_installed_tile(page);
    // back from the MODS tab with a change that needs a restart?
    const bool from_hub = g_in_hub;
    g_in_hub            = false;
    if (from_hub && g_popup_ok && restart_pending()) {
        std::string now = current_values();
        if (now != g_asked_values) {
            g_asked_values = now;
            g_popup_back   = &tile_installed;
            if (show_popup(page)) slog("asked about restarting");
        }
    }
}

bool show_marketplace_tab() {
    static const uint8_t hide[] = {0x41, 0xB8, 0x01, 0x00, 0x00, 0x00}, show[] = {0x41, 0xB8, 0x00, 0x00, 0x00, 0x00};
    for (uintptr_t rva : RVA_MARKETPLACE_HIDE)
        if (!code_matches(rva, hide, sizeof hide) && !code_matches(rva, show, sizeof show)) return false;
    for (uintptr_t rva : RVA_MARKETPLACE_HIDE) {
        auto* at  = reinterpret_cast<uint8_t*>(g_base + rva);
        DWORD old = 0;
        if (!VirtualProtect(at + 2, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
        at[2] = 0;
        VirtualProtect(at + 2, 1, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_base + RVA_MARKETPLACE_HIDE[0]), 0x2000);
    return true;
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
        {RVA_TILE_SEASONPASS, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48}},
        {RVA_TILE_VIP, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48}},
    };
    auto undo_files = [] {
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml");
        clear_file(&g_owner_mod, "stateflow/pause.xml");
        clear_file(&g_owner_mod, "stateflow/ui_screens.xml");
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
        {"Marketplace: Season Pass tile", RVA_TILE_SEASONPASS, reinterpret_cast<void*>(&d_tile_making), &o_season_pass},
        {"Marketplace: VIP tile", RVA_TILE_VIP, reinterpret_cast<void*>(&d_tile_all_settings), &o_vip},
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
    if (!show_marketplace_tab()) slog("the code that hides the Marketplace tab doesn't look as expected; MODS shows only with Extra Options' EnableMarketplace");
    slog("MODS tab ready");
}

// ---- hooking up ----------------------------------------------------------------------------
void hook_game() {
    void* base = g_api->wait_for_code(0xFFFFFFFFu);
    if (!base) return;
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
    if (!g_sliders_ok) slog("the game's float properties don't look as expected; sliders won't work");
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
    {
        std::scoped_lock lk{g_mu};
        g_asked_values = current_values();
    }

    FH2Mod* m = &g_owner_mod;
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
        return;
    }
    slog("ready");
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
    if (find_row(m, s->key)) {
        log_line(m->name, std::string("add_setting: there's already a row called ") + s->key);
        return 0;
    }
    Row& r       = g_rows.emplace_back();
    r.mod        = m;
    r.key        = s->key;
    r.label      = s->label && *s->label ? s->label : s->key;
    r.description = s->description ? s->description : "";
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
    log_line(m->name, "Mods page row: " + r.label + (r.restart ? " (after a restart)" : "") +
                          (g_written ? "; the page is already written, so it shows from the next launch" : ""));
    return 1;
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
    if (Row* r = find_row(m, key); r && !r->slider) r->value = value ? 1 : 0;
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

// After every mod's fh2mod_init: write the pages (the MODS tab's, and Settings > Mods if any
// mod added a row) and hook the game.
void settings_start(const FH2ModApi* api) {
    g_api = api;
    g_owner_mod.name  = kWho;
    g_owner_mod.wname = widen(kWho);
    g_hub_owner.name  = "MODS tab";
    g_hub_owner.wname = widen(g_hub_owner.name);
    std::size_t n = 0;
    {
        std::scoped_lock lk{g_mu};
        n = g_rows.size();
    }
    add_hub_rows();
    g_written = true;
    const char* const files[] = {"UI/Scenes/Anthem/A_PauseOptions.xaml", "UI/Scenes/Anthem/A_ModOptions.xaml",
                                 "UI/Scenes/Anthem/A_PauseMenu.xaml",    "stateflow/pause.xml", "stateflow/ui_screens.xml"};
    if (game_mount().empty()) {
        slog("the game folder wasn't found; no Mods page or MODS tab");
        for (const char* f : files) remove_leftover_file(f);
        return;
    }
    // the MODS tab
    g_hub_files = write_pause_menu() && write_pause_flow() && write_ui_screens_flow();
    for (int p = kHubInstalled; p <= kHubMaking && g_hub_files; ++p) g_hub_files = write_hub_page(p);
    for (std::size_t i = 0; i < g_entries.size() && i < kMaxModPages && g_hub_files; ++i)
        g_hub_files = write_hub_page(kHubModPage + static_cast<int>(i));
    if (!g_hub_files) {
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseMenu.xaml");
        clear_file(&g_owner_mod, "stateflow/pause.xml");
        clear_file(&g_owner_mod, "stateflow/ui_screens.xml");
    }
    // Settings > Mods (also the tab's All settings)
    bool mods_page = write_mods_page();
    g_settings_tile = n && mods_page && write_settings_page();
    if (!g_settings_tile) {
        // No rows this time (the mods that had some were removed): take the tile back out
        // before the game reads its menus, rather than with the clean-up later.
        remove_leftover_file("UI/Scenes/Anthem/A_PauseOptions.xaml");
        clear_file(&g_owner_mod, "UI/Scenes/Anthem/A_PauseOptions.xaml");
    }
    if (!mods_page) remove_leftover_file("UI/Scenes/Anthem/A_ModOptions.xaml");
    slog(std::to_string(n) + " row(s) on the Mods page, " + std::to_string(g_entries.size()) + " mod(s) on the MODS tab" +
         (g_hub_files ? "" : " (not shown)"));
    if (g_hub_files || g_settings_tile) std::thread(hook_game).detach();
}

FH2Mod* settings_owner() { return &g_owner_mod; }

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

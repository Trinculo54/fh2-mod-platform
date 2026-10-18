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

// The Mods page: A_HUDOptions.xaml with its rows swapped for the mods'.
bool write_mods_page() {
    std::string page = read_ui("Scenes/Anthem/A_HUDOptions.xaml");
    if (page.empty()) return false;
    std::string res = "\r\n            <local:FZBooleanToVisibilityConverter x:Key=\"FH2Mod_ShowNotice\" UseHidden=\"true\" InvertResult=\"false\" />\r\n";
    std::string rows = "\r\n";
    {
        std::scoped_lock lk{g_mu};
        for (std::size_t i = 0; i < g_rows.size(); ++i) {
            const Row& r     = g_rows[i];
            std::string desc = r.description;
            if (r.restart) desc += (desc.empty() ? "" : " ") + std::string("Takes effect the next time you start the game.");
            desc += (desc.empty() ? "" : " ") + std::string("(") + r.mod->name + ")";
            res += "            <local:TileMenuDataItem x:Key=\"FH2Mod_Item_" + std::to_string(i) + "\" Heading=\"" +
                   xml_escape(r.label) + "\" Description=\"" + xml_escape(desc) + "\" />\r\n";
            if (r.slider) {
                res += slider_resources(r, i);
                rows += "                                        <Slider Value=\"{Binding " + r.prop + "}\" Tag=\"{StaticResource FH2Mod_Item_" +
                        std::to_string(i) + "}\" Style=\"{StaticResource FH2Mod_Slider_" + std::to_string(i) + "}\"" +
                        (i == 0 ? " x:Name=\"FirstItem\"" : "") + " />\r\n";
                continue;
            }
            rows += "                                        <local:SpinnerControl Label=\"" + xml_escape(r.label) +
                    "\" Tag=\"{StaticResource FH2Mod_Item_" + std::to_string(i) + "}\" SelectedIndex=\"{Binding " + r.prop +
                    "}\" ItemsSource=\"{Binding OnOffOptions}\" Style=\"{StaticResource A_Style_SpinnerControl_Default}\"" +
                    (i == 0 ? " x:Name=\"FirstItem\"" : "") + " />\r\n";
        }
    }
    res += "        ";
    rows += "                                ";
    const std::string title = "Content=\"{local:Loc ScreenTitles.IDS_HUDOptions}\"";
    const std::string help_y = "local:ControllerButtonPanel.YDescription=";
    const std::string desc_panel = "Style=\"{StaticResource A_Style_DescriptionPanel}\"";
    if (page.find(title) == std::string::npos || page.find(help_y) == std::string::npos ||
        page.find(desc_panel) == std::string::npos ||
        !replace_between(page, "<ResourceDictionary>", "</ResourceDictionary>", res, false) ||
        !replace_between(page, "<local:CommonListBox", "</local:CommonListBox>", rows, true)) {
        slog("A_HUDOptions.xaml doesn't look as expected; no Mods page");
        return false;
    }
    const std::size_t t = page.find(title);
    page.replace(t, title.size(), "Content=\"MODS\"");
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
    return set_file(&g_owner_mod, "UI/Scenes/Anthem/A_ModOptions.xaml", page.data(), page.size()) != 0;
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

void mods_tile_pressed(void* page) {
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

// L"Anthem\A_HudOptions" -> L"Anthem\A_ModOptions", in place (same length)
bool point_at_mods_page(uint8_t* rec) {
    __try {
        const uint64_t size = *reinterpret_cast<uint64_t*>(rec + 0x38);
        const uint64_t cap  = *reinterpret_cast<uint64_t*>(rec + 0x40);
        wchar_t* p = cap > 7 ? *reinterpret_cast<wchar_t**>(rec + 0x28) : reinterpret_cast<wchar_t*>(rec + 0x28);
        if (size != 19 || !readable(p, 40) || std::wcsncmp(p, L"Anthem\\A_HudOptions", 19) != 0) return false;
        p[9]  = L'M';
        p[10] = L'o';
        p[11] = L'd';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint8_t* d_hud_scene(uint8_t* out) {
    uint8_t* r = o_hud_scene(out);
    const uint64_t asked = g_mods_asked.load();
    if (asked && GetTickCount64() - asked < 1500 && !point_at_mods_page(r))
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
            mods_tile_pressed(self->page);
            break;
        default: break;
    }
}

void d_wire_tiles(void* page) {
    g_mods_asked = 0;
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
            if (show_popup(page)) slog("asked about restarting");
        }
    }
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
        slog("hooking the Settings page failed; no Mods tile");
        g_api->disable_hooks(m, nullptr);
        clear_file(m, nullptr);
        return;
    }
    slog("ready");
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

// After every mod's fh2mod_init: write the pages and hook the game, if any mod added a row.
void settings_start(const FH2ModApi* api) {
    g_api = api;
    g_owner_mod.name  = kWho;
    g_owner_mod.wname = widen(kWho);
    std::size_t n = 0;
    {
        std::scoped_lock lk{g_mu};
        n = g_rows.size();
    }
    g_written = true;
    if (!n) {
        // No rows this time (the mods that had some were removed): take the tile and the
        // page back out before the game reads its menus, rather than with the clean-up later.
        remove_leftover_file("UI/Scenes/Anthem/A_PauseOptions.xaml");
        remove_leftover_file("UI/Scenes/Anthem/A_ModOptions.xaml");
        return;
    }
    if (game_mount().empty()) {
        slog("the game folder wasn't found; no Mods page");
        return;
    }
    if (!write_mods_page() || !write_settings_page()) {
        clear_file(&g_owner_mod, nullptr);
        return;
    }
    slog(std::to_string(n) + " row(s) on the Mods page");
    std::thread(hook_game).detach();
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

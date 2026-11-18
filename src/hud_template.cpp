// The HUD layer preview's template (hud_template.hpp): one copy of the layer for every mod, its parts
// the union of the layers' parts, written into the free-roam HUD page (UI.zip's Scenes/Anthem/A_Hud.xaml)
// once a session. Every text is a binding to a platform property (FH2Mod_Hud_<name>); the only literal
// texts are the platform's "+", "<" and ">". The page is checked before set_file: tags, resource keys,
// Styles, the root, every binding and the binding budget, so nothing a mod sets can break the page.
#include "hud_template.hpp"

#include "fh2mod_hud.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace fh2platform {

const char* const kHudPartNames[kHudParts] = {"BAR", "CHIPS", "TIP", "PANEL", "COUNTDOWN", "FEED", "CANVAS"};
const char* const kHudPartProps[kHudParts] = {"Bar", "Chips", "Tip", "Panel", "Count", "Feed", "Canvas"};

std::string hud_parts_text(uint32_t parts) {
    std::string s;
    for (int k = 0; k < kHudParts; ++k)
        if (parts & (1u << k)) s += (s.empty() ? "" : ", ") + std::string(kHudPartNames[k]);
    return s.empty() ? "none" : s;
}

const char* hud_type_word(char t) { return t == 'b' ? "bool" : t == 'i' ? "int" : t == 'f' ? "float" : "String"; }

namespace {

constexpr int kBar = 0, kChips = 1, kTip = 2, kPanel = 3, kCount = 4, kFeed = 5, kCanvas = 6;
bool has(uint32_t parts, int k) { return (parts & (1u << k)) != 0; }
bool ring_parts(uint32_t parts) { return has(parts, kBar) || has(parts, kCount); }

// The 24 controller pictures (FH2_GLYPH_ 1-24), each the game's own A_BitmapImage_ControllerButton_<key>.
struct Glyph {
    int v;
    const char* key;
};
const Glyph kGlyphs[24] = {{1, "A"},
                           {2, "B"},
                           {3, "X"},
                           {4, "Y"},
                           {5, "LB"},
                           {6, "RB"},
                           {7, "LT"},
                           {8, "RT"},
                           {9, "Menu"},
                           {10, "View"},
                           {11, "DPadUp"},
                           {12, "DPadDown"},
                           {13, "DPadLeft"},
                           {14, "DPadRight"},
                           {15, "DPadUpDown"},
                           {16, "DPadLeftRight"},
                           {17, "L3"},
                           {18, "R3"},
                           {19, "LeftThumb"},
                           {20, "LeftThumb_UpDown"},
                           {21, "LeftThumb_LeftRight"},
                           {22, "RightThumb"},
                           {23, "RightThumb_UpDown"},
                           {24, "RightThumb_LeftRight"}};

// ---- the property table -----------------------------------------------------------------------------

struct TableBuilder {
    std::vector<HudProp> props;
    int part = -1;
    void add(const std::string& n, char type, HudRole role = HudRole::content, std::vector<int> values = {}, const char* def = nullptr) {
        HudProp& p   = props.emplace_back();
        p.short_name = n;
        p.name       = "FH2Mod_Hud_" + n;
        p.type       = type;
        p.values     = std::move(values);
        p.part       = part;
        p.role       = role;
        if (def) p.def = def;
    }
};

std::string num(int i) { return std::to_string(i); }

} // namespace

std::vector<HudProp> hud_build_props(uint32_t parts, const std::vector<HudCanvas>& canvases) {
    std::vector<int> glyphs;
    for (const Glyph& g : kGlyphs) glyphs.push_back(g.v);
    const std::vector<int> two = {1, 2}, three = {1, 2, 3};
    TableBuilder t;
    t.add("Shown", 'b', HudRole::root);
    t.add("Shutter", 'b', HudRole::shutter);
    if (has(parts, kBar)) {
        t.part = kBar;
        t.add("Bar", 'b', HudRole::part_fade);
        t.add("BarVis", 'b', HudRole::part_vis);
        t.add("HoldWord", 's', HudRole::content, {}, "HOLD");
        t.add("HoldAngle", 'f', HudRole::frame_value);
        t.add("HoldFill", 'f', HudRole::frame_value);
        t.add("TabsShown", 'b');
        t.add("TabKbd", 'b', HudRole::kbd);
        t.add("TabKeyL", 's');
        t.add("TabKeyR", 's');
        for (int i = 0; i < 8; ++i) t.add("Tab" + num(i), 's');
        for (int i = 0; i < 8; ++i) t.add("Tab" + num(i) + "Shown", 'b');
        for (int i = 0; i < 8; ++i) t.add("Tab" + num(i) + "Sel", 'b');
        t.add("Alert", 'b');
        t.add("L1", 's');
        t.add("L1After", 's');
        t.add("L1Style", 'i', HudRole::content, two);
        t.add("L2", 's');
        t.add("Dots", 'b');
        for (int j = 0; j < 8; ++j) t.add("Dot" + num(j), 'i', HudRole::content, three);
        t.add("InlineAfter", 's');
        t.add("StripRow", 'b');
        t.add("Rail", 'b');
        t.add("RailLeft", 's');
        t.add("RailRight", 's');
        t.add("RowAfter", 's');
        for (int k = 0; k < 16; ++k) {
            const std::string m = "Mark" + num(k);
            t.add(m + "Shown", 'b');
            t.add(m + "X", 'f');
            t.add(m + "Angle", 'f');
            t.add(m + "Style", 'i', HudRole::content, two);
            t.add(m + "Num", 's');
        }
        t.add("Playhead", 'b');
        t.add("PlayheadX", 'f', HudRole::frame_value);
        for (int i = 0; i < 5; ++i) {
            const std::string p = "P" + num(i);
            t.add(p + "Shown", 'b');
            t.add(p + "Hold", 'b');
            t.add(p + "Kbd", 'b', HudRole::kbd);
            t.add(p + "Chord", 'b');
            t.add(p + "G", 'i', HudRole::content, glyphs);
            t.add(p + "K0", 's');
            t.add(p + "K0Arrow", 'b');
            t.add(p + "K0Angle", 'f');
            t.add(p + "K1Shown", 'b');
            t.add(p + "Join", 's');
            t.add(p + "K1", 's');
            t.add(p + "K1Arrow", 'b');
            t.add(p + "K1Angle", 'f');
            t.add(p + "Label", 's');
        }
    }
    if (has(parts, kChips)) {
        t.part = kChips;
        t.add("Chips", 'b', HudRole::part_fade);
        t.add("ChipsVis", 'b', HudRole::part_vis);
        for (int i = 0; i < 5; ++i) {
            const std::string c = "C" + num(i);
            t.add(c + "Shown", 'b');
            t.add(c, 's');
            for (int j = 0; j < 5; ++j) t.add(c + "D" + num(j), 'i', HudRole::content, two);
        }
    }
    if (has(parts, kTip)) {
        t.part = kTip;
        t.add("Tip", 'b', HudRole::part_fade);
        t.add("TipVis", 'b', HudRole::part_vis);
        t.add("TipBefore", 's');
        t.add("TipKbd", 'b', HudRole::kbd);
        t.add("TipChord", 'b');
        t.add("TipG", 'i', HudRole::content, glyphs);
        t.add("TipK0", 's');
        t.add("TipK0Arrow", 'b');
        t.add("TipK0Angle", 'f');
        t.add("TipK1Shown", 'b');
        t.add("TipJoin", 's');
        t.add("TipK1", 's');
        t.add("TipK1Arrow", 'b');
        t.add("TipK1Angle", 'f');
        t.add("TipAfter", 's');
    }
    if (has(parts, kPanel)) {
        t.part = kPanel;
        t.add("Panel", 'b', HudRole::part_fade);
        t.add("PanelVis", 'b', HudRole::part_vis);
        for (int r = 0; r < 8; ++r) {
            const std::string p = "R" + num(r);
            t.add(p + "Shown", 'b');
            t.add(p + "Sel", 'b');
            t.add(p + "Label", 's');
            t.add(p + "Style", 'i', HudRole::content, two);
            t.add(p + "HasValue", 'b');
            t.add(p + "Value", 's');
        }
        t.add("PanelFooter", 's');
    }
    if (has(parts, kCount)) {
        t.part = kCount;
        t.add("Count", 'b', HudRole::part_fade);
        t.add("CountVis", 'b', HudRole::part_vis);
        t.add("CountText", 's');
        t.add("CountPop", 'b', HudRole::count_pop);
        t.add("CountLabel", 's');
        t.add("CountLabelShown", 'b');
    }
    if (has(parts, kFeed)) {
        t.part = kFeed;
        t.add("Feed", 'b', HudRole::part_fade);
        t.add("FeedVis", 'b', HudRole::part_vis);
        for (int i = 0; i < kHudFeedLines; ++i) {   // F0 is the bottom line, the newest
            const std::string f = "F" + num(i);
            t.add(f + "Shown", 'b');
            t.add(f + "HasName", 'b');
            t.add(f + "Name", 's');
            t.add(f + "Text", 's');
            t.add(f + "Style", 'i', HudRole::content, {1, 2, 3});
        }
    }
    if (has(parts, kCanvas)) {
        t.part = kCanvas;
        t.add("Canvas", 'b', HudRole::part_fade);
        t.add("CanvasVis", 'b', HudRole::part_vis);
        for (const HudCanvas& c : canvases) {
            const std::string cv = "Cv" + num(c.layer);
            t.add(cv + "On", 'b');   // this canvas is the one showing: its layer has the screen
            const std::size_t first = t.props.size();
            for (int i = 0; i < c.numbers; ++i) t.add(cv + "N" + num(i), 'f', HudRole::frame_value);
            for (int i = 0; i < c.texts; ++i) t.add(cv + "T" + num(i), 's');
            for (int i = 0; i < c.switches; ++i) t.add(cv + "B" + num(i), 'b');
            for (std::size_t i = first; i < t.props.size(); ++i) t.props[i].canvas = true;
        }
    }
    return t.props;
}

std::string hud_props_text(const std::vector<HudProp>& props) {
    std::string out;
    for (const HudProp& p : props) {
        out += p.name + "\t" + hud_type_word(p.type) + "\t";
        out += p.type == 's' ? p.def : p.type == 'b' ? "false" : p.type == 'f' ? "0.0" : "0";
        out += "\t";
        for (std::size_t i = 0; i < p.values.size(); ++i) out += (i ? "," : "") + std::to_string(p.values[i]);
        out += "\r\n";
    }
    return out;
}

namespace {

// ---- the markup -------------------------------------------------------------------------------------

const char* const kGlass  = " Preset=\"{DynamicResource A_FZMaterialPresetGlassRoundtangle_HudItem}\"";
const char* const kRt     = " Preset=\"{DynamicResource A_FZMaterialPresetRoundtangle_HudItem}\"";
const char* const kSmall  = " Style=\"{StaticResource A_Style_FZTextBlock_HudSmall}\"";
const char* const kMedium = " Style=\"{StaticResource A_Style_FZTextBlock_HudMedium}\"";
const char* const kLocalNs = "xmlns:local=\"clr-namespace:ForzaUI;assembly=\"";
const char* const kEdNs    = "xmlns:ed=\"http://schemas.microsoft.com/expression/2010/drawing\"";

std::string V(const std::string& x) {
    return " Visibility=\"{Binding FH2Mod_Hud_" + x + ", Converter={StaticResource A_FZBooleanToVisibilityConverter}}\"";
}
std::string VN(const std::string& x) {
    return " Visibility=\"{Binding FH2Mod_Hud_" + x + ", Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\"";
}
std::string T(const std::string& x) { return " Text=\"{Binding FH2Mod_Hud_" + x + "}\""; }
// Stretch="None": FZTextBlock otherwise shrinks text to fit before it trims (demo run 1), and the game's own
// pages set it on the text they trim.
std::string TRIM(int w) {
    return " MaxWidth=\"" + std::to_string(w) + "\" TextTrimming=\"CharacterEllipsis\" Stretch=\"None\"";
}

// Indented lines with \r\n, as the notice writes them.
struct Xml {
    std::string s;
    int base, depth = 0;
    explicit Xml(int b) : base(b) {}
    void line(const std::string& t) {
        s.append(static_cast<std::size_t>(base + 4 * depth), ' ');
        s += t;
        s += "\r\n";
    }
    void open(const std::string& t) {
        line(t);
        ++depth;
    }
    void close(const std::string& tag) {
        --depth;
        line("</" + tag + ">");
    }
};

void trigger_setter(Xml& x, const std::string& prop, const std::string& value, const char* property, const std::string& to) {
    x.open("<DataTrigger Binding=\"{Binding FH2Mod_Hud_" + prop + "}\" Value=\"" + value + "\">");
    x.line(std::string("<Setter Property=\"") + property + "\" Value=\"" + to + "\" />");
    x.close("DataTrigger");
}

using ColourMap = std::initializer_list<std::pair<const char*, const char*>>;

// COLOURED(tag, p, base, v -> c, ...): the element's inline Style, Color = base, one DataTrigger per value.
void coloured(Xml& x, const std::string& tag, const std::string& prop, const char* base, ColourMap map) {
    x.open("<" + tag + ".Style>");
    x.open("<Style TargetType=\"" + tag + "\">");
    x.line(std::string("<Setter Property=\"Color\" Value=\"") + base + "\" />");
    x.open("<Style.Triggers>");
    for (const auto& [v, c] : map) trigger_setter(x, prop, v, "Color", c);
    x.close("Style.Triggers");
    x.close("Style");
    x.close(tag + ".Style");
}

// SHOWSTYLE(tag, p, v -> c, ...): collapsed unless p has one of the values, then in its colour.
void showstyle(Xml& x, const std::string& tag, const std::string& prop, ColourMap map) {
    x.open("<" + tag + ".Style>");
    x.open("<Style TargetType=\"" + tag + "\">");
    x.line("<Setter Property=\"Visibility\" Value=\"Collapsed\" />");
    x.open("<Style.Triggers>");
    for (const auto& [v, c] : map) {
        x.open("<DataTrigger Binding=\"{Binding FH2Mod_Hud_" + prop + "}\" Value=\"" + v + "\">");
        x.line(std::string("<Setter Property=\"Color\" Value=\"") + c + "\" />");
        x.line("<Setter Property=\"Visibility\" Value=\"Visible\" />");
        x.close("DataTrigger");
    }
    x.close("Style.Triggers");
    x.close("Style");
    x.close(tag + ".Style");
}

// An inline FZTextBlock Style BasedOn one of the HUD's, Color = base, one DataTrigger per value.
void text_style(Xml& x, const char* based_on, const std::string& prop, const char* base, ColourMap map) {
    x.open("<local:FZTextBlock.Style>");
    x.open(std::string("<Style TargetType=\"{x:Type local:FZTextBlock}\" BasedOn=\"{StaticResource ") + based_on + "}\">");
    x.line(std::string("<Setter Property=\"Color\" Value=\"") + base + "\" />");
    x.open("<Style.Triggers>");
    for (const auto& [v, c] : map) trigger_setter(x, prop, v, "Color", c);
    x.close("Style.Triggers");
    x.close("Style");
    x.close("local:FZTextBlock.Style");
}

// GLYPH(p): a 32 px controller picture picked by the int property p (FH2_GLYPH_ 1-24; 0 collapsed).
void glyph(Xml& x, const std::string& prop) {
    x.open("<local:FZMaterial Width=\"32\" Height=\"32\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\">");
    x.open("<local:FZMaterial.Style>");
    x.open("<Style TargetType=\"local:FZMaterial\">");
    x.line("<Setter Property=\"Visibility\" Value=\"Collapsed\" />");
    x.open("<Style.Triggers>");
    for (const Glyph& g : kGlyphs) {
        x.open("<DataTrigger Binding=\"{Binding FH2Mod_Hud_" + prop + "}\" Value=\"" + std::to_string(g.v) + "\">");
        x.line(std::string("<Setter Property=\"ImageSource\" Value=\"{StaticResource A_BitmapImage_ControllerButton_") + g.key + "}\" />");
        x.line("<Setter Property=\"Visibility\" Value=\"Visible\" />");
        x.close("DataTrigger");
    }
    x.close("Style.Triggers");
    x.close("Style");
    x.close("local:FZMaterial.Style");
    x.close("local:FZMaterial");
}

// PICTURE(name): one of the game's controller pictures, fixed.
void picture(Xml& x, const char* name, const std::string& extra = "") {
    x.line(std::string("<local:FZMaterial Width=\"32\" Height=\"32\" VerticalAlignment=\"Center\" ImageSource=\"{StaticResource "
                       "A_BitmapImage_ControllerButton_") +
           name + "}\"" + extra + " />");
}

// CHORD(p): LB + RB +, before a glyph, while p is true.
void chord(Xml& x, const std::string& prop) {
    x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + V(prop) + ">");
    picture(x, "LB");
    x.line(std::string("<local:FZTextBlock") + kSmall + " Text=\"+\" Margin=\"4,0,4,0\" VerticalAlignment=\"Center\" />");
    picture(x, "RB");
    x.line(std::string("<local:FZTextBlock") + kSmall + " Text=\"+\" Margin=\"4,0,4,0\" VerticalAlignment=\"Center\" />");
    x.close("StackPanel");
}

// CAP(t), and ACAP(t, a, g) with one arrow turned by its bound angle (Right 0, Down 90, Left 180, Up 270).
void cap(Xml& x, const std::string& text, const std::string& arrow, const std::string& angle, const std::string& outer = "") {
    x.open(std::string("<local:FZMaterial") + kRt + " Color=\"#B2F5F5F5\" VerticalAlignment=\"Center\"" + outer + ">");
    x.open(std::string("<local:FZMaterial") + kRt + " Color=\"#FF2F2F2F\" Margin=\"2\">");
    x.open("<Grid MinWidth=\"28\" Height=\"28\">");
    x.line(std::string("<local:FZTextBlock") + kSmall + T(text) + " Margin=\"6,0,6,0\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" />");
    if (!arrow.empty()) {
        x.open("<Path Data=\"M0,6 L12,6 M6,0 L12,6 L6,12\" Width=\"12\" Height=\"12\" Stroke=\"#FFF5F5F5\" StrokeThickness=\"2\" "
               "StrokeStartLineCap=\"Round\" StrokeEndLineCap=\"Round\" StrokeLineJoin=\"Round\" Stretch=\"Fill\" "
               "HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" RenderTransformOrigin=\"0.5,0.5\"" +
               V(arrow) + ">");
        x.open("<Path.RenderTransform>");
        x.line("<RotateTransform Angle=\"{Binding FH2Mod_Hud_" + angle + "}\" />");
        x.close("Path.RenderTransform");
        x.close("Path");
    }
    x.close("Grid");
    x.close("local:FZMaterial");
    x.close("local:FZMaterial");
}

// KEYS(x): a prompt's or the tip's key caps, x = "P<i>" or "Tip".
void keys(Xml& x, const std::string& p) {
    x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\">");
    cap(x, p + "K0", p + "K0Arrow", p + "K0Angle");
    x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + V(p + "K1Shown") + ">");
    x.line(std::string("<local:FZTextBlock") + kSmall + T(p + "Join") + " Margin=\"4,0,4,0\" VerticalAlignment=\"Center\" />");
    cap(x, p + "K1", p + "K1Arrow", p + "K1Angle");
    x.close("StackPanel");
    x.close("StackPanel");
}

// CHIP(i): dark glass, the text and five dots that hide themselves.
void chip(Xml& x, int i) {
    const std::string c = "C" + num(i);
    x.open(std::string("<local:FZMaterial") + kGlass + " Color=\"#CC101418\" Margin=\"" + (i == 4 ? "0" : "0,0,8,0") + "\"" +
           V(c + "Shown") + ">");
    x.open("<Border BorderThickness=\"2\" BorderBrush=\"#16FFFFFF\" CornerRadius=\"4\">");
    x.open("<StackPanel Orientation=\"Horizontal\" Margin=\"10,4,10,4\">");
    x.line(std::string("<local:FZTextBlock") + kSmall + T(c) + " VerticalAlignment=\"Center\"" + TRIM(i == 4 ? 320 : 200) + " />");
    for (int j = 0; j < 5; ++j) {
        x.open(std::string("<local:FZMaterial") + kRt + " Width=\"10\" Height=\"10\" Margin=\"3,0,3,0\" VerticalAlignment=\"Center\">");
        showstyle(x, "local:FZMaterial", c + "D" + num(j), {{"1", "#66F5F5F5"}, {"2", "#FFF5F5F5"}});
        x.close("local:FZMaterial");
    }
    x.close("StackPanel");
    x.close("Border");
    x.close("local:FZMaterial");
}

void opacity_storyboard(Xml& x, const char* to) {
    x.open("<BeginStoryboard>");
    x.open("<Storyboard>");
    x.line(std::string("<DoubleAnimation Storyboard.TargetProperty=\"(UIElement.Opacity)\" To=\"") + to + "\" Duration=\"0:0:0.15\" />");
    x.close("Storyboard");
    x.close("BeginStoryboard");
}

// FH2HL_Fade_<P>: the part's opacity follows FH2Mod_Hud_<P>, over 0.15 s either way.
void fade_style(Xml& x, const char* part) {
    x.open(std::string("<Style x:Key=\"FH2HL_Fade_") + part + "\" TargetType=\"Grid\">");
    x.line("<Setter Property=\"Opacity\" Value=\"0\" />");
    x.open("<Style.Triggers>");
    for (const bool on : {true, false}) {
        x.open(std::string("<DataTrigger Binding=\"{Binding FH2Mod_Hud_") + part + "}\" Value=\"" + (on ? "True" : "False") + "\">");
        x.open("<DataTrigger.EnterActions>");
        opacity_storyboard(x, on ? "1" : "0");
        x.close("DataTrigger.EnterActions");
        x.close("DataTrigger");
    }
    x.close("Style.Triggers");
    x.close("Style");
}

void key_frame(Xml& x, const char* time, const char* value, const char* ease) {
    if (!ease) {
        x.line(std::string("<EasingDoubleKeyFrame KeyTime=\"") + time + "\" Value=\"" + value + "\" />");
        return;
    }
    x.open(std::string("<EasingDoubleKeyFrame KeyTime=\"") + time + "\" Value=\"" + value + "\">");
    x.open("<EasingDoubleKeyFrame.EasingFunction>");
    x.line(std::string("<CircleEase EasingMode=\"") + ease + "\" />");
    x.close("EasingDoubleKeyFrame.EasingFunction");
    x.close("EasingDoubleKeyFrame");
}

// FH2HL_Root, as the check requires it (text compared with the white space between tags taken out):
// collapsed unless FH2Mod_Hud_Shown is true, so a page whose bindings never resolve shows nothing.
const char* const kRootStyle =
    "<Style x:Key=\"FH2HL_Root\" TargetType=\"Grid\"><Setter Property=\"Visibility\" Value=\"Collapsed\" /><Style.Triggers>"
    "<DataTrigger Binding=\"{Binding FH2Mod_Hud_Shown}\" Value=\"True\"><Setter Property=\"Visibility\" Value=\"Visible\" />"
    "</DataTrigger></Style.Triggers></Style>";

// ---- the layer, part by part ------------------------------------------------------------------------

void layer_chips(Xml& x) {
    x.open("<Grid Style=\"{StaticResource FH2HL_Fade_Chips}\"" + V("ChipsVis") + ">");
    x.open("<StackPanel Orientation=\"Horizontal\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Margin=\"108,56,0,0\">");
    for (int i = 0; i < 4; ++i) chip(x, i);
    x.close("StackPanel");
    x.open("<StackPanel Orientation=\"Horizontal\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Top\" Margin=\"0,56,108,0\">");
    chip(x, 4);
    x.close("StackPanel");
    x.close("Grid");
}

void layer_panel(Xml& x) {
    x.open("<Grid Grid.Row=\"0\" Style=\"{StaticResource FH2HL_Fade_Panel}\"" + V("PanelVis") +
           " HorizontalAlignment=\"Left\" VerticalAlignment=\"Bottom\" Margin=\"108,0,0,16\" Width=\"520\">");
    x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#B2000000\" />");
    x.open("<StackPanel Margin=\"0,8,0,8\">");
    for (int r = 0; r < 8; ++r) {
        const std::string p = "R" + num(r);
        x.open("<Grid Height=\"40\"" + V(p + "Shown") + ">");
        x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#FFFE5723\"" + V(p + "Sel") + " />");
        x.open("<Grid Margin=\"16,0,16,0\">");
        x.open("<Grid.ColumnDefinitions>");
        x.line("<ColumnDefinition Width=\"*\" />");
        x.line("<ColumnDefinition Width=\"Auto\" />");
        x.close("Grid.ColumnDefinitions");
        // Left: with MaxWidth in a wider star column, a stretched label is centred in it (demo run 1)
        x.open("<local:FZTextBlock" + T(p + "Label") + " HorizontalAlignment=\"Left\" VerticalAlignment=\"Center\"" + TRIM(300) + ">");
        text_style(x, "A_Style_FZTextBlock_HudSmall", p + "Style", "#FFF5F5F5", {{"1", "#99F5F5F5"}, {"2", "#FFFE5723"}});
        x.close("local:FZTextBlock");
        x.open("<StackPanel Grid.Column=\"1\" Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + V(p + "HasValue") + ">");
        x.line(std::string("<local:FZTextBlock") + kSmall + " Text=\"&lt;\" Color=\"#99F5F5F5\" Margin=\"0,0,8,0\" VerticalAlignment=\"Center\" />");
        x.open("<local:FZTextBlock" + T(p + "Value") + " VerticalAlignment=\"Center\"" + TRIM(200) + ">");
        text_style(x, "A_Style_FZTextBlock_HudSmall", p + "Style", "#FFF5F5F5", {{"1", "#99F5F5F5"}});
        x.close("local:FZTextBlock");
        x.line(std::string("<local:FZTextBlock") + kSmall + " Text=\"&gt;\" Color=\"#99F5F5F5\" Margin=\"8,0,0,0\" VerticalAlignment=\"Center\" />");
        x.close("StackPanel");
        x.close("Grid");
        x.close("Grid");
    }
    x.line(std::string("<local:FZTextBlock") + kSmall + T("PanelFooter") + " Color=\"#FFDEDEDE\" Margin=\"16,8,16,0\"" + TRIM(488) + " />");
    x.close("StackPanel");
    x.close("Grid");
}

void layer_tip(Xml& x) {
    x.open("<Grid Grid.Row=\"0\" Style=\"{StaticResource FH2HL_Fade_Tip}\"" + V("TipVis") +
           " HorizontalAlignment=\"Center\" VerticalAlignment=\"Bottom\" Margin=\"0,0,0,16\" Width=\"640\" Height=\"44\">");
    x.line(std::string("<local:FZMaterial") + kGlass + " Color=\"#CC101418\" />");
    x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#FFFE5723\" Width=\"4\" HorizontalAlignment=\"Left\" />");
    x.open("<Grid Margin=\"16,0,12,0\" VerticalAlignment=\"Center\" ClipToBounds=\"True\">");
    x.open("<Grid.ColumnDefinitions>");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.close("Grid.ColumnDefinitions");
    x.line(std::string("<local:FZTextBlock") + kSmall + T("TipBefore") +
           " Margin=\"0,0,8,0\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Center\"" + TRIM(280) + " />");
    x.open("<StackPanel Grid.Column=\"1\" Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + VN("TipKbd") + ">");
    chord(x, "TipChord");
    glyph(x, "TipG");
    x.close("StackPanel");
    x.open("<Grid Grid.Column=\"1\" VerticalAlignment=\"Center\"" + V("TipKbd") + ">");
    keys(x, "Tip");
    x.close("Grid");
    x.line(std::string("<local:FZTextBlock Grid.Column=\"2\"") + kSmall + T("TipAfter") +
           " Margin=\"8,0,0,0\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Center\"" + TRIM(280) + " />");
    x.close("Grid");
    x.close("Grid");
}

void bar_strip_row(Xml& x) {
    x.open("<Grid Grid.Row=\"0\" Height=\"56\"" + V("StripRow") + ">");
    x.open("<Grid Margin=\"108,0,108,0\">");
    x.open("<Grid.ColumnDefinitions>");
    x.line("<ColumnDefinition Width=\"96\" />");
    x.line("<ColumnDefinition Width=\"1104\" />");
    x.line("<ColumnDefinition Width=\"96\" />");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.close("Grid.ColumnDefinitions");
    x.line(std::string("<local:FZTextBlock") + kSmall + T("RailLeft") +
           " Color=\"#FFDEDEDE\" HorizontalAlignment=\"Right\" VerticalAlignment=\"Top\" Margin=\"0,4,12,0\"" + TRIM(84) + " />");
    x.open("<Grid Grid.Column=\"1\" Width=\"1104\" Height=\"56\">");
    x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#66F5F5F5\" Height=\"2\" VerticalAlignment=\"Top\" Margin=\"0,15,0,0\"" +
           V("Rail") + " />");
    for (int k = 0; k < 16; ++k) {
        const std::string m = "Mark" + num(k);
        x.open("<Grid Width=\"40\" Height=\"56\" HorizontalAlignment=\"Left\"" + V(m + "Shown") + ">");
        x.open("<Grid.RenderTransform>");
        x.line("<TranslateTransform X=\"{Binding FH2Mod_Hud_" + m + "X}\" />");
        x.close("Grid.RenderTransform");
        x.open(std::string("<local:FZMaterial") + kRt +
               " Width=\"12\" Height=\"12\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Top\" Margin=\"0,10,0,0\" "
               "RenderTransformOrigin=\"0.5,0.5\">");
        x.open("<local:FZMaterial.RenderTransform>");
        x.line("<RotateTransform Angle=\"{Binding FH2Mod_Hud_" + m + "Angle}\" />");
        x.close("local:FZMaterial.RenderTransform");
        coloured(x, "local:FZMaterial", m + "Style", "#FFF5F5F5", {{"1", "#FFFE5723"}, {"2", "#66F5F5F5"}});
        x.close("local:FZMaterial");
        x.line(std::string("<local:FZTextBlock") + kSmall + T(m + "Num") +
               " HorizontalAlignment=\"Center\" VerticalAlignment=\"Top\" Margin=\"0,24,0,0\" />");
        x.close("Grid");
    }
    x.open(std::string("<local:FZMaterial") + kRt +
           " Color=\"#FFF5F5F5\" Width=\"3\" Height=\"30\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Margin=\"0,1,0,0\"" +
           V("Playhead") + ">");
    x.open("<local:FZMaterial.RenderTransform>");
    x.line("<TranslateTransform X=\"{Binding FH2Mod_Hud_PlayheadX}\" />");
    x.close("local:FZMaterial.RenderTransform");
    x.close("local:FZMaterial");
    x.close("Grid");
    x.line(std::string("<local:FZTextBlock Grid.Column=\"2\"") + kSmall + T("RailRight") +
           " Color=\"#FFDEDEDE\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Margin=\"12,4,0,0\"" + TRIM(84) + " />");
    x.line(std::string("<local:FZTextBlock Grid.Column=\"3\"") + kSmall + T("RowAfter") +
           " Color=\"#FFDEDEDE\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Margin=\"24,4,0,0\"" + TRIM(384) + " />");
    x.close("Grid");
    x.close("Grid");
}

void bar_tabs(Xml& x) {
    x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + V("TabsShown") + ">");
    x.open("<Grid VerticalAlignment=\"Center\" Margin=\"0,0,6,0\">");
    picture(x, "LB", VN("TabKbd"));
    cap(x, "TabKeyL", "", "", V("TabKbd"));
    x.close("Grid");
    for (int i = 0; i < 8; ++i) {
        const std::string t = "Tab" + num(i);
        x.open("<Grid Margin=\"6,0,6,0\" VerticalAlignment=\"Center\"" + V(t + "Shown") + ">");
        x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#FFF5F5F5\"" + V(t + "Sel") + " />");
        x.line(std::string("<local:FZTextBlock") + kSmall + T(t) + " Margin=\"12,2,12,2\"" + VN(t + "Sel") + TRIM(200) + " />");
        x.line(std::string("<local:FZTextBlock") + kSmall + T(t) + " Margin=\"12,2,12,2\" Color=\"#FF2F2F2F\" Effect=\"{x:Null}\"" +
               V(t + "Sel") + TRIM(200) + " />");
        x.close("Grid");
    }
    x.open("<Grid VerticalAlignment=\"Center\" Margin=\"6,0,0,0\">");
    picture(x, "RB", VN("TabKbd"));
    cap(x, "TabKeyR", "", "", V("TabKbd"));
    x.close("Grid");
    x.close("StackPanel");
}

void bar_middle(Xml& x) {
    x.open("<Grid Grid.Column=\"1\" ClipToBounds=\"True\">");
    x.open(std::string("<local:FZMaterial") + kRt + " HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" Margin=\"24,0,24,0\" MaxWidth=\"760\">");
    coloured(x, "local:FZMaterial", "Alert", "#00000000", {{"True", "#FFDD791F"}});
    x.open("<Grid Margin=\"16,6,16,6\">");
    x.open("<Grid.RowDefinitions>");
    x.line("<RowDefinition Height=\"Auto\" />");
    x.line("<RowDefinition Height=\"Auto\" />");
    x.close("Grid.RowDefinitions");
    x.open("<Grid Grid.Row=\"0\">");
    x.open("<Grid.ColumnDefinitions>");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.close("Grid.ColumnDefinitions");
    x.line(std::string("<local:FZTextBlock") + kMedium + T("L1") + " VerticalAlignment=\"Center\"" + TRIM(520) + " />");
    x.open("<local:FZTextBlock Grid.Column=\"1\"" + T("L1After") + " Margin=\"10,0,0,0\" VerticalAlignment=\"Center\"" + TRIM(240) + ">");
    text_style(x, "A_Style_FZTextBlock_HudMedium", "L1Style", "#FFDEDEDE", {{"1", "#FFFFFF4B"}, {"2", "#FFFE5723"}});
    x.close("local:FZTextBlock");
    x.close("Grid");
    x.open("<Grid Grid.Row=\"1\">");
    x.open("<Grid.ColumnDefinitions>");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.close("Grid.ColumnDefinitions");
    x.line(std::string("<local:FZTextBlock") + kSmall + T("L2") + " Color=\"#FFDEDEDE\" VerticalAlignment=\"Center\"" + TRIM(600) + " />");
    x.open("<StackPanel Grid.Column=\"1\" Orientation=\"Horizontal\" Margin=\"10,0,0,0\" VerticalAlignment=\"Center\"" + V("Dots") + ">");
    for (int j = 0; j < 8; ++j) {
        x.open(std::string("<local:FZMaterial") + kRt + " Width=\"10\" Height=\"10\" Margin=\"4,0,4,0\" VerticalAlignment=\"Center\">");
        showstyle(x, "local:FZMaterial", "Dot" + num(j), {{"1", "#FFF5F5F5"}, {"2", "#FFFE5723"}, {"3", "#66F5F5F5"}});
        x.close("local:FZMaterial");
    }
    x.close("StackPanel");
    x.line(std::string("<local:FZTextBlock Grid.Column=\"2\"") + kSmall + T("InlineAfter") +
           " Color=\"#FFDEDEDE\" Margin=\"10,0,0,0\" VerticalAlignment=\"Center\"" + TRIM(240) + " />");
    x.close("Grid");
    x.close("Grid");
    x.close("local:FZMaterial");
    x.close("Grid");
}

void bar_prompts(Xml& x) {
    x.open("<StackPanel Grid.Column=\"2\" Orientation=\"Horizontal\" VerticalAlignment=\"Center\">");
    for (int i = 4; i >= 0; --i) {   // P4 first, so P0 is the rightmost (the game's help-bar order)
        const std::string p = "P" + num(i);
        x.open("<StackPanel Orientation=\"Horizontal\" Margin=\"16,0,0,0\" VerticalAlignment=\"Center\"" + V(p + "Shown") + ">");
        x.line(std::string("<local:FZTextBlock") + kSmall + T("HoldWord") + " Margin=\"0,0,6,0\" VerticalAlignment=\"Center\"" + V(p + "Hold") +
               " />");
        x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + VN(p + "Kbd") + ">");
        chord(x, p + "Chord");
        x.open("<Grid Width=\"40\" Height=\"40\" VerticalAlignment=\"Center\">");
        glyph(x, p + "G");
        x.open("<ed:Arc Width=\"40\" Height=\"40\" ArcThickness=\"3\" ArcThicknessUnit=\"Pixel\" StartAngle=\"0\" "
               "EndAngle=\"{Binding FH2Mod_Hud_HoldAngle}\" Stretch=\"None\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\"" +
               V(p + "Hold") + ">");
        x.open("<ed:Arc.Fill>");
        x.line("<SolidColorBrush Color=\"#FFFE5723\" />");
        x.close("ed:Arc.Fill");
        x.close("ed:Arc");
        x.close("Grid");
        x.close("StackPanel");
        x.open("<Grid VerticalAlignment=\"Center\"" + V(p + "Kbd") + ">");
        keys(x, p);
        x.open(std::string("<local:FZMaterial") + kRt +
               " Color=\"#FFFE5723\" Height=\"4\" HorizontalAlignment=\"Stretch\" VerticalAlignment=\"Bottom\" Margin=\"0,0,0,-6\" "
               "RenderTransformOrigin=\"0,0.5\"" +
               V(p + "Hold") + ">");
        x.open("<local:FZMaterial.RenderTransform>");
        x.line("<ScaleTransform ScaleX=\"{Binding FH2Mod_Hud_HoldFill}\" ScaleY=\"1\" />");
        x.close("local:FZMaterial.RenderTransform");
        x.close("local:FZMaterial");
        x.close("Grid");
        x.line(std::string("<local:FZTextBlock") + kSmall + T(p + "Label") + " Margin=\"8,0,0,0\" VerticalAlignment=\"Center\"" + TRIM(260) + " />");
        x.close("StackPanel");
    }
    x.close("StackPanel");
}

void layer_bar(Xml& x) {
    x.open("<Grid Grid.Row=\"1\" Style=\"{StaticResource FH2HL_Fade_Bar}\"" + V("BarVis") + ">");
    x.open("<Grid.RowDefinitions>");
    x.line("<RowDefinition Height=\"Auto\" />");
    x.line("<RowDefinition Height=\"84\" />");
    x.close("Grid.RowDefinitions");
    x.line(std::string("<local:FZMaterial Grid.RowSpan=\"2\"") + kRt + " Color=\"#D8000000\" />");
    bar_strip_row(x);
    x.open("<Grid Grid.Row=\"1\" Margin=\"108,0,108,0\">");
    x.open("<Grid.ColumnDefinitions>");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.line("<ColumnDefinition Width=\"*\" />");
    x.line("<ColumnDefinition Width=\"Auto\" />");
    x.close("Grid.ColumnDefinitions");
    bar_tabs(x);
    bar_middle(x);
    bar_prompts(x);
    x.close("Grid");
    x.close("Grid");
}

// The feed: up to five lines at the left, over the game's minimap, the newest at the bottom (F0). Each is the
// name in white with a colon, then the text in its colour, as a chat log. No backing: the HUD's own text style
// carries its shadow, as the game's HUD texts over the world do.
void layer_feed(Xml& x) {
    x.open("<Grid Style=\"{StaticResource FH2HL_Fade_Feed}\"" + V("FeedVis") + " HorizontalAlignment=\"Left\" VerticalAlignment=\"Bottom\" Margin=\"108,0,0," +
           num(kHudFeedBottom) + "\" Width=\"760\">");
    x.open("<StackPanel VerticalAlignment=\"Bottom\">");
    for (int i = kHudFeedLines - 1; i >= 0; --i) {   // top to bottom: F4 (oldest) ... F0 (newest)
        const std::string f = "F" + num(i);
        x.open("<StackPanel Orientation=\"Horizontal\" Height=\"36\"" + V(f + "Shown") + ">");
        x.open("<StackPanel Orientation=\"Horizontal\" VerticalAlignment=\"Center\"" + V(f + "HasName") + ">");
        x.line(std::string("<local:FZTextBlock") + kMedium + T(f + "Name") + " Color=\"#FFF5F5F5\" VerticalAlignment=\"Center\"" + TRIM(300) + " />");
        x.line(std::string("<local:FZTextBlock") + kMedium + " Text=\":\" Color=\"#FFF5F5F5\" Margin=\"0,0,10,0\" VerticalAlignment=\"Center\" />");
        x.close("StackPanel");
        x.open("<local:FZTextBlock" + T(f + "Text") + " VerticalAlignment=\"Center\"" + TRIM(720) + ">");
        text_style(x, "A_Style_FZTextBlock_HudMedium", f + "Style", "#FFF5F5F5", {{"1", "#FF52D273"}, {"2", "#FFFE5723"}, {"3", "#FFFFD400"}});
        x.close("local:FZTextBlock");
        x.close("StackPanel");
    }
    x.close("StackPanel");
    x.close("Grid");
}

// The canvases, under the layer's other parts: for each, a Grid over the whole page that shows while its layer
// has the screen. A canvas's XAML goes in at its placeholder once it has passed its own check (hud_splice).
void layer_canvas(Xml& x, const std::vector<HudCanvas>& canvases) {
    x.open("<Grid Style=\"{StaticResource FH2HL_Fade_Canvas}\"" + V("CanvasVis") + ">");
    for (const HudCanvas& c : canvases) {
        x.open("<Grid" + V("Cv" + num(c.layer) + "On") + ">");
        x.line(hud_canvas_placeholder(c.layer));
        x.close("Grid");
    }
    x.close("Grid");
}

void layer_count(Xml& x) {
    x.open("<Grid Style=\"{StaticResource FH2HL_Fade_Count}\"" + V("CountVis") + ">");
    x.open("<ed:Arc Width=\"220\" Height=\"220\" ArcThickness=\"4\" ArcThicknessUnit=\"Pixel\" StartAngle=\"0\" EndAngle=\"360\" "
           "Stretch=\"None\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\">");
    x.open("<ed:Arc.Fill>");
    x.line("<SolidColorBrush Color=\"#FFFE5723\" />");
    x.close("ed:Arc.Fill");
    x.close("ed:Arc");
    // The pop scales a Grid round the number, not the text block: in the preview 2 re-check a pop on the text
    // block's own transform never ran, while the probe's Grid pop and the platform's own page zoom do
    x.open("<Grid Style=\"{StaticResource FH2HL_CountPop}\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" "
           "RenderTransformOrigin=\"0.5,0.5\">");
    x.open("<Grid.RenderTransform>");
    x.open("<TransformGroup>");
    x.line("<ScaleTransform />");
    x.close("TransformGroup");
    x.close("Grid.RenderTransform");
    x.line("<local:FZTextBlock Style=\"{StaticResource FH2HL_Count}\"" + T("CountText") +
           " HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" />");
    x.close("Grid");
    x.open(std::string("<local:FZMaterial") + kGlass + " Color=\"#CC101418\" HorizontalAlignment=\"Center\" VerticalAlignment=\"Center\" "
           "Margin=\"0,300,0,0\"" + V("CountLabelShown") + ">");
    x.line(std::string("<local:FZTextBlock") + kSmall + T("CountLabel") + " Margin=\"12,4,12,4\"" + TRIM(600) + " />");
    x.close("local:FZMaterial");
    x.close("Grid");
}

} // namespace

std::string hud_resources_markup(uint32_t parts) {
    Xml x(12);
    x.line("<!-- FH2 Mod Platform: HUD layer resources -->");
    x.open("<Style x:Key=\"FH2HL_Root\" TargetType=\"Grid\">");
    x.line("<Setter Property=\"Visibility\" Value=\"Collapsed\" />");
    x.open("<Style.Triggers>");
    trigger_setter(x, "Shown", "True", "Visibility", "Visible");
    x.close("Style.Triggers");
    x.close("Style");
    for (int k = 0; k < kHudParts; ++k)
        if (has(parts, k)) fade_style(x, kHudPartProps[k]);
    if (has(parts, kCount)) {
        // The countdown's number: its size only.
        x.open("<Style x:Key=\"FH2HL_Count\" TargetType=\"{x:Type local:FZTextBlock}\" BasedOn=\"{StaticResource A_Style_FZTextBlock_HudVeryLarge}\">");
        x.line("<Setter Property=\"FontSize\" Value=\"152\" />");
        x.close("Style");
        // The countdown's pop: one key-frame animation on the ScaleTransform of the Grid round the number, with
        // no values of its own (the shape of the game's repeatable pop, HUDSkillTokenControl.xaml 34-66),
        // restarted by CountPop going from False to True for each number, so a number always shows at its size
        // even if a pop doesn't run. On a Grid, as the game only scales Grids and materials, never a text block
        // (in the preview 2 re-check a pop on the FZTextBlock's own transform never ran). The exit stops the
        // storyboard, as the game's HUD pages do (AR_BucketListActivation.xaml): in demo run 1 a finished pop
        // left holding the scale let no later pop run.
        x.open("<Style x:Key=\"FH2HL_CountPop\" TargetType=\"Grid\">");
        x.open("<Style.Triggers>");
        x.open("<DataTrigger Binding=\"{Binding FH2Mod_Hud_CountPop}\" Value=\"True\">");
        x.open("<DataTrigger.EnterActions>");
        x.open("<BeginStoryboard x:Name=\"FH2HL_CountPopBegin\">");
        x.open("<Storyboard>");
        for (const char* axis : {"ScaleX", "ScaleY"}) {
            x.open(std::string("<DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.RenderTransform).(TransformGroup.Children)[0]."
                               "(ScaleTransform.") +
                   axis + ")\">");
            key_frame(x, "0", "1.35", nullptr);
            key_frame(x, "0:0:0.25", "1", "EaseOut");
            x.close("DoubleAnimationUsingKeyFrames");
        }
        x.close("Storyboard");
        x.close("BeginStoryboard");
        x.close("DataTrigger.EnterActions");
        x.open("<DataTrigger.ExitActions>");
        x.line("<StopStoryboard BeginStoryboardName=\"FH2HL_CountPopBegin\" />");
        x.close("DataTrigger.ExitActions");
        x.close("DataTrigger");
        x.close("Style.Triggers");
        x.close("Style");
    }
    x.open("<Style x:Key=\"FH2HL_Shutter\" TargetType=\"local:FZMaterial\">");
    x.line("<Setter Property=\"Opacity\" Value=\"0\" />");
    x.open("<Style.Triggers>");
    x.open("<DataTrigger Binding=\"{Binding FH2Mod_Hud_Shutter}\" Value=\"True\">");
    x.open("<DataTrigger.EnterActions>");
    x.open("<BeginStoryboard x:Name=\"FH2HL_ShutterBegin\">");
    x.open("<Storyboard>");
    x.open("<DoubleAnimationUsingKeyFrames Storyboard.TargetProperty=\"(UIElement.Opacity)\" Duration=\"0:0:0.1\">");
    key_frame(x, "0", "0.9", nullptr);
    key_frame(x, "0:0:0.1", "0", "EaseIn");
    x.close("DoubleAnimationUsingKeyFrames");
    x.close("Storyboard");
    x.close("BeginStoryboard");
    x.close("DataTrigger.EnterActions");
    x.open("<DataTrigger.ExitActions>");   // stopped as the countdown's pop is, so a second blink runs too
    x.line("<StopStoryboard BeginStoryboardName=\"FH2HL_ShutterBegin\" />");
    x.close("DataTrigger.ExitActions");
    x.close("DataTrigger");
    x.close("Style.Triggers");
    x.close("Style");
    x.line("<!-- FH2 Mod Platform: end of the HUD layer resources -->");
    return x.s;
}

std::string hud_canvas_placeholder(int layer) { return "<!-- FH2 Mod Platform: canvas " + std::to_string(layer) + " -->"; }

std::string hud_layer_markup(uint32_t parts, const std::vector<HudCanvas>& canvases) {
    Xml x(12);
    x.line("<!-- FH2 Mod Platform: HUD layer -->");
    x.open("<Grid x:Name=\"FH2HL_Root\" Style=\"{StaticResource FH2HL_Root}\">");
    if (has(parts, kCanvas)) layer_canvas(x, canvases);
    if (has(parts, kChips)) layer_chips(x);
    if (has(parts, kBar) || has(parts, kTip) || has(parts, kPanel)) {
        // the bar in row 1, the panel and the tip in row 0, 16 px above whatever is below them
        x.open("<Grid VerticalAlignment=\"Bottom\">");
        x.open("<Grid.RowDefinitions>");
        x.line("<RowDefinition Height=\"Auto\" />");
        x.line("<RowDefinition Height=\"Auto\" />");
        x.close("Grid.RowDefinitions");
        if (has(parts, kPanel)) layer_panel(x);
        if (has(parts, kTip)) layer_tip(x);
        if (has(parts, kBar)) layer_bar(x);
        x.close("Grid");
    }
    if (has(parts, kCount)) layer_count(x);
    if (has(parts, kFeed)) layer_feed(x);
    x.line(std::string("<local:FZMaterial") + kRt + " Color=\"#FFFFFFFF\" Style=\"{StaticResource FH2HL_Shutter}\" />");
    x.close("Grid");
    x.line("<!-- FH2 Mod Platform: end of the HUD layer -->");
    return x.s;
}

namespace {

// ---- the check before set_file ----------------------------------------------------------------------

std::size_t count_of(const std::string& s, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + what.size())) ++n;
    return n;
}

// One tag as scan_xml finds it.
struct XTag {
    std::size_t at = 0, end = 0;     // its '<', and one past its '>'
    int depth      = 0;              // the element's depth: 0 at the top level of the text
    bool close = false, self = false;
    std::string_view name, parent;   // the element's name, and the enclosing element's ("" at the top level)
    std::string_view text;           // the whole tag
};

bool xml_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Calls f(tag) for every start, self-closing and end tag of `xml`, in order, skipping comments,
// <?...?>, <!...> and CDATA. False, with why, if the tags don't balance.
template <class F> bool scan_xml(const std::string& xml, F&& f, std::string& why) {
    std::vector<std::string_view> open;
    const std::string_view s(xml);
    std::size_t i = 0;
    while ((i = s.find('<', i)) != std::string_view::npos) {
        const char* skip_to = s.compare(i, 4, "<!--") == 0        ? "-->"
                              : s.compare(i, 9, "<![CDATA[") == 0 ? "]]>"
                              : s.compare(i, 2, "<?") == 0        ? "?>"
                              : s.compare(i, 2, "<!") == 0        ? ">"
                                                                  : nullptr;
        if (skip_to) {
            const std::size_t e = s.find(skip_to, i + 2);
            if (e == std::string_view::npos) return why = "a comment or declaration isn't closed", false;
            i = e + std::strlen(skip_to);
            continue;
        }
        std::size_t e = i + 1;
        char quote    = 0;
        for (; e < s.size(); ++e) {
            const char c = s[e];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (e >= s.size()) return why = "a tag isn't closed", false;
        XTag t;
        t.at    = i;
        t.end   = e + 1;
        t.close = s[i + 1] == '/';
        t.self  = !t.close && s[e - 1] == '/';
        std::size_t n0 = i + (t.close ? 2 : 1), n1 = n0;
        while (n1 < e && !xml_space(s[n1]) && s[n1] != '/' && s[n1] != '>') ++n1;
        t.name = s.substr(n0, n1 - n0);
        t.text = s.substr(i, e + 1 - i);
        if (t.name.empty()) return why = "a tag has no name", false;
        if (t.close) {
            if (open.empty() || open.back() != t.name)
                return why = "</" + std::string(t.name) + "> closes " +
                             (open.empty() ? std::string("nothing") : "<" + std::string(open.back()) + ">"),
                       false;
            open.pop_back();
        }
        t.depth  = static_cast<int>(open.size());
        t.parent = open.empty() ? std::string_view() : open.back();
        if (!t.close && !t.self) open.push_back(t.name);
        f(static_cast<const XTag&>(t));
        i = e + 1;
    }
    if (!open.empty()) return why = "<" + std::string(open.back()) + "> isn't closed", false;
    return true;
}

// Calls f(name, value) for each attribute of a start tag.
template <class F> void each_attr(std::string_view tag, F&& f) {
    std::size_t i = 1;
    while (i < tag.size() && !xml_space(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;   // the element's name
    while (i < tag.size()) {
        while (i < tag.size() && xml_space(tag[i])) ++i;
        const std::size_t n0 = i;
        while (i < tag.size() && tag[i] != '=' && !xml_space(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;
        const std::string_view name = tag.substr(n0, i - n0);
        while (i < tag.size() && xml_space(tag[i])) ++i;
        if (name.empty() || i >= tag.size() || tag[i] != '=') {
            if (name.empty()) ++i;
            continue;
        }
        ++i;
        while (i < tag.size() && xml_space(tag[i])) ++i;
        if (i >= tag.size() || (tag[i] != '"' && tag[i] != '\'')) return;
        const char quote    = tag[i];
        const std::size_t e = tag.find(quote, i + 1);
        if (e == std::string_view::npos) return;
        f(name, tag.substr(i + 1, e - i - 1));
        i = e + 1;
    }
}

bool attr(std::string_view tag, std::string_view name, std::string_view& value) {
    bool found = false;
    each_attr(tag, [&](std::string_view n, std::string_view v) {
        if (!found && n == name) found = true, value = v;
    });
    return found;
}

// "{x:Type local:FZTextBlock}" and "local:FZTextBlock" -> "FZTextBlock"
std::string type_name(std::string_view t) {
    if (t.rfind("{x:Type ", 0) == 0 && !t.empty() && t.back() == '}') t = t.substr(8, t.size() - 9);
    while (!t.empty() && xml_space(t.front())) t.remove_prefix(1);
    while (!t.empty() && xml_space(t.back())) t.remove_suffix(1);
    const std::size_t colon = t.find(':');
    return std::string(colon == std::string_view::npos ? t : t.substr(colon + 1));
}

// A resource a page can use by its key: its element and, for a Style, its target type.
struct KeyDef {
    std::string element, target;
    std::size_t at = 0, end = 0;   // the platform's own: where its element is in the resources block
};

// The keys a dictionary gives pages: x:Key on the children of the document's own ResourceDictionary
// (the root, or Application.Resources' one), never a key nested in a Style's or a template's own
// resources, and never one in a comment.
bool collect_dict_keys(const std::string& xaml, std::map<std::string, KeyDef>& keys, std::string& why) {
    int dict = -1;   // the depth of the document's own ResourceDictionary while it's open
    return scan_xml(
        xaml,
        [&](const XTag& t) {
            if (t.name == "ResourceDictionary" && !t.close && dict < 0 &&
                (t.depth == 0 || (t.depth == 2 && t.parent == "Application.Resources"))) {
                if (!t.self) dict = t.depth;
                return;
            }
            if (t.close && t.name == "ResourceDictionary" && t.depth == dict) {
                dict = -1;
                return;
            }
            std::string_view key, target;
            if (dict < 0 || t.close || t.depth != dict + 1 || !attr(t.text, "x:Key", key)) return;
            KeyDef d;
            d.element = std::string(t.name);
            if (attr(t.text, "TargetType", target)) d.target = type_name(target);
            keys.emplace(std::string(key), d);
        },
        why);
}

// Every {StaticResource K} and {DynamicResource K} in one attribute value, with which kind it is.
template <class F> void each_resource_use(std::string_view v, F&& f) {
    for (const char* kind : {"{StaticResource ", "{DynamicResource "}) {
        const std::size_t klen = std::strlen(kind);
        for (std::size_t at = v.find(kind); at != std::string_view::npos; at = v.find(kind, at + 1)) {
            const std::size_t b = at + klen;
            std::size_t e       = b;
            while (e < v.size() && v[e] != '}' && v[e] != ',' && !xml_space(v[e])) ++e;
            f(v.substr(b, e - b), kind[1] == 'S');
        }
    }
}

// Which property type an attribute bound to a platform property needs; 0 (with why) for a binding the
// check doesn't know.
char bound_kind(const XTag& t, std::string_view name, std::string_view value, std::string& why) {
    if (name == "Visibility") {
        if (value.find(", Converter={StaticResource A_FZBooleanToVisibilityConverter}}") != std::string_view::npos ||
            value.find(", Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}") != std::string_view::npos)
            return 'b';
        why = "Visibility is bound without the boolean converter";
        return 0;
    }
    if (value.find(", Converter=") != std::string_view::npos) {
        why = "it's bound through " + std::string(name) + " with a converter, which only Visibility may have";
        return 0;
    }
    if (name == "Binding" && t.name == "DataTrigger") {
        std::string_view v;
        if (!attr(t.text, "Value", v)) return why = "a DataTrigger has no Value", 0;
        if (v == "True" || v == "False") return 'b';
        bool digits = !v.empty();
        for (char c : v) digits = digits && c >= '0' && c <= '9';
        if (digits) return 'i';
        why = "a DataTrigger's Value \"" + std::string(v) + "\" is neither True, False nor a number";
        return 0;
    }
    if (name == "Text") return 's';
    if (name == "ScaleX" || name == "ScaleY" || name == "X" || name == "Y" || name == "EndAngle" || name == "Angle" || name == "Opacity")
        return 'f';
    why = "it's bound through " + std::string(name) + ", which the check doesn't know";
    return 0;
}

// Every Style goes on an element of its target type, which the game checks as the page loads: a
// Style="{StaticResource K}" names a Style whose TargetType is the element's own type, and an inline
// Style sits in an <X.Style> inside an X and targets X. The platform's keyed Styles sit at the top of
// the resources block; a Style anywhere else is refused.
bool styles_fit(const std::vector<XTag>& rt, const std::vector<XTag>& lt, const std::map<std::string, KeyDef>& own,
                const std::map<std::string, KeyDef>& known, std::string& why) {
    for (const auto* tags : {&rt, &lt}) {
        std::vector<std::string_view> open;   // the open elements' names, by depth
        for (const XTag& t : *tags) {
            if (t.close) continue;
            open.resize(static_cast<std::size_t>(t.depth));
            open.push_back(t.name);
            const std::string element = type_name(t.name);
            std::string_view v;
            if (attr(t.text, "Style", v)) {
                std::string_view k;
                for (const char* kind : {"{StaticResource ", "{DynamicResource "})
                    if (v.rfind(kind, 0) == 0 && v.size() > std::strlen(kind) + 1 && v.back() == '}')
                        k = v.substr(std::strlen(kind), v.size() - std::strlen(kind) - 1);
                if (k.empty()) return why = "a <" + std::string(t.name) + ">'s Style \"" + std::string(v) + "\" isn't a resource", false;
                const auto o    = own.find(std::string(k));
                const auto a    = known.find(std::string(k));
                const KeyDef* d = o != own.end() ? &o->second : a != known.end() ? &a->second : nullptr;
                if (!d) return why = "the Style " + std::string(k) + " on a <" + std::string(t.name) + "> isn't defined", false;
                if (d->element != "Style")
                    return why = "a <" + std::string(t.name) + ">'s Style is " + std::string(k) + ", which isn't a Style", false;
                if (d->target != element)
                    return why = "the Style " + std::string(k) + " (for " + (d->target.empty() ? std::string("no type") : d->target) +
                                 ") is on a <" + std::string(t.name) + ">",
                           false;
            }
            if (t.name != "Style") continue;
            if (tags == &rt && t.depth == 0) continue;   // one of the platform's keyed Styles: checked where it's used
            const std::string_view holder = t.parent;
            const bool prop_el           = holder.size() > 6 && holder.substr(holder.size() - 6) == ".Style";
            const std::string_view owner = prop_el ? holder.substr(0, holder.size() - 6) : std::string_view();
            const std::string_view in    = t.depth >= 2 ? open[static_cast<std::size_t>(t.depth) - 2] : std::string_view();
            if (owner.empty() || in != owner)
                return why = "a Style sits in a <" + std::string(holder) + ">" + (in.empty() ? "" : " inside a <" + std::string(in) + ">") +
                             ", not in an element's own <X.Style>",
                       false;
            std::string_view tt;
            const std::string target = attr(t.text, "TargetType", tt) ? type_name(tt) : std::string();
            if (target != type_name(owner))
                return why = "the Style in a <" + std::string(holder) + "> targets " + (target.empty() ? std::string("nothing") : target) +
                             ", not " + type_name(owner),
                       false;
        }
    }
    return true;
}

bool balanced(const HudUi& ui, const std::string& xml, std::string& why) {
    if (ui.balanced && !ui.balanced(xml, why)) return false;
    return scan_xml(xml, [](const XTag&) {}, why);
}

std::size_t line_start(const std::string& page, std::size_t at) {
    const std::size_t nl = page.rfind('\n', at);
    return nl == std::string::npos ? 0 : nl + 1;
}

// The keys every page has: AnthemApp.xaml's and those of the dictionaries it merges, each at its own level.
bool app_keys(const HudUi& ui, std::map<std::string, KeyDef>& known, int& dicts, std::string& why) {
    std::string tag_why;
    const std::string app = ui.read ? ui.read("AnthemApp.xaml") : std::string();
    if (app.empty()) return why = "AnthemApp.xaml couldn't be read from UI.zip", false;
    if (!collect_dict_keys(app, known, tag_why)) return why = "AnthemApp.xaml's tags: " + tag_why, false;
    const std::string src = "Source=\"Resources/Anthem/";
    for (std::size_t at = app.find(src); at != std::string::npos; at = app.find(src, at + 1)) {
        const std::size_t b = at + 8, e = app.find('"', b);
        if (e == std::string::npos) continue;
        const std::string path = app.substr(b, e - b);
        const std::string d    = ui.read(path.c_str());
        if (d.empty()) return why = path + " couldn't be read from UI.zip", false;
        if (!collect_dict_keys(d, known, tag_why)) return why = path + "'s tags: " + tag_why, false;
        ++dicts;
    }
    return true;
}

bool ends_with(std::string_view s, std::string_view tail) { return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail; }

// A mod's canvas before it goes into the page (FH2HudCanvas in fh2mod_hud.h says what passes): its tags balance,
// its elements are the page's kinds, every {...} is a binding to one of its own values where that kind of value
// goes, a key every page has, x:Type or x:Null, and it brings no names, keys, resources or pictures of its own.
// Then `block` gets the XAML with its N, T and B names made the page's (FH2Mod_Hud_Cv<layer>...), indented for
// the page. False with why.
bool canvas_block(const HudCanvas& c, const std::map<std::string, KeyDef>& known, const HudUi& ui, std::string& block, std::size_t& bindings,
                  bool& ed, std::string& why) {
    bindings = 0;
    ed       = false;
    if (c.xaml.find_first_not_of(" \t\r\n") == std::string::npos) return why = "its XAML is empty", false;
    std::string tag_why;
    if (!balanced(ui, c.xaml, tag_why)) return why = "its tags: " + tag_why, false;
    std::vector<XTag> tags;
    if (!scan_xml(c.xaml, [&](const XTag& t) { tags.push_back(t); }, tag_why)) return why = "its tags: " + tag_why, false;
    static const char* const kNoAttr[] = {"x:Name", "Name", "x:Key", "x:Class", "x:Uid", "Source", "ImageSource", "UriSource"};
    static const char* const kConv[]   = {", Converter={StaticResource A_FZBooleanToVisibilityConverter}}",
                                          ", Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}"};
    for (const XTag& t : tags) {
        if (t.close) continue;
        const std::string name(t.name);
        const std::size_t colon = t.name.find(':');
        if (colon != std::string_view::npos && t.name.substr(0, colon) != "local" && t.name.substr(0, colon) != "ed")
            return why = "<" + name + "> has a prefix the HUD page doesn't have (it has local: and ed:)", false;
        if (ends_with(t.name, ".Resources") || t.name == "ResourceDictionary")
            return why = "<" + name + ">: a canvas can't have resources of its own", false;
        if (t.name.rfind("ed:", 0) == 0) ed = true;
        std::string bad;
        each_attr(t.text, [&](std::string_view an, std::string_view av) {
            if (!bad.empty()) return;
            const std::string a(an), v(av);
            for (const char* no : kNoAttr)
                if (an == no) bad = a + " on a <" + name + ">: a canvas can't have names, keys or pictures of its own";
            if (an.rfind("xmlns", 0) == 0) bad = a + " on a <" + name + ">: a canvas can't declare a namespace";
            if (av.find("FH2Mod_") != std::string_view::npos) bad = a + "=\"" + v + "\" names a platform property: bind N<i>, T<i> or B<i>";
            if (!bad.empty() || av.find('{') == std::string_view::npos) return;
            if (av.rfind("{Binding ", 0) == 0) {
                const char k    = av.size() > 9 ? av[9] : 0;
                std::size_t e   = 10;
                int n = 0, digs = 0;
                for (; e < av.size() && av[e] >= '0' && av[e] <= '9' && digs < 3; ++e, ++digs) n = n * 10 + (av[e] - '0');
                const int count = k == 'N' ? c.numbers : k == 'T' ? c.texts : k == 'B' ? c.switches : -1;
                const std::string_view rest = e <= av.size() ? av.substr(e) : std::string_view();
                const bool tail = rest == "}" || (k == 'B' && (rest == kConv[0] || rest == kConv[1]));
                if (count < 0 || !digs || !tail) {
                    bad = a + "=\"" + v + "\" isn't a plain {Binding N<i>}, {Binding T<i>} or {Binding B<i>} (a switch may have the "
                          "boolean-to-Visibility converter)";
                    return;
                }
                const std::string what = std::string(1, k) + std::to_string(n);
                if (n >= count) {
                    bad = "it binds " + what + ", but the canvas has " + std::to_string(count) + " of those (from " + std::string(1, k) + "0)";
                    return;
                }
                std::string kwhy;
                const char kind = bound_kind(t, an, av, kwhy);
                const char want = k == 'N' ? 'f' : k == 'T' ? 's' : 'b';
                if (!kind) bad = what + ": " + kwhy;
                else if (kind != want) bad = what + " is a " + hud_type_word(want) + ", bound where a " + hud_type_word(kind) + " goes";
                else ++bindings;
                return;
            }
            for (const char* kind : {"{StaticResource ", "{DynamicResource "}) {
                if (av.rfind(kind, 0) != 0) continue;
                const std::string_view key = av.substr(std::strlen(kind), av.size() - std::strlen(kind) - 1);
                if (av.back() != '}' || key.empty() || key.find_first_of("{}, ") != std::string_view::npos)
                    bad = a + "=\"" + v + "\" isn't a plain " + kind + "K}";
                else if (!known.count(std::string(key)))
                    bad = "the resource key " + std::string(key) + " isn't one every page of the game has (UI.zip's application dictionaries)";
                return;
            }
            if (av == "{x:Null}" || (av.rfind("{x:Type ", 0) == 0 && av.back() == '}' && av.find('{', 1) == std::string_view::npos)) return;
            bad = a + "=\"" + v + "\" uses a {...} a canvas can't: its own N, T and B bindings, StaticResource, DynamicResource, x:Type and "
                  "x:Null are what it can";
        });
        if (!bad.empty()) return why = bad, false;
    }
    if (bindings > kHudCanvasBindings)
        return why = std::to_string(bindings) + " bindings, more than the " + std::to_string(kHudCanvasBindings) + " a canvas may have", false;
    if (!styles_fit(std::vector<XTag>{}, tags, std::map<std::string, KeyDef>{}, known, why)) return false;
    // its names made the page's own, in attribute values only, and its lines indented as the page's are
    const std::string cv = "FH2Mod_Hud_Cv" + std::to_string(c.layer);
    std::string x        = c.xaml;
    for (const char q : {'"', '\''})
        for (const char k : {'N', 'T', 'B'}) {
            const std::string from = std::string(1, q) + "{Binding " + k, to = std::string(1, q) + "{Binding " + cv + k;
            for (std::size_t at = x.find(from); at != std::string::npos; at = x.find(from, at + to.size())) x.replace(at, from.size(), to);
        }
    block.clear();
    for (std::size_t at = 0; at < x.size();) {
        std::size_t e = x.find('\n', at);
        if (e == std::string::npos) e = x.size();
        std::string_view line(x.data() + at, e - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.find_first_not_of(" \t") != std::string_view::npos) block.append(24, ' ').append(line).append("\r\n");
        at = e + 1;
    }
    return true;
}

} // namespace

bool hud_check_markup(const std::string& res, const std::string& layer, uint32_t parts, const std::vector<HudProp>& props,
                      const HudUi& ui, HudCheck& out, std::string& why, std::size_t max_bindings) {
    out = HudCheck{};
    // 1: tags, each block on its own (the platform's own check, and the scanner's)
    std::string tag_why;
    if (!balanced(ui, res, tag_why)) return why = "the resources' tags: " + tag_why, false;
    if (!balanced(ui, layer, tag_why)) return why = "the layer's tags: " + tag_why, false;
    std::vector<XTag> rt, lt;
    if (!scan_xml(res, [&](const XTag& t) { rt.push_back(t); }, tag_why)) return why = "the resources' tags: " + tag_why, false;
    if (!scan_xml(layer, [&](const XTag& t) { lt.push_back(t); }, tag_why)) return why = "the layer's tags: " + tag_why, false;

    // 2: the application's keys (AnthemApp.xaml and its merged dictionaries, each at its own level)
    std::map<std::string, KeyDef> known;
    if (!app_keys(ui, known, out.dicts, why)) return false;
    out.known = known.size();
    // the platform's own: each top-level element of the resources block, keyed FH2HL_, once
    std::map<std::string, KeyDef> own;
    std::string open_key;
    for (const XTag& t : rt) {
        if (t.depth != 0) continue;
        if (t.close) {
            own[open_key].end = t.end;
            continue;
        }
        std::string_view key, target;
        if (!attr(t.text, "x:Key", key) || key.rfind("FH2HL_", 0) != 0)
            return why = "the resources block holds a <" + std::string(t.name) + "> without an FH2HL_ key", false;
        if (own.count(std::string(key)) || known.count(std::string(key)))
            return why = "the resource key " + std::string(key) + " is defined twice", false;
        KeyDef d;
        d.element = std::string(t.name);
        if (attr(t.text, "TargetType", target)) d.target = type_name(target);
        d.at     = t.at;
        d.end    = t.self ? t.end : 0;
        open_key = std::string(key);
        own.emplace(open_key, d);
    }
    // every key used is defined; a StaticResource to one of the platform's own only after its
    // definition; BasedOn only on a Style of the same target type
    std::set<std::string> used;
    for (const auto* tags : {&rt, &lt})
        for (const XTag& t : *tags) {
            if (t.close) continue;
            const bool in_res = tags == &rt;
            std::string bad;
            each_attr(t.text, [&](std::string_view an, std::string_view av) {
                each_resource_use(av, [&](std::string_view k, bool is_static) {
                    const std::string key(k);
                    used.insert(key);
                    if (!bad.empty()) return;
                    const auto o = own.find(key);
                    if (o == own.end() && !known.count(key)) {
                        bad = "the resource key " + key + " isn't in UI.zip's application dictionaries";
                        return;
                    }
                    if (o != own.end() && is_static && in_res && !(o->second.end && o->second.end <= t.at))
                        bad = "{StaticResource " + key + "} is used before " + key + " is defined";
                    if (!bad.empty() || an != "BasedOn" || t.name != "Style") return;
                    const KeyDef& base = o != own.end() ? o->second : known[key];
                    std::string_view tt;
                    const std::string mine = attr(t.text, "TargetType", tt) ? type_name(tt) : "";
                    if (base.element != "Style") bad = "a Style is BasedOn " + key + ", which isn't a Style";
                    else if (!mine.empty() && !base.target.empty() && mine != base.target)
                        bad = "a Style for " + mine + " is BasedOn " + key + ", a Style for " + base.target;
                });
            });
            if (!bad.empty()) return why = bad, false;
        }
    out.used = used.size();

    // 3: Styles on their own types
    if (!styles_fit(rt, lt, own, known, why)) return false;

    // 4: the root: the layer block is one Grid, FH2HL_Root, collapsed unless FH2Mod_Hud_Shown is true
    int tops = 0;
    for (const XTag& t : lt)
        if (t.depth == 0 && !t.close) {
            ++tops;
            std::string_view n, st, vis;
            if (t.name != "Grid" || !attr(t.text, "x:Name", n) || n != "FH2HL_Root" || !attr(t.text, "Style", st) ||
                st != "{StaticResource FH2HL_Root}" || attr(t.text, "Visibility", vis))
                return why = "the layer's top element isn't the FH2HL_Root Grid with its Style and no Visibility of its own", false;
        }
    if (tops != 1) return why = "the layer block holds " + std::to_string(tops) + " top elements, not one (FH2HL_Root)", false;
    const auto root = own.find("FH2HL_Root");
    if (root == own.end() || !root->second.end) return why = "the FH2HL_Root style isn't in the resources block", false;
    std::string root_style;   // its text, with the white space between tags taken out
    for (std::size_t i = root->second.at; i < root->second.end; ++i) {
        if (xml_space(res[i])) {
            std::size_t j = i;
            while (j < root->second.end && xml_space(res[j])) ++j;
            const bool between_tags = !root_style.empty() && root_style.back() == '>' && j < root->second.end && res[j] == '<';
            if (!between_tags) root_style += ' ';
            i = j - 1;
            continue;
        }
        root_style += res[i];
    }
    if (root_style != kRootStyle) return why = "the FH2HL_Root style isn't exactly collapsed unless FH2Mod_Hud_Shown is true", false;

    // 5: bindings, from real attribute values only, each of its property's kind; an int trigger only on
    // one of the property's values, and each of those values with its trigger; every property bound.
    // 6: no literal Text but the platform's "+", "<" and ">".
    std::map<std::string, int> index;
    for (int i = 0, n = static_cast<int>(props.size()); i < n; ++i) index[props[i].short_name] = i;
    std::set<std::string> bound;
    std::map<int, std::set<int>> trigger_values;
    for (const auto* tags : {&rt, &lt})
        for (const XTag& t : *tags) {
            if (t.close) continue;
            std::string bad;
            each_attr(t.text, [&](std::string_view an, std::string_view av) {
                if (!bad.empty()) return;
                if (an == "Text" && av.rfind("{Binding ", 0) != 0 && av != "+" && av != "&lt;" && av != "&gt;" && av != ":") {
                    bad = "Text=\"" + std::string(av) + "\" is a literal text, and only the platform's +, <, > and : may be";
                    return;
                }
                if (av.find("FH2Mod_Hud_") == std::string_view::npos) return;
                if (av.rfind("{Binding FH2Mod_Hud_", 0) != 0) {
                    bad = std::string(an) + "=\"" + std::string(av) + "\" names a HUD layer property without binding it plainly";
                    return;
                }
                std::size_t e = 20;
                while (e < av.size() && (std::isalnum(static_cast<unsigned char>(av[e])) || av[e] == '_')) ++e;
                const std::string n(av.substr(20, e - 20));
                if (e >= av.size() || (av[e] != '}' && av[e] != ',')) {
                    bad = "the binding " + std::string(av) + " isn't a plain {Binding FH2Mod_Hud_...}";
                    return;
                }
                const auto it = index.find(n);
                if (it == index.end()) {
                    bad = "the binding FH2Mod_Hud_" + n + " isn't one of this page's properties";
                    return;
                }
                std::string kwhy;
                const char kind = bound_kind(t, an, av, kwhy);
                if (!kind) {
                    bad = "FH2Mod_Hud_" + n + ": " + kwhy;
                    return;
                }
                const HudProp& p = props[static_cast<std::size_t>(it->second)];
                if (kind != p.type) {
                    bad = "FH2Mod_Hud_" + n + " is a " + hud_type_word(p.type) + ", bound where a " + hud_type_word(kind) + " goes";
                    return;
                }
                if (an == "Binding") ++out.triggers;
                if (kind == 'i' && an == "Binding") {
                    std::string_view v;
                    attr(t.text, "Value", v);
                    const int value = std::atoi(std::string(v).c_str());
                    if (std::find(p.values.begin(), p.values.end(), value) == p.values.end()) {
                        bad = "a trigger shows FH2Mod_Hud_" + n + " = " + std::string(v) + ", which isn't one of its values";
                        return;
                    }
                    trigger_values[it->second].insert(value);
                }
                bound.insert(n);
                ++out.bindings;
            });
            if (!bad.empty()) return why = bad, false;
        }
    for (int i = 0, n = static_cast<int>(props.size()); i < n; ++i) {
        const HudProp& p = props[static_cast<std::size_t>(i)];
        if (p.canvas) continue;   // a canvas's own: bound in the mod's XAML, if it uses it
        if (!bound.count(p.short_name)) return why = "the property " + p.name + " isn't bound anywhere", false;
        for (int v : p.values)
            if (!trigger_values[i].count(v)) return why = "no trigger shows " + p.name + " = " + std::to_string(v) + ", one of its values", false;
    }
    out.distinct = bound.size();

    // 7: the rings (ed:Arc) exactly when BAR or COUNTDOWN is declared
    bool arc = false;
    for (const auto* tags : {&rt, &lt})
        for (const XTag& t : *tags) arc = arc || (t.name == "ed:Arc" && !t.close);
    if (arc != ring_parts(parts))
        return why = arc ? "the markup has an ed:Arc, but its parts (" + hud_parts_text(parts) + ") have no ring"
                         : "the markup has no ed:Arc, but its parts (" + hud_parts_text(parts) + ") have rings",
               false;

    // 8: the binding budget: every binding costs every PropertyChanged on the page
    if (out.bindings > max_bindings)
        return why = std::to_string(out.bindings) + " bindings, more than the " + std::to_string(max_bindings) + " the layer may have", false;
    return true;
}

bool hud_splice(std::string& page, uint32_t parts, const std::vector<HudProp>& props, const HudUi& ui, HudCheck& out, std::string& why,
                const std::vector<HudCanvas>& canvases, std::vector<std::string>* canvas_why) {
    if (page.empty()) return why = "A_Hud.xaml couldn't be read from the game's UI.zip", false;
    // the anchors, in the page as it is now (after the notice's insertions)
    const std::size_t res_n = count_of(page, "</ResourceDictionary>"), zp_n = count_of(page, "</local:ZPanel>");
    const std::size_t z     = page.rfind("</local:ZPanel>");
    const std::size_t g     = z == std::string::npos ? z : page.rfind("</Grid>", z);
    if (res_n != 1 || zp_n != 1 || g == std::string::npos)
        return why = "A_Hud.xaml's insertion points aren't each there once (</ResourceDictionary> " + std::to_string(res_n) +
                     ", </local:ZPanel> " + std::to_string(zp_n) + ")",
               false;
    const std::string res = hud_resources_markup(parts), layer = hud_layer_markup(parts, canvases);
    if (!hud_check_markup(res, layer, parts, props, ui, out, why)) return false;
    // the canvases, each checked on its own: one that doesn't pass is left out, and the rest goes in
    std::vector<std::string> blocks(canvases.size());
    if (canvas_why) canvas_why->assign(canvases.size(), std::string());
    bool ed = false;
    if (!canvases.empty()) {
        std::map<std::string, KeyDef> known;
        int dicts = 0;
        std::string keys_why;
        const bool keys = app_keys(ui, known, dicts, keys_why);
        for (std::size_t i = 0; i < canvases.size(); ++i) {
            std::size_t n = 0;
            bool uses_ed  = false;
            std::string cw;
            if (!keys) cw = keys_why;
            else if (canvas_block(canvases[i], known, ui, blocks[i], n, uses_ed, cw)) out.bindings += n, ed = ed || uses_ed;
            if (!cw.empty()) blocks[i].clear();
            if (canvas_why) (*canvas_why)[i] = cw;
        }
    }
    const bool ring = ring_parts(parts) || ed;
    if (ring && count_of(page, kLocalNs) != 1)
        return why = "the Page tag's xmlns:local wasn't found once, so xmlns:ed (the rings) can't go in", false;
    // insert, from the bottom up: the layer (then each canvas at its placeholder), the resources, xmlns:ed
    std::string s = page;
    s.insert(line_start(s, g), layer);
    for (std::size_t i = 0; i < canvases.size(); ++i) {
        const std::size_t at = s.find(hud_canvas_placeholder(canvases[i].layer));
        if (at == std::string::npos || blocks[i].empty()) continue;
        const std::size_t ls = line_start(s, at), eol = s.find('\n', at);
        s.replace(ls, (eol == std::string::npos ? s.size() : eol + 1) - ls, blocks[i]);
    }
    s.insert(line_start(s, s.find("</ResourceDictionary>")), res);
    if (ring) {
        const std::size_t ns  = s.find(kLocalNs);
        const std::size_t eol = s.find('\n', ns);
        const std::size_t ls  = line_start(s, ns);
        std::size_t indent    = ls;
        while (indent < ns && s[indent] == ' ') ++indent;
        if (eol == std::string::npos) return why = "the xmlns:local line has no end", false;
        s.insert(eol + 1, s.substr(ls, indent - ls) + kEdNs + "\r\n");
    }
    std::string tag_why;
    if (!balanced(ui, s, tag_why)) return why = "the page's tags as inserted: " + tag_why, false;
    page = std::move(s);
    return true;
}

} // namespace fh2platform

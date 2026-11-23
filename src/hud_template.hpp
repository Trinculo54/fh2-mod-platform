// The HUD layer preview's template: the property table for a set of parts, the markup written into
// A_Hud.xaml, the check it passes before set_file, and the splice into the page. Pure: no game calls
// and no platform globals, so the tests build it without the game. UI.zip is read through a callback.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fh2platform {

// The parts, in FH2_HUD_PART_ bit order (fh2mod_hud.h): BAR, CHIPS, TIP, PANEL, COUNTDOWN, FEED, CANVAS.
constexpr int kHudParts = 7;
extern const char* const kHudPartNames[kHudParts];   // "BAR", "CHIPS", "TIP", "PANEL", "COUNTDOWN", "FEED", "CANVAS"
extern const char* const kHudPartProps[kHudParts];   // their fade properties: "Bar", "Chips", "Tip", "Panel", "Count", "Feed", "Canvas"
constexpr uint32_t kHudPartsKnown = 0x7Fu;           // every part bit, the feed's and the canvas's included
// The parts that take the screen for their layer: FH2_HUD_PART_ALL's five and the canvas (the feed is shared).
constexpr uint32_t kHudPartsScreen = 0x5Fu;
constexpr int kHudMaxLayers = 4;                     // FH2_HUD_MAX_LAYERS
// A canvas's limits (FH2_HUD_CANVAS_ in fh2mod_hud.h).
constexpr int kHudCanvasNumbers = 32, kHudCanvasTexts = 16, kHudCanvasSwitches = 16;
constexpr std::size_t kHudCanvasBindings = 64;

// A mod's canvas (add_hud_canvas): its XAML as the mod gave it, and how many of each kind of value it binds.
// Its properties are Cv<layer>On (the platform's: this canvas is the one showing), Cv<layer>N<i>, Cv<layer>T<i>
// and Cv<layer>B<i>; the mod's XAML names the last three as N<i>, T<i> and B<i>.
struct HudCanvas {
    int layer = 0;
    std::string xaml;
    int numbers = 0, texts = 0, switches = 0;
};
constexpr int kHudFeedLines = 5;                     // FH2_HUD_MAX_FEED
// Where the feed's bottom line sits: its foot this far above the bottom of the 1920 x 1080 page, its left at
// the HUD's 108 px margin, over the game's minimap.
constexpr int kHudFeedBottom = 360;
std::string hud_parts_text(uint32_t parts);          // "BAR, CHIPS", or "none"

// What a property is for, which says when the runtime publishes and tells it.
enum class HudRole : uint8_t {
    content,       // what a part shows
    kbd,           // a part's key caps switch (TabKbd, P<i>Kbd, TipKbd): told after the other content
    frame_value,   // PlayheadX, HoldAngle, HoldFill: may change every frame, told uncapped
    part_fade,     // <P>: the part's fade
    part_vis,      // <P>Vis: the part's Visibility
    root,          // Shown
    shutter,       // Shutter
    count_pop,     // CountPop
};

struct HudProp {
    std::string name;          // "FH2Mod_Hud_Shown" (a descriptor keeps a pointer to it: the table is never resized)
    std::string short_name;    // "Shown"
    char type = 'b';           // b bool, i int, f float, s String
    std::string def;           // a String's default (UTF-8); the others default to false, 0 and 0.0
    std::vector<int> values;   // an int's values, each with its DataTrigger in the page
    int part = -1;             // the part whose elements bind it (0 BAR ... 6 CANVAS), -1 for Shown and Shutter
    HudRole role = HudRole::content;
    bool canvas = false;       // a canvas's number, text or switch: bound in the mod's own XAML, if at all
};

// The table for the declared parts (FH2_HUD_PART_ bits), in template order: 316 properties with the five
// parts before the feed, 338 with the feed; then, with the canvas part, its two and each canvas's own.
std::vector<HudProp> hud_build_props(uint32_t parts, const std::vector<HudCanvas>& canvases = {});
// fh2-mods\platform-hudlayer-props.txt: "name<TAB>type<TAB>default<TAB>values", one a line, \r\n.
std::string hud_props_text(const std::vector<HudProp>& props);
const char* hud_type_word(char type);   // "bool", "int", "float", "String"

// The two blocks spliced into A_Hud.xaml, indented for the page, \r\n lines.
std::string hud_resources_markup(uint32_t parts);
std::string hud_layer_markup(uint32_t parts, const std::vector<HudCanvas>& canvases = {});
// The line in the layer's markup that stands for a canvas until hud_splice puts its checked XAML there.
std::string hud_canvas_placeholder(int layer);

struct HudUi {
    std::function<std::string(const char*)> read;                     // a file of UI.zip by its path, "" if unreadable
    std::function<bool(const std::string&, std::string&)> balanced;   // the platform's balanced_xml, or empty
};

struct HudCheck {
    std::size_t used = 0;       // distinct resource keys used
    std::size_t known = 0;      // keys in UI.zip's application dictionaries
    std::size_t distinct = 0;   // properties bound
    std::size_t bindings = 0;   // {Binding FH2Mod_Hud_...} attribute values
    std::size_t triggers = 0;   // DataTriggers on them
    int dicts = 0;              // merged dictionaries read
};

constexpr std::size_t kHudMaxBindings = 640;

// The check before set_file on the two blocks (tags, keys, Styles, the root, bindings, literal texts, the
// rings and the budget); false with the first problem.
bool hud_check_markup(const std::string& res, const std::string& layer, uint32_t parts, const std::vector<HudProp>& props,
                      const HudUi& ui, HudCheck& out, std::string& why, std::size_t max_bindings = kHudMaxBindings);

// Builds the blocks, checks them and inserts them into `page` (A_Hud.xaml as it is after the notice's
// insertions): the resources at the start of the line holding the one </ResourceDictionary>, the layer at
// the start of the line holding the last </Grid> before the one </local:ZPanel>, and xmlns:ed after the
// xmlns:local line when BAR or COUNTDOWN is declared. False, with the first problem and `page` untouched,
// if anything doesn't fit (item 9 included).
// Each canvas is checked on its own (FH2HudCanvas in fh2mod_hud.h says what passes): one that doesn't is left out
// of the page, with the reason in `canvas_why` at its place ("" for one that's in), and the rest goes in.
bool hud_splice(std::string& page, uint32_t parts, const std::vector<HudProp>& props, const HudUi& ui, HudCheck& out,
                std::string& why, const std::vector<HudCanvas>& canvases = {}, std::vector<std::string>* canvas_why = nullptr);

} // namespace fh2platform

// The UI templates' rules for message boxes, UI sounds and the help bar and hint line of mods' pages (platform
// 2.9.0), as plain code with no game, no thread of its own and no clock: what's accepted, what's cut and how, which
// sound plays when. The glue is in settings.cpp (boxes, help, hints) and ui_sounds.cpp (sounds); the tests
// (tests/ui_rules_test.cpp) drive these with a fake clock.
//
// The rules (UI-TEMPLATES-290 1.1, 2.8 and 2.10, with S4's and S6's findings):
//  - Message boxes: a title of 30 characters (shown in capitals), a message of 200, 1 to 99 choices of 32 each, to
//    which the platform adds Back; longer text is cut with an ellipsis. Over 10 choices, 8 a page, with Previous and
//    Next, as the title screen's lists are; Back is always the last item.
//  - Sounds: FH2_SOUND_SELECT to _SHUTTER (1 to 11). The same sound at most once per 100 ms from a mod. NOTIFY and
//    the three countdown beeps sound only over the driving view, so they're refused from a page or box callback
//    (a menu is up) and while the free-roam HUD page is known to be off screen. From a callback a sound plays at
//    once; from anywhere else it waits for the game's next tick, four at most per mod (a fifth drops the oldest),
//    one played a tick, oldest first, and it's dropped if it waited 0.5 s.
//  - Page help: on a page of rows, A (only when the page has on_select) and B; on a page of the mod's own design,
//    A, B, X, Y, LB and RB when it has on_button, else only B. A label of 24 characters, or one of the game's words.
//    The last prompt for a button wins.
//  - Hint lines: up to 8 texts of 80 characters a page.
#pragma once

#include "fh2mod.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fh2platform {

// `s` cut to `chars` characters (text_chars), ending in "…" when it was longer.
std::string cut_ellipsis(const std::string& s, std::size_t chars);

// ---- message boxes -----------------------------------------------------------------------------------------------

constexpr std::size_t kBoxTitleChars   = 30;
constexpr std::size_t kBoxMessageChars = 200;
constexpr std::size_t kBoxChoiceChars  = 32;
constexpr std::size_t kBoxMaxChoices   = FH2_TITLE_MAX_CHOICES - 1;   // the platform adds Back
constexpr std::size_t kBoxFits         = 10;   // choices one box shows, with Back
constexpr std::size_t kBoxPageSize     = 8;    // choices a page of a longer list shows, with Previous, Next and Back
// What an item of a box is, besides a choice (from 0).
constexpr int kBoxItemBack = -1, kBoxItemPrevious = -2, kBoxItemNext = -3;

// A message box as a mod gave it (only `size` bytes of it read), copied and cut to the limits.
struct BoxSpec {
    std::string key, title, message;   // the title as given (the glue makes the capitals)
    std::vector<std::string> choices;
    uint32_t focus = 0;
    FH2BoxChooseFn on_choose = nullptr;
    void* user = nullptr;
    std::vector<std::string> cut;      // what was cut, for the log ("the title", "choice 2")
};
// False, with `why` for the log, when it can't be shown: no struct or too small, no title, no choices or too many,
// a choice with no text.
bool read_box(const FH2MessageBox* box, BoxSpec& out, std::string& why);
// How many pages a box of `n` choices takes (1 when they fit), the page holding choice `choice`, and what page `p`'s
// items are, in order: Previous (after the first page), its choices, Next (before the last page), then Back.
int box_pages(std::size_t n);
int box_page_of(std::size_t n, uint32_t choice);
std::vector<int> box_page_items(std::size_t n, int p);

// ---- UI sounds ---------------------------------------------------------------------------------------------------

struct UiSoundInfo {
    const char* name;      // for the log: "SELECT"
    const char* text;      // the game's FMOD event
    uintptr_t slot;        // the game's qword pointing at that text (game+RVA), 0 for the four the platform has
    uintptr_t text_rva;    // where it points    always played by name
    bool world_only;       // silent under the game's menus (S4)
};
// Indexed by FH2_SOUND_ (0 is none).
extern const UiSoundInfo kUiSounds[FH2_SOUND_SHUTTER + 1];
inline bool ui_sound_known(uint32_t sound) { return sound >= FH2_SOUND_SELECT && sound <= FH2_SOUND_SHUTTER; }

class UiSoundRules {
public:
    static constexpr double kRepeatMs = 100, kLateMs = 500;
    static constexpr std::size_t kPerMod = 4;
    enum class Ask { play_now, queued, unknown, repeat, world_only };
    // A mod asks for `sound` at `now` (ms). `in_callback`: from one of its page, setting, title-screen or box
    // callbacks, on the game's UI thread, where a menu or the title screen is up; `on_screen`: the free-roam HUD page
    // is on screen (1), isn't (0), or the platform doesn't know (-1).
    Ask ask(const void* mod, uint32_t sound, double now, bool in_callback, int on_screen);
    struct Due {
        const void* mod = nullptr;
        uint32_t sound  = 0;
    };
    enum class Drop { late, off_screen };
    struct Dropped {
        const void* mod;
        uint32_t sound;
        Drop why;
    };
    // The game's tick: the oldest waiting sound to play now (false if none), and what was dropped on the way.
    bool tick(double now, int on_screen, Due& out, std::vector<Dropped>& dropped);
    std::size_t waiting() const;
    // The mod was stopped for online play: its waiting sounds go. How many.
    std::size_t forget(const void* mod);

private:
    struct Waiting {
        const void* mod;
        uint32_t sound;
        double at;
    };
    mutable std::mutex mu_;
    std::map<std::pair<const void*, uint32_t>, double> last_;
    std::deque<Waiting> queue_;
};

// ---- page help and hint lines ------------------------------------------------------------------------------------

constexpr std::size_t kPromptLabelChars = 24;
constexpr std::size_t kMaxHints         = 8;
constexpr std::size_t kHintChars        = 80;

// One prompt on a page's help bar, accepted: its button (FH2_GLYPH_A to _RB), and the game's word or a label.
struct HelpPrompt {
    uint32_t glyph = 0;
    uint32_t word  = 0;      // FH2_WORD_, or 0 for the label
    std::string label;
};
struct HelpPage {
    bool custom    = false;  // a page of the mod's own design
    bool on_select = false;
    bool on_button = false;
};
// The prompts a page accepts, one per button (the last given wins), in the game's order; `refused` gets a line for
// each one that wasn't, and `notes` one for anything ignored (vk, hotkey and hold mean nothing on pages).
std::vector<HelpPrompt> accept_prompts(const FH2Prompt* prompts, uint32_t count, const HelpPage& page, std::vector<std::string>& refused,
                                       std::vector<std::string>& notes);
// "A", "LB": a glyph's name for the log.
const char* glyph_name(uint32_t glyph);

// A page's hint texts as given: up to kMaxHints, each cut to kHintChars; `notes` says what was left out or cut.
std::vector<std::string> read_hints(const char* const* texts, uint32_t count, std::vector<std::string>& notes);

} // namespace fh2platform

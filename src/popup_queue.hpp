// The pop-up queue (platform 2.9.0): every rule of the UI templates' pop-ups and tips (show_toast, clear_toast,
// show_tip, tip_seen, set_tip_seen and what ui_status says about them), as plain code with no game, no thread and
// no clock of its own. popups.cpp is the glue: it gives the queue the time and the game's state at each tick of the
// game's thread, shows what the queue picks through the game's own notification, and tells it what happened. The
// tests (tests/ui_queue_test.cpp) drive it with a fake clock and a fake seen store.
//
// The rules (UI-TEMPLATES-290 2.1 and 2.7, with S2's findings):
//  - One queue for every mod. URGENT first, then REPLY, then the rest in the order they came. A pop-up starts only
//    when no Show has been seen for 5.0 s (the game's 4 s and its fade), or, without the watch that sees the game's
//    own, 5.5 s after the platform's last.
//  - A waiting pop-up with the same mod and key is replaced where it stands; one on screen can't be, so the new one
//    waits its turn, and isn't queued at all if its text is the same. A mod's 5th waiting pop-up drops its oldest
//    waiting one that isn't URGENT.
//  - REPLY counts only within 1 s of the platform calling one of that mod's callbacks, and waits at most 6 s of time
//    it could have shown. URGENT counts twice a session per mod, and expires after 10 minutes of such time. Other
//    pop-ups expire after expire_s (default 120 s) of it: the HUD page on screen and the game's HUD not suspended.
//  - Tips are pop-ups with the mod's name as the title, after delay_s of driving with nothing else showing, never in
//    the first 12 s the HUD is on screen (the platform's own notice then), at least 3 minutes after the last tip,
//    one per mod and three in all a session, behind every pop-up. Seen only when the game is known to have drawn it.
//  - Logging: one line per pop-up queued, shown or dropped, in the mod's name, with the text unless it's PRIVATE;
//    lines about replacing, dropping and queuing at most once per 10 s per mod and key, with a count.
// Every public call is thread-safe (one lock) and quick; the env's callbacks are called under that lock, so they
// mustn't call back into the queue.
#pragma once

#include "fh2mod.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fh2platform {

// Who's calling.
struct PopupMod {
    const void* id = nullptr;   // the FH2Mod
    std::string name;           // its name: the log, and a tip's title
    std::string dll;            // its DLL's file name: the seen store's "who" ("" = no seen state)
    uint32_t uses = 0;          // what it said it uses (FH2_UI_USES_)
};

// What the queue needs from the platform. Every member may be empty in a test that doesn't need it.
struct PopupEnv {
    std::function<void(const std::string& who, const std::string& line)> log;   // who: a mod's name, or "Pop-ups"
    std::function<std::string(const std::string&)> upper;                       // capitals in the game's language
    std::function<std::string(const char* english)> tr;                         // the platform's own words
    std::function<std::string(const std::string& who, const std::string& key)> seen_get;
    std::function<bool(const std::string& who, const std::string& key, const std::string& value)> seen_set;
    std::function<bool(const std::string& who, const std::string& key)> seen_clear;
    std::function<int()> seen_clear_tips;   // every mod's tips: how many were forgotten
    // A mod's hotkey (API 9) by its key: its binding now. False if the mod has no such hotkey.
    std::function<bool(const void* mod, const std::string& hotkey, uint32_t* vk, uint32_t* modifiers, uint32_t* chord)> hotkey;
    // The name of a key that isn't in the queue's own table, on the player's keyboard layout ("" if none).
    std::function<std::string(uint32_t vk)> key_name;
    std::function<std::string()> clock_text;   // the time of day for "Recent pop-ups" ("14:05")
};

// The game's state at one tick of its thread.
struct PopupTick {
    double now     = 0;      // ms, any origin, never going back
    bool on_screen = false;  // the free-roam HUD page is on screen
    bool suspended = false;  // the game has suspended its HUD (and the platform doesn't hold it)
    int option     = -1;     // the game's Notifications option: 1 on, 0 off, -1 not known (yet, or can't be read)
    bool keyboard  = false;  // the keyboard or mouse was used last
    bool ready     = false;  // Show can be called now (a notification component is there to show it on)
    bool wide      = false;  // the text goes to the game as UTF-16; otherwise it's folded to ASCII
};

// What tick() says to show now (id 0: nothing).
struct PopupShow {
    uint64_t id = 0;
    bool tip    = false;
    std::string mod;            // the mod's name
    std::string title, line;    // UTF-8, as the game is to get them: capitals, folded unless wide, cut
    std::string icon;           // the game's IconName ("" for none)
    uint32_t sound = 0;         // FH2_SOUND_ to play as it appears (never NOTIFY: the game chimes by itself)
};

// What became of a Show (outcome()).
enum class PopupOutcome {
    shown,     // the watch saw the game make it
    refused,   // the watch saw the game's gate turn it down (its Notifications option is off)
    failed,    // it couldn't be shown (no component, or the game's code faulted)
    unknown,   // Show was called, but with no watch nobody can tell
};

struct PopupRecent {
    std::string mod, when, title;   // title: "(private)" for a PRIVATE pop-up
    bool tip = false;
    bool priv = false;              // a PRIVATE pop-up
};

class PopupQueue {
public:
    explicit PopupQueue(PopupEnv env = {});

    // ---- the API's calls (any thread) ----
    int show_toast(const PopupMod& m, const FH2Toast* toast, double now);
    void clear_toast(const PopupMod& m, const char* key);
    int show_tip(const PopupMod& m, const FH2Tip* tip, double now);
    int tip_seen(const PopupMod& m, const char* key);
    void set_tip_seen(const PopupMod& m, const char* key, int seen);
    // ui_status's bits: FH2_UI_TOASTS, _TOASTS_OFF, _TIPS_OFF, _ON_SCREEN and _GAME_HUD_HIDDEN from the queue's
    // state, _KEYBOARD from `keyboard`.
    uint32_t status(const void* mod, bool keyboard) const;

    // ---- the platform ----
    // The platform is about to call one of the mod's own callbacks (a hotkey, a command, a page, setting, title or
    // message box callback): a REPLY within 1 s of it counts.
    void note_callback(const void* mod, double now);
    // The platform caught a crash in one of the mod's callbacks: its waiting pop-ups and tip go.
    void faulted(const PopupMod& m);
    // Whether pop-ups can show this session: -1 not known yet (they're queued meanwhile), 1 yes, 0 no (`why`, a
    // sentence: everything waiting is dropped, and later calls return 0).
    void set_available(int state, const std::string& why = "");
    int available() const;
    // Whether the watch on the game's Show is in (pop-ups 5.0 s after any Show), or not (5.5 s after the platform's).
    void set_watch(bool on);
    // The player's rows (the settings restructure's): "Mod pop-ups", a mod's "Pop-ups", "Tips". On by default.
    void set_platform_toasts(bool on);
    void set_mod_toasts(const void* mod, bool on);
    void set_tips(bool on);
    // "Show tips again": every mod's tips are unseen. Returns how many were forgotten.
    int tips_again();

    // One tick of the game's thread: time counted, pop-ups expired, and what to show now, if anything.
    PopupShow tick(const PopupTick& in);
    // What became of the Show tick() asked for.
    void outcome(uint64_t id, PopupOutcome what, double now);
    // The watch saw a Show the game let through (anyone's: the game's, another mod's or the platform's own).
    void show_seen(double now);

    std::vector<PopupRecent> recent() const;   // the last 10 shown, newest first
    std::string describe(double now) const;     // what's waiting and why, for a command or a crash report
    std::size_t waiting() const;                // pop-ups and tips waiting

private:
    struct Item {
        uint64_t id = 0, seq = 0;
        bool tip = false;
        const void* mod = nullptr;
        std::string mod_name, dll, key;
        std::string title, line;   // a pop-up's as given; a tip's text in `line`
        std::string shown_title, shown_line;   // as they went to the game (tick)
        uint32_t icon = 0, sound = 0, flags = 0;
        double expire_ms = 0, waited = 0, queued_at = 0;
        uint32_t held = 0;         // what it waited for (kHeld bits)
        // tips
        FH2Prompt button{};
        std::string hotkey;        // button.hotkey, copied
        bool has_button = false;
        double delay_ms = 0, drive = 0;
        uint32_t max_sessions = 1, tip_flags = 0;
    };
    struct Rated {
        double since = -1e18;
        int more     = 0;
        std::string who, what, key;
    };
    struct OnScreen {
        const void* mod;
        std::string mod_name, key, title, line;   // title and line only for the same-text check, never printed if private
        uint32_t icon;
        bool priv;
        double at;
    };

    void log(const std::string& who, const std::string& line) const;
    void log_once(const PopupMod& m, const std::string& what, const std::string& line);
    // A line at most once per 10 s per (who, what, key), the ones held back counted into the next.
    void log_rated(const std::string& who, const std::string& what, const std::string& key, const std::string& line, double now);
    void flush_rated(double now, bool all);
    std::string text_of(const Item& it) const;   // "TITLE / line (key K)", or "key K" when private
    std::string tip_name(const Item& it) const;  // "tip FirstTree" or "a tip"
    void drop(std::size_t i, const std::string& why, double now, bool rated);
    bool lane_free(double now) const;
    std::string prepare(const std::string& s, std::size_t limit, bool capitals, bool wide);
    // The button's text; `wide`: the text goes to the game as UTF-16, so words needn't fold to ASCII.
    std::string button_text(const Item& it, bool keyboard, bool wide);
    std::string key_text(uint32_t vk, uint32_t modifiers, bool wide);
    std::string glyph_text(uint32_t glyph, bool wide);
    std::string chord_text(uint32_t chord, bool wide);
    std::string tr(const char* english);    // the platform's own words (lang.cpp's table has each)
    std::string caps(const std::string& s); // capitals in the game's language
    // A word of the platform's for a button, in capitals in the game's language; in English when the text is
    // folded to ASCII and the word has letters with no plain form (Russian, Japanese, Korean, Chinese), which would
    // be question marks.
    std::string word(int which, bool wide);   // which: popup_queue.cpp's kWords
    std::string held_text(uint32_t held) const;
    void note_shown(const Item& it, double now, bool private_title);

    PopupEnv env_;
    mutable std::mutex mu_;
    std::vector<Item> items_;   // waiting, in the order they came
    Item inflight_;             // tick() picked it; outcome() settles it (id 0: none)
    uint64_t next_id_ = 1, next_seq_ = 1;
    int available_ = -1;
    std::string why_off_;
    bool watch_ = false, platform_toasts_ = true, tips_ = true;
    std::map<const void*, bool> mod_off_;
    int option_     = -1;
    bool on_screen_ = false, suspended_ = false;
    double last_tick_ = -1, first_screen_ = -1;
    double last_show_ = -1e18, last_own_show_ = -1e18, last_tip_ = -1e18;
    std::map<const void*, double> last_callback_;
    std::map<const void*, int> urgent_used_;
    std::map<const void*, bool> tipped_;   // shown a tip this session
    int tips_shown_ = 0;
    std::map<std::string, Rated> rated_;
    std::map<std::string, bool> once_;
    std::vector<OnScreen> on_screen_items_;
    std::vector<PopupRecent> recent_;
};

// ---- text, for the queue and its tests ----

// UTF-8 `s` folded to ASCII for the game's narrow display strings: letters with marks to their base letter (ą to a,
// ł to l, ß to ss), typographic quotes, dashes, the ellipsis and the no-break spaces to their plain forms, anything
// else to '?'. *changed (may be null) says whether anything was folded.
std::string popup_fold_ascii(const std::string& s, bool* changed = nullptr);

// "Recent pop-ups" as the player sees it (the page on the platform's own page under YOUR MODS, and "fh2mod Platform
// ui recent"): a row per pop-up or tip, newest first, labelled with its title as it was shown (a tip's line, and
// "(private)" in the player's language for a private one), the time on the right, and which mod sent it in the
// description; or a single row saying there are none yet. `tr` gives the platform's own words (lang.cpp's tr).
struct RecentRow {
    std::string label, value, description;
};
std::vector<RecentRow> recent_page(const std::vector<PopupRecent>& recent, const std::function<std::string(const char*)>& tr);

} // namespace fh2platform

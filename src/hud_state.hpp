// The HUD layer preview's state (hud_state.cpp): the layers' staged state, text and key caps, the screen
// (owner, parts, fades, the root, CLEAN, pulses), targets, publishing and what the page was told, the
// per-frame budget and rate limits, the String cache, leases, status bits, sounds, and the device judge.
// Pure: what touches the game goes through HudGame, and time through a clock, so the tests drive it
// frame by frame with fakes.
#pragma once

#include "fh2mod_hud.h"
#include "hud_template.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fh2platform {

// What the runtime needs from the game (hud_layer.cpp for the real one, fakes in the tests).
struct HudGame {
    virtual ~HudGame() = default;
    // A new String of text[0, len) holding one reference, the platform's. False if the game's code
    // faulted; true with *out null if the game gave no memory.
    virtual bool make_string(const wchar_t* text, int len, void** out) = 0;
    virtual void add_ref(void* s) = 0;                 // one more reference (InterlockedIncrement at +0x10)
    virtual bool release(void* s, bool* freed) = 0;    // one reference given back, freed at 0; false if it faulted
    virtual int refs(void* s) = 0;                     // its count at +0x10 (for the stats only)
    virtual void pin(void* s) = 0;                     // never freed: its count raised by 1 << 20 (the shared empty String)
    virtual bool notify(int prop) = 0;                 // PropertyChanged on the live page; false if it faulted
};

struct HudEnv {
    HudGame* game = nullptr;
    std::function<double()> clock;                                           // ms, any thread
    std::function<void(const std::string& who, const std::string& text)> log;   // who: a mod's name, or "HUD layer"
    // A layer's on_status(user, bits), guarded: false (with the exception code) if it crashed.
    std::function<bool(FH2HudStatusFn fn, void* user, uint32_t bits, uint32_t* code)> call_status;
};

// ---- text and key caps ------------------------------------------------------------------------------

enum : unsigned {
    kHudTextControl = 1,   // a control character became a space
    kHudTextCut     = 2,   // longer than its limit: cut
    kHudTextOdd     = 4,   // letters outside the set seen working in the HUD's fonts (*odd: the first)
};
// UTF-8 to what the HUD shows: control characters (U+0000-001F, 007F-009F, 2028, 2029) as spaces, cut to
// `limit` UTF-16 units (never between the halves of a surrogate pair), in capitals if `caps`.
std::wstring hud_text(const char* utf8, std::size_t limit, bool caps, unsigned* flags, uint32_t* odd);
bool hud_letter_ok(uint32_t c);   // in the set seen working in the HUD's fonts

// A key as one or two caps: split at the first + or / with text on both sides; Up, Down, Left and Right
// (ASCII, any case) as arrows; the other names in capitals, each cut to 12.
struct HudKeyCaps {
    bool given = false;
    bool two   = false;              // a second cap after `join`
    std::wstring k0, join, k1;
    int angle0 = -1, angle1 = -1;    // an arrow's angle (Right 0, Down 90, Left 180, Up 270), -1 for text
};
HudKeyCaps hud_key_caps(const std::wstring& key, unsigned* flags);

// ---- the device: the judge, kept apart from the reads so the tests can drive it ---------------------

struct HudPad {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
// A pad's new input between two reads ("buttons", "trigger", "left stick", "right stick"), or nullptr: a
// button pressed or let go, a trigger crossing 30 or moving 8 past it, a stick crossing 7849 or moving
// 1024 past it. Holding still isn't new input.
const char* hud_pad_input(const HudPad& now, const HudPad& was);

// The cursor against where it was the round before, counted only when it moved in two rounds running
// and 3 px or more over the two: a single jump (the window placing the pointer) isn't the mouse.
struct HudMouseMoves {
    long x = 0, y = 0;
    bool have = false;
    int run   = 0;
    long last = 0;
    bool round(bool read, long nx, long ny);
    void forget();
};

// The keyboard: a key going down counts, apart from the modifiers, the Windows keys, any key while a
// Windows key is down, and the hotkeys' own keys. `down` is the 256 keys' state this round.
struct HudKeyWatch {
    bool was[256] = {};
    // The first key that went down and counts (its text "key 0x41" in buf), or nullptr. Every key's state
    // is taken either way.
    const char* round(const bool* down, const uint32_t* hotkeys, std::size_t n_hotkeys, char* buf, std::size_t cap);
};

// The newest input wins: a key or the mouse picks key caps, a pad picks pad pictures, both kinds in one
// round change nothing, and switches come at most every 150 ms (a later input waits, it isn't dropped).
struct HudDeviceJudge {
    bool kbd    = false;
    int pending = -1;
    char cause[48] = "";
    double flipped_at = -1e9;
    bool round(const char* kb, const char* pad, double now);   // true when the device changes
};

// ---- the runtime ------------------------------------------------------------------------------------

// Why the layer is off for the session, the first reason kept.
enum class HudOff : uint8_t {
    none,
    left_out,         // the crash marker, the page check, the page not written, code or hooks (logged by the glue)
    notify_fault,     // L33: nothing is told again
    strings_fault,    // L35: nothing more is released; the getter gives no Strings
    bool_box_fault,   // L34 for a bool: no last hide
    value_box_fault,  // L34 for an int, float or String: the last hide
    interlock,        // L25
    page_unread,      // L23
    descriptors,      // L19
};

class HudEngine {
public:
    explicit HudEngine(HudEnv env);
    ~HudEngine();
    HudEngine(const HudEngine&) = delete;
    HudEngine& operator=(const HudEngine&) = delete;

    // ---- the interface's calls: any thread, `layer` from layer_of -----------------------------------
    // fh2mod_init's thread: checks and records a layer (logged with the mod's name); its index, or -1.
    int add_layer(void* mod, const std::string& mod_name, const FH2HudLayer* layer);
    int layer_of(const void* mod) const;   // -1: no layer
    int layer_count() const;
    uint32_t layer_parts(int layer) const;
    uint32_t layer_lease(int layer) const;
    const std::string& layer_name(int layer) const;
    int set_state(int layer, const FH2HudState* state);
    int set_value(int layer, int which, float value);
    int show(int layer, uint32_t parts);
    int pulse(int layer, int pulse);
    uint32_t status(int layer);
    int hold(int layer, int hold);
    int play_sound(int layer, uint32_t sound);
    int post_feed(int layer, const FH2HudFeedLine* line);   // interface version 2
    // The canvas (interface version 3). set_canvas is fh2mod_init's thread: the layer's canvas and how many of
    // each kind of value it has; false if the layer has one already. The other three are any thread.
    bool set_canvas(int layer, int numbers, int texts, int switches);
    bool has_canvas(int layer) const;
    int canvas_numbers(int layer, const float* numbers, uint32_t count);
    int canvas_text(int layer, uint32_t which, const char* utf8);
    int canvas_switches(int layer, uint32_t bits);
    // Its mod was stopped for online play (any thread): from the next frame its layer is hidden and its hold let
    // go, as when its lease runs out, whatever it calls (its hud_status still keeps a lease). The glue refuses its
    // shows with parts and its holds from then on.
    void stop_layer(int layer);

    // ---- the session (the loader thread, after every fh2mod_init) -----------------------------------
    uint32_t parts() const;                              // the union of the layers' parts
    void build(const std::vector<HudProp>& props);       // the table this session's page binds
    const std::vector<HudProp>& props() const;
    int prop_index(const std::string& short_name) const; // -1 if the page doesn't have it

    // ---- what's off ---------------------------------------------------------------------------------
    void set_off(HudOff why);                            // any thread; the first reason is kept
    bool off() const;
    HudOff off_reason() const;
    void set_hold_available(bool ok);                    // G5 (default: available until known)
    void set_sounds_available(bool ok);                  // G6, or a sound that crashed

    // ---- the pump (the game's UI thread, once a frame) ----------------------------------------------
    struct FrameIn {
        double now = 0;            // ms
        uint64_t frame = 0;
        bool live = false;         // notifies may go to the page
        const void* page = nullptr;   // the live page: a different one gets a full refresh, hides first
        uint64_t page_gen = 0;        // bumped for each page tracked, so a new page at an old page's address is new too
        bool kbd = false;          // the prompts show key caps
        bool device_forced = false;
        bool available = false;    // the glue's part of AVAILABLE (ready, and the page read if one was shown)
        bool game_hidden = false;  // the platform's hold is in force
    };
    struct FrameOut {
        int sound = -1;            // FH2_SOUND_ to play now, or -1
        int sound_layer = -1;
        uint32_t notified = 0;
    };
    FrameOut frame(const FrameIn& in);
    bool hold_wanted() const;      // some mod's hold wish counts (not lapsed, not removed, the layer not off)
    std::string holders() const;   // the mods whose wish counts, comma-separated
    // The pump's view for the crash text and the stats.
    int owner() const;             // the layer on screen, -1 none
    uint32_t shown_parts() const;  // the parts IN or going out
    uint64_t last_notify_frame() const;
    bool clean() const;
    struct Stats {
        uint64_t frames = 0, busy_frames = 0, notifies = 0, max_notifies = 0, carried = 0, skipped = 0;
    };
    Stats take_stats();            // since the last call
    std::size_t strings_in_use() const;
    std::string strings_line() const;   // L41 without the "[HUD layer] " (reads the counts through HudGame::refs)
    // A line of the mod's own, at most 20 per 10 s for each kind (0 sounds, 1 shows, 2 holds), then a count.
    void log_rated(int layer, int kind, const std::string& text);
    bool once(int layer, const std::string& key);   // true the first time for this layer and key (-1: the platform's)
    static const char* sound_name(uint32_t sound);  // "ERROR" ... "SHUTTER", "" for others

    // ---- the getter (any thread that reads the page) ------------------------------------------------
    uint32_t value(int prop, bool gated) const;   // a bool's, int's or float's bits: published, or the default
    void* string(int prop, bool gated);           // a String with a reference added for the caller; nullptr after a String fault

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace fh2platform

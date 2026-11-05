// The pad layer preview's rules, with no game or Windows in them, so tests can run them as they are
// (pad_layer.cpp is the game side).
//
// The game keeps each controller's state as a mask of control numbers. 0 to 15 are XInput's bits,
// so FH2_PAD_ bits, apart from 10 and 11, which are the triggers. When two d-pad directions are
// held, the game drops those two bits and sets one of four diagonal controls (33 to 36) instead.
// Each controller also has a mask of controls the game has taken: while a control's bit is set
// there, the game's actions see it as not pressed, and the game clears the bit itself once the
// control is let go. The pad layer sets bits in that mask while a layer's modifier is held.
#pragma once

#include <cstdint>

#include "fh2mod.h"

namespace fh2platform::pad {

constexpr int kMaxLayers = 16;
constexpr int kSlots     = 9;   // the controller manager's slots

constexpr uint32_t kDpad = FH2_PAD_DPAD_UP | FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT | FH2_PAD_DPAD_RIGHT;
// The FH2_PAD_ bits a layer can use: XInput's, without its two unused ones.
constexpr uint32_t kPadBits = 0xF3FFu;

// The game's diagonal d-pad controls and the two directions each stands for.
struct Diagonal {
    int control;
    uint32_t pad;
};
constexpr Diagonal kDiagonals[4] = {
    {33, FH2_PAD_DPAD_UP | FH2_PAD_DPAD_LEFT},
    {34, FH2_PAD_DPAD_UP | FH2_PAD_DPAD_RIGHT},
    {35, FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_LEFT},
    {36, FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_RIGHT},
};

// The FH2_PAD_ bits held, from the game's control mask.
inline uint32_t pad_from_controls(uint64_t controls) {
    uint32_t pad = static_cast<uint32_t>(controls) & kPadBits;
    for (const Diagonal& d : kDiagonals)
        if (controls & (1ull << d.control)) pad |= d.pad;
    return pad;
}

// The game controls that stand for some of `pad`: its own bits, and every diagonal one of them is part of.
inline uint64_t controls_for(uint32_t pad) {
    uint64_t c = pad & kPadBits;
    for (const Diagonal& d : kDiagonals)
        if (pad & d.pad) c |= 1ull << d.control;
    return c;
}

inline int bit_count(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// Two layers clash when one modifier holds the other's (so both would be open at once) and they take
// a button in common. Different modifiers that can't both be held, like LB and RB, never clash.
inline bool clash(uint32_t mod_a, uint32_t buttons_a, uint32_t mod_b, uint32_t buttons_b) {
    if (!mod_a || !mod_b) return false;
    const bool nested = (mod_a & mod_b) == mod_a || (mod_a & mod_b) == mod_b;
    return nested && (buttons_a & buttons_b) != 0;
}

// What a layer looks like to the step, read once per call.
struct LayerView {
    uint32_t modifier, buttons;
};

// One controller's memory between steps.
struct SlotState {
    uint32_t held  = 0;   // every FH2_PAD_ bit held, last step
    uint32_t owned = 0;   // buttons hidden last step: hidden until they go up, modifier or not
};

// The d-pad as one control: 30 is the d-pad itself (a direction, with values in the reading), 31 its
// left-right axis and 32 its up-down axis. The game sets them with the direction bits, so the radio,
// which reads the d-pad this way, still saw it in the first two runs.
constexpr uint64_t kDpadAsOne = (1ull << 30) | (1ull << 31) | (1ull << 32);

struct StepOut {
    uint64_t hide     = 0;             // game controls to clear from the current reading
    uint64_t take     = 0;             // of those, the ones to mark as taken (digital ones: the game unmarks them itself)
    uint32_t hide_pad = 0;             // the hidden buttons as FH2_PAD_ bits (for the raw button word)
    bool dpad_idle    = false;         // the d-pad as one control goes too: write the reading's idle d-pad
    uint32_t pressed[kMaxLayers] = {}; // per layer: buttons newly pressed while it was open
};

// One frame of one controller: `controls` is the game's current mask, as the game built it. Layers
// whose modifier is held take their held buttons, and a button once taken stays hidden until it goes
// up, so letting go of the modifier first doesn't hand it to the game mid-press. A press is a button
// that wasn't held at all last step, so one already held when the modifier goes down (the game has
// had it) isn't one.
inline StepOut step(SlotState& s, uint64_t controls, const LayerView* layers, int n) {
    StepOut out;
    const uint32_t held = pad_from_controls(controls);
    uint32_t take       = s.owned & held;
    for (int i = 0; i < n && i < kMaxLayers; ++i) {
        const LayerView& l = layers[i];
        uint32_t now       = 0;
        if (l.modifier && (held & l.modifier) == l.modifier) now = l.buttons & held;
        out.pressed[i] = now & ~s.held;
        take |= now;
    }
    s.held       = held;
    s.owned      = take;
    out.hide     = controls_for(take) & controls;
    out.take     = out.hide;
    out.hide_pad = take;
    // Every held d-pad direction hidden: the d-pad as one control is idle too. With a direction still
    // the game's (a layer that takes only some), it stays as the game built it.
    if ((held & kDpad) && !(held & kDpad & ~take)) {
        out.hide |= kDpadAsOne & controls;
        out.dpad_idle = true;
    }
    return out;
}

// Checks for a layer's key: letters, digits and '_', 1 to 40 of them.
inline bool key_ok(const char* k) {
    if (!k || !*k) return false;
    int n = 0;
    for (const char* c = k; *c; ++c, ++n)
        if (n >= 40 || !((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_'))
            return false;
    return true;
}

// Why a layer's bits can't be used, or nullptr.
inline const char* bits_problem(uint32_t modifier, uint32_t buttons) {
    if ((modifier | buttons) & ~kPadBits) return "uses bits that aren't FH2_PAD_ buttons";
    if (!buttons) return "takes no buttons";
    if (modifier & buttons) return "its modifier is one of its own buttons";
    return nullptr;
}

} // namespace fh2platform::pad

// The pad layer preview's rules (src/pad_engine.hpp), with no game. Exits 0 when every check passes.
#include "pad_engine.hpp"

#include <cstdio>

using namespace fh2platform::pad;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

constexpr uint64_t C(int control) { return 1ull << control; }
constexpr int kUp = 0, kDown = 1, kLeft = 2, kRight = 3, kLB = 8, kRB = 9, kLT = 10, kA = 12;

} // namespace

int main() {
    // control numbers and FH2_PAD_ bits
    check(pad_from_controls(C(kUp) | C(kLB)) == (FH2_PAD_DPAD_UP | FH2_PAD_LB), "single buttons map to their FH2_PAD_ bits");
    check(pad_from_controls(C(kLT) | C(11)) == 0, "the triggers' controls aren't FH2_PAD_ bits");
    check(pad_from_controls(C(33)) == (FH2_PAD_DPAD_UP | FH2_PAD_DPAD_LEFT), "diagonal 33 is Up+Left");
    check(pad_from_controls(C(36)) == (FH2_PAD_DPAD_DOWN | FH2_PAD_DPAD_RIGHT), "diagonal 36 is Down+Right");
    check(controls_for(FH2_PAD_DPAD_LEFT) == (C(kLeft) | C(33) | C(35)), "Left also hides both diagonals with Left in them");
    check(controls_for(kDpad) == (0xFull | C(33) | C(34) | C(35) | C(36)), "the whole d-pad hides its four diagonals");
    check(controls_for(FH2_PAD_A) == C(kA), "a face button hides only itself");

    // the bits a layer may use
    check(bits_problem(FH2_PAD_LB, kDpad) == nullptr, "LB with the d-pad is fine");
    check(bits_problem(0, kDpad) == nullptr, "no modifier yet is fine (the layer is off)");
    check(bits_problem(FH2_PAD_LB, 0) != nullptr, "a layer must take buttons");
    check(bits_problem(FH2_PAD_LB, FH2_PAD_LB | FH2_PAD_A) != nullptr, "the modifier can't be one of its own buttons");
    check(bits_problem(FH2_PAD_LB, 0x0400) != nullptr, "XInput's unused bits are refused");
    check(key_ok("QuickChat") && key_ok("a_1") && !key_ok("") && !key_ok("two words") && !key_ok(nullptr), "keys");

    // clashes
    check(clash(FH2_PAD_LB, kDpad, FH2_PAD_LB, FH2_PAD_DPAD_UP), "same modifier, a button in common: clash");
    check(!clash(FH2_PAD_LB, FH2_PAD_DPAD_UP, FH2_PAD_LB, FH2_PAD_DPAD_DOWN), "same modifier, different buttons: fine");
    check(clash(FH2_PAD_LB, kDpad, FH2_PAD_LB | FH2_PAD_RB, kDpad), "LB and LB+RB would both be open: clash");
    check(!clash(FH2_PAD_LB, kDpad, FH2_PAD_RB, kDpad), "LB and RB are separate modifiers: fine");
    check(!clash(0, kDpad, FH2_PAD_LB, kDpad), "a layer that's off clashes with nothing");

    // steps
    const LayerView chat[1] = {{FH2_PAD_LB, kDpad}};
    {
        SlotState s;
        StepOut o = step(s, C(kLeft), chat, 1);
        check(o.hide == 0 && o.pressed[0] == 0, "Left without LB: the game has it, no press");
        o = step(s, C(kLeft) | C(kLB), chat, 1);
        check(o.hide == C(kLeft) && o.pressed[0] == 0, "LB down while Left is held: hidden from now on, but not a press");
        o = step(s, C(kLB), chat, 1);
        check(o.hide == 0 && o.pressed[0] == 0, "Left up: nothing");
        o = step(s, C(kLB) | C(kLeft), chat, 1);
        check(o.hide == C(kLeft) && o.pressed[0] == FH2_PAD_DPAD_LEFT, "LB held, Left pressed: hidden and one press");
        o = step(s, C(kLB) | C(kLeft), chat, 1);
        check(o.hide == C(kLeft) && o.pressed[0] == 0, "Left still held: hidden, no second press");
        o = step(s, C(kLeft), chat, 1);
        check(o.hide == 0 && o.pressed[0] == 0, "LB let go first: not hidden by the layer (the game keeps it taken until Left goes up)");
    }
    {
        SlotState s;
        step(s, C(kLB), chat, 1);
        StepOut o = step(s, C(kLB) | C(34), chat, 1);
        check(o.hide == C(34) && o.pressed[0] == (FH2_PAD_DPAD_UP | FH2_PAD_DPAD_RIGHT), "a diagonal is hidden, and both directions are presses");
        o = step(s, C(kLB) | C(kA), chat, 1);
        check(o.hide == 0 && o.pressed[0] == 0, "A isn't the layer's: the game has it");
        o = step(s, C(kLB) | C(kRB) | C(kDown), chat, 1);
        check(o.hide == C(kDown) && o.pressed[0] == FH2_PAD_DPAD_DOWN, "extra buttons held with the modifier still open the layer");
    }
    {
        const LayerView two[2] = {{FH2_PAD_LB, FH2_PAD_DPAD_UP}, {FH2_PAD_RB, FH2_PAD_DPAD_UP | FH2_PAD_A}};
        SlotState s;
        StepOut o = step(s, C(kRB) | C(kA), two, 2);
        check(o.hide == C(kA) && o.pressed[0] == 0 && o.pressed[1] == FH2_PAD_A, "two layers: only the open one takes its button");
        const LayerView off[1] = {{0, kDpad}};
        SlotState t;
        o = step(t, C(kLB) | C(kUp), off, 1);
        check(o.hide == 0 && o.pressed[0] == 0, "a layer with no modifier does nothing");
    }

    std::printf("%s\n", g_failed ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failed ? 1 : 0;
}

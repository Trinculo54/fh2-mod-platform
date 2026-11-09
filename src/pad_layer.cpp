// The pad layer preview (FH2Platform.PadLayer v1, include/fh2mod_pad.h): while a layer's modifier is
// held, the game doesn't act on the layer's buttons, and the mod is told about each press.
//
// How the game reads a controller. CControllerManager (vtable game+0x2E7A2E8, the one instance at
// game+0x365D7C8, made at game+0xF654FB) has 9 slots of 0x1300 bytes from +0x10, and +0xAB28 points
// at the slot the game is using. Each frame its update (game+0x1075930 on) copies a slot's current
// reading (+0x70) to the previous one (+0x128), builds the new one from the pad (game+0x1074410:
// +0x78 becomes the mask of held controls, numbered as XInput's bits, with the triggers at 10 and 11
// and the d-pad's diagonals at 33 to 36), then calls game+0x10768D0, which clears bits in the slot's
// mask of taken controls (+0x1E8) for controls no longer held. Every press query (game+0x106EEF0)
// reads a control in that mask as not pressed: it's how the game makes a button that opened a menu
// stop counting until it's let go.
//
// The pad layer hooks game+0x10768D0, which runs right after the reading is built. After the game has
// cleared what it clears, the layers' buttons held while their modifier is held are hidden: taken out
// of the reading's mask (+0x78) and its raw button word (+0xC0), and marked as taken. Not only taken:
// the first in-game run showed the game still acting on the d-pad, and the game's value query
// (game+0x106F040) reads the mask without looking at the taken one. Run 2 then had telemetry stopped but the radio
// still changing stations: with each direction the game also sets the d-pad as one control (30, axes 31
// and 32, with a direction and values in the reading), and the radio reads that. When a layer has every
// held direction, that goes too, and the reading's d-pad is written as the builder leaves it idle. A button once hidden stays hidden
// until it goes up, so letting go of the modifier first can't give the game a press it didn't get.
// The same pad is in more than one slot, so presses only count from the slot the game is using.
// Everything runs on the game's thread, in the game's own order; no game memory is written anywhere
// else. Presses go to a platform thread, which calls the mods.
//
// Game memory is read through safe_copy, never with a plain load: clang's __try only covers calls made
// inside it, and a plain load can be moved out of it. The slot is written only once those reads have
// worked, inside a helper called through a pointer from a __try of its own.
#include "pad_engine.hpp"
#include "platform.hpp"

#include "fh2mod_pad.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace fh2platform {

namespace {

using namespace pad;

// ---- game addresses (forza_durango_release_final.exe of 24 Nov 2015 only) ----
constexpr uintptr_t RVA_MANAGER    = 0x365D7C8;   // CControllerManager* game+0x365D7C8
constexpr uintptr_t RVA_MANAGER_VT = 0x2E7A2E8;   // CControllerManager's vtable game+0x2E7A2E8
constexpr uintptr_t RVA_RELEASE    = 0x10768D0;   // slot: clear taken controls no longer held, game+0x10768D0
constexpr uintptr_t OFF_SLOTS      = 0x10;        // manager: the first slot
constexpr uintptr_t SLOT_SIZE      = 0x1300;
constexpr uintptr_t OFF_ACTIVE     = 0xAB28;      // manager: the slot in use
constexpr uintptr_t OFF_CONTROLS   = 0x78;        // slot: held controls (the current reading +0x70, its mask +8)
constexpr uintptr_t OFF_TAKEN      = 0x1E8;       // slot: taken controls
constexpr uintptr_t OFF_RAW        = 0xC0;        // slot: the pad's raw buttons (XInput bits), as HonkHonk reads them
constexpr uintptr_t OFF_DPAD       = 0x80;        // slot: the reading's d-pad as one control (+0x10: direction, then 4 floats);
                                                  // game+0x1074436 sets it idle (8, then zeros) each frame

struct CodeCheck {
    uintptr_t rva;
    const char* what;
    unsigned char bytes[24];
    unsigned n;
};
// What the hook's reasoning rests on, checked in memory before the hook is made (none of these is the
// start of a function, so no mod's hook can sit on them). The hook's own target is checked against the
// exe on disk by hook_function.
const CodeCheck kChecks[] = {
    {0x1076910, "game+0x1076910 (the release loop: test [rbx+0x78] then test [rbx+0x1E8])",
     {0x48, 0x85, 0x73, 0x78, 0x0F, 0x85, 0xED, 0x01, 0x00, 0x00, 0x48, 0x85, 0xB3, 0xE8, 0x01, 0x00, 0x00}, 17},
    {0x1076927, "game+0x1076927 (the release loop: a digital control's bit is cleared)",
     {0x41, 0xF6, 0x06, 0x01, 0x74, 0x12, 0x48, 0x8B, 0xC6, 0x48, 0xF7, 0xD0, 0x48, 0x21, 0x83, 0xE8, 0x01, 0x00, 0x00}, 19},
    {0x106EF10, "game+0x106EF10 (the press query reads a taken control as not pressed)",
     {0x4C, 0x85, 0x87, 0xE8, 0x01, 0x00, 0x00, 0x0F, 0x85, 0x09, 0x01, 0x00, 0x00, 0x4C, 0x85, 0x87, 0x30, 0x01, 0x00, 0x00}, 20},
    {0x1074436, "game+0x1074436 (the reading builder sets the d-pad idle: direction 8)",
     {0x48, 0xC7, 0x41, 0x10, 0x08, 0x00, 0x00, 0x00, 0x48, 0x89, 0x69, 0x18}, 12},
    {0x1074755, "game+0x1074755 (d-pad up also sets controls 30 and 32)", {0x48, 0xB8, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00}, 10},
    {0x1075AD5, "game+0x1075AD5 (the pads' update calls game+0x10768D0)", {0xE8, 0xF6, 0x0D, 0x00, 0x00}, 5},
    {0x1075F12, "game+0x1075F12 (slot 8's update calls game+0x10768D0)", {0xE8, 0xB9, 0x09, 0x00, 0x00}, 5},
};

FH2Mod make_owner() {
    FH2Mod m;
    m.name          = "Pad layer";
    m.wname         = L"Pad layer";
    m.check_updates = false;
    return m;
}
FH2Mod g_owner = make_owner();   // owns the hook; never registered
const FH2ModApi* g_api = nullptr;
std::atomic<uintptr_t> g_base{0};

struct Layer {
    FH2Mod* mod = nullptr;
    std::string key, mod_name;
    uint32_t buttons = 0;
    std::atomic<uint32_t> modifier{0};
    FH2PadPressFn on_press = nullptr;
    void* user             = nullptr;
};
std::mutex g_mu;                    // adding layers, changing modifiers
Layer g_layers[kMaxLayers];
std::atomic<int> g_count{0};        // layers [0, count) are complete
std::atomic<bool> g_live{false};    // the hook is in and the checks passed
bool g_started = false;

SlotState g_slots[kSlots];          // game thread only
std::atomic<uint32_t> g_held[kSlots];   // per slot: the buttons held before the layers hid any (for buttons())

// Presses, from the game's thread to the delivery thread.
struct Press {
    int layer;
    uint32_t button;
};
constexpr unsigned kQueue = 64;
Press g_queue[kQueue];
unsigned g_head = 0, g_tail = 0;    // under g_qlock
SRWLOCK g_qlock = SRWLOCK_INIT;
HANDLE g_wake   = nullptr;
std::atomic<unsigned> g_dropped{0};

const char* button_name(uint32_t b) {
    switch (b) {
        case FH2_PAD_DPAD_UP: return "Up";
        case FH2_PAD_DPAD_DOWN: return "Down";
        case FH2_PAD_DPAD_LEFT: return "Left";
        case FH2_PAD_DPAD_RIGHT: return "Right";
        case FH2_PAD_START: return "Menu";
        case FH2_PAD_BACK: return "View";
        case FH2_PAD_LTHUMB: return "LS";
        case FH2_PAD_RTHUMB: return "RS";
        case FH2_PAD_LB: return "LB";
        case FH2_PAD_RB: return "RB";
        case FH2_PAD_A: return "A";
        case FH2_PAD_B: return "B";
        case FH2_PAD_X: return "X";
        case FH2_PAD_Y: return "Y";
        default: return "?";
    }
}

std::string bits_name(uint32_t bits) {
    if (!bits) return "off";
    std::string s;
    for (uint32_t b = 1; b <= 0x8000; b <<= 1)
        if (bits & b) s += (s.empty() ? "" : "+") + std::string(button_name(b));
    return s;
}

void say(const std::string& text) { log_line(g_owner.name, text); }

// ---- the game's thread ----

void push_press(int layer, uint32_t button) {
    AcquireSRWLockExclusive(&g_qlock);
    const bool full = g_tail - g_head >= kQueue;
    if (!full) g_queue[g_tail++ % kQueue] = {layer, button};
    ReleaseSRWLockExclusive(&g_qlock);
    if (full) g_dropped.fetch_add(1, std::memory_order_relaxed);
    else SetEvent(g_wake);
}

// One value of game memory, or false if it can't be read.
template <class T> bool rd(uintptr_t a, T& v) { return a >= 0x10000 && safe_copy(&v, reinterpret_cast<const void*>(a), sizeof v); }

// The controller manager, read from the game: false until the game has made it, or if the pointer isn't to an
// object with its vtable.
bool manager(uintptr_t base, uintptr_t* mgr) {
    uintptr_t vt = 0;
    return base && rd(base + RVA_MANAGER, *mgr) && *mgr && rd(*mgr, vt) && vt == base + RVA_MANAGER_VT;
}

// Which of the manager's slots `at` is, or -1 if it isn't the start of one.
int slot_index(uintptr_t mgr, uintptr_t at) {
    const uintptr_t first = mgr + OFF_SLOTS;
    if (at < first || at >= first + kSlots * SLOT_SIZE || (at - first) % SLOT_SIZE) return -1;
    return static_cast<int>((at - first) / SLOT_SIZE);
}

// The writes to a slot, called only through g_hide (below), so a fault in them unwinds to the guarded call.
__attribute__((noinline)) void hide_in_slot(unsigned char* slot, const StepOut* out) {
    // Not just taken: some of the game's readers (the value query, game+0x106F040) don't look at the taken
    // mask, so the buttons go from the reading itself, and from its raw button word.
    *reinterpret_cast<uint64_t*>(slot + OFF_CONTROLS) &= ~out->hide;
    *reinterpret_cast<uint16_t*>(slot + OFF_RAW) &= static_cast<uint16_t>(~out->hide_pad);
    *reinterpret_cast<uint64_t*>(slot + OFF_TAKEN) |= out->take;
    if (out->dpad_idle) {   // the reading's d-pad as the builder leaves it with nothing held
        *reinterpret_cast<uint64_t*>(slot + OFF_DPAD)      = 8;   // direction 8 (none), magnitude 0
        *reinterpret_cast<uint64_t*>(slot + OFF_DPAD + 8)  = 0;
        *reinterpret_cast<uint32_t*>(slot + OFF_DPAD + 16) = 0;
    }
}
void (*volatile g_hide)(unsigned char*, const StepOut*) = hide_in_slot;

// No objects with destructors here (__try).
bool hide_guarded(unsigned char* slot, const StepOut* out) {
    __try {
        g_hide(slot, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One slot's frame: true when its presses count (it's the slot the game is using; the game keeps the
// same pad in more than one slot, so presses from the others would come twice). If the manager, the slot
// or its reading can't be read, or the slot can't be written, nothing is hidden and no press counts, and
// the slot's state stays as it was.
bool step_slot(unsigned char* slot, StepOut* out) {
    const uintptr_t base = g_base.load(std::memory_order_relaxed), at = reinterpret_cast<uintptr_t>(slot);
    uintptr_t mgr = 0, active = 0;
    uint64_t controls = 0;
    if (!manager(base, &mgr)) return false;
    const int idx = slot_index(mgr, at);
    if (idx < 0 || !rd(mgr + OFF_ACTIVE, active) || !rd(at + OFF_CONTROLS, controls)) return false;
    LayerView views[kMaxLayers];
    const int n = g_count.load(std::memory_order_acquire);
    // a layer of a mod stopped for online play (platform.cpp's go_online) is off: modifier 0
    for (int i = 0; i < n; ++i)
        views[i] = {stopped_online(g_layers[i].mod) ? 0u : g_layers[i].modifier.load(std::memory_order_relaxed), g_layers[i].buttons};
    const SlotState before = g_slots[idx];
    *out                   = step(g_slots[idx], controls, views, n);
    if (out->hide && !hide_guarded(slot, out)) {
        g_slots[idx] = before;
        return false;
    }
    g_held[idx].store(g_slots[idx].held, std::memory_order_relaxed);
    return active ? active == at : idx == 0;
}

using ReleaseFn = void (*)(void* slot);
ReleaseFn o_release = nullptr;

void d_release(void* slot) {
    o_release(slot);
    if (!g_live.load(std::memory_order_acquire) || !slot) return;
    StepOut out;
    if (!step_slot(static_cast<unsigned char*>(slot), &out)) return;
    const int n = g_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        for (uint32_t b = out.pressed[i]; b; b &= b - 1) push_press(i, b & (~b + 1));
}

// ---- the delivery thread ----

bool call_press(FH2PadPressFn fn, void* user, const char* key, uint32_t button) {
    __try {
        fn(user, key, button);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI deliver(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;) {
            Press p{};
            AcquireSRWLockExclusive(&g_qlock);
            const bool any = g_head != g_tail;
            if (any) p = g_queue[g_head++ % kQueue];
            ReleaseSRWLockExclusive(&g_qlock);
            if (!any) break;
            Layer& l = g_layers[p.layer];
            if (stopped_online(l.mod)) continue;   // stopped for online play (platform.cpp's go_online)
            if (!call_press(l.on_press, l.user, l.key.c_str(), p.button))
                log_line(l.mod_name, "crashed in its on_press for pad layer " + l.key);
        }
        if (const unsigned d = g_dropped.exchange(0)) say(std::to_string(d) + " press(es) dropped: the mods weren't keeping up");
    }
}

// ---- start-up ----

bool code_ok(uintptr_t base, const CodeCheck& c) {
    unsigned char got[sizeof c.bytes];
    return c.n <= sizeof got && safe_copy(got, reinterpret_cast<const void*>(base + c.rva), c.n) && std::memcmp(got, c.bytes, c.n) == 0;
}

// The code the pad layer rests on, checked in memory: "" if it's as expected, else what doesn't match.
const char* code_mismatch(uintptr_t base) {
    for (const CodeCheck& c : kChecks)
        if (!code_ok(base, c)) return c.what;
    return "";
}

DWORD WINAPI start_thread(LPVOID) {
    void* game = g_api->wait_for_code(0xFFFFFFFFu);
    if (!game) {
        say("the game's code never became ready (or Azimuth started another game); pad layers are off");
        return 0;
    }
    const auto base = reinterpret_cast<uintptr_t>(game);
    if (const char* bad = code_mismatch(base); *bad) {
        say(std::string("pad layers are off: the game code doesn't match at ") + bad);
        return 0;
    }
    g_base.store(base);
    FH2Hook* h = g_api->hook_function(&g_owner, "controller: taken controls (pad layer)", reinterpret_cast<void*>(base + RVA_RELEASE),
                                      reinterpret_cast<void*>(&d_release), reinterpret_cast<void**>(&o_release));
    if (!h || !g_api->enable_hooks(&g_owner, h)) {
        if (h) g_api->disable_hooks(&g_owner, h);
        say("pad layers are off: the hook at game+0x10768D0 couldn't be made (the log above says why)");
        return 0;
    }
    g_live.store(true, std::memory_order_release);
    say("pad layers are on (" + std::to_string(g_count.load()) + ")");
    return 0;
}

// ---- the interface ----

Layer* find(FH2Mod* mod, const char* key) {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i)
        if (g_layers[i].mod == mod && g_layers[i].key == key) return &g_layers[i];
    return nullptr;
}

// Another layer this one would clash with, or nullptr (under g_mu).
const Layer* clashes(const Layer* self, uint32_t modifier, uint32_t buttons) {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        const Layer& o = g_layers[i];
        if (&o != self && clash(modifier, buttons, o.modifier.load(), o.buttons)) return &o;
    }
    return nullptr;
}

std::string describe(uint32_t modifier, uint32_t buttons) {
    return modifier ? "while " + bits_name(modifier) + " is held, the game doesn't see " + bits_name(buttons)
                    : "off until its mod sets a modifier (" + bits_name(buttons) + ")";
}

int api_add_layer(FH2Mod* mod, const FH2PadLayer* l) {
    if (!mod) return 0;
    if (!l || l->size < sizeof(FH2PadLayer)) {
        log_line(mod->name, "add_layer: the FH2PadLayer's size is too small (set size = sizeof(FH2PadLayer))");
        return 0;
    }
    const std::string key = l->key ? l->key : "";
    if (!key_ok(l->key)) {
        log_line(mod->name, "add_layer: \"" + key + "\" isn't a usable key (letters, digits, _)");
        return 0;
    }
    if (!in_mod_init()) {
        log_line(mod->name, "add_layer: pad layer " + key + " came after fh2mod_init; add layers from fh2mod_init");
        return 0;
    }
    if (const char* why = bits_problem(l->modifier, l->buttons)) {
        log_line(mod->name, "add_layer: pad layer " + key + " can't be used: " + why);
        return 0;
    }
    if (!l->on_press || l->flags) {
        log_line(mod->name, "add_layer: pad layer " + key + (l->on_press ? " has flags set (none exist yet)" : " needs an on_press"));
        return 0;
    }
    std::scoped_lock lk{g_mu};
    if (find(mod, l->key)) {
        log_line(mod->name, "add_layer: there's already a pad layer called " + key);
        return 0;
    }
    if (g_count.load() >= kMaxLayers) {
        log_line(mod->name, "add_layer: pad layer " + key + " is one too many (" + std::to_string(kMaxLayers) + " at most, across all mods)");
        return 0;
    }
    if (const Layer* o = clashes(nullptr, l->modifier, l->buttons)) {
        log_line(mod->name, "add_layer: pad layer " + key + " clashes with " + o->mod_name + "'s " + o->key + " (" +
                                bits_name(o->modifier.load()) + " with " + bits_name(o->buttons) + ")");
        return 0;
    }
    Layer& n   = g_layers[g_count.load()];
    n.mod      = mod;
    n.mod_name = mod->name;
    n.key      = key;
    n.buttons  = l->buttons;
    n.modifier.store(l->modifier);
    n.on_press = l->on_press;
    n.user     = l->user;
    g_count.fetch_add(1, std::memory_order_release);
    log_line(mod->name, "pad layer " + key + ": " + describe(l->modifier, l->buttons));
    return 1;
}

int api_set_modifier(FH2Mod* mod, const char* key, uint32_t modifier) {
    if (!mod || !key) return 0;
    std::scoped_lock lk{g_mu};
    Layer* l = find(mod, key);
    if (!l) {
        log_line(mod->name, std::string("set_modifier: there's no pad layer called ") + key);
        return 0;
    }
    if (const char* why = bits_problem(modifier, l->buttons)) {
        log_line(mod->name, "set_modifier: pad layer " + l->key + " can't take that modifier: " + why);
        return 0;
    }
    if (const Layer* o = clashes(l, modifier, l->buttons)) {
        log_line(mod->name, "set_modifier: " + bits_name(modifier) + " for pad layer " + l->key + " clashes with " + o->mod_name + "'s " +
                                o->key + "; it keeps " + bits_name(l->modifier.load()));
        return 0;
    }
    if (l->modifier.exchange(modifier) != modifier) log_line(mod->name, "pad layer " + l->key + ": " + describe(modifier, l->buttons));
    return 1;
}

bool read_held(uint32_t* held) {
    const uintptr_t base = g_base.load(std::memory_order_relaxed);
    uintptr_t mgr = 0, slot = 0;
    uint64_t controls = 0;
    if (!manager(base, &mgr) || !rd(mgr + OFF_ACTIVE, slot)) return false;
    const int idx = slot ? slot_index(mgr, slot) : -1;
    if (idx < 0 || !rd(slot + OFF_CONTROLS, controls)) return false;
    // with layers on, the reading has their buttons taken out: what was held before that
    *held = pad_from_controls(controls);
    if (g_live.load(std::memory_order_acquire)) *held |= g_held[idx].load(std::memory_order_relaxed);
    return true;
}

int api_buttons(uint32_t* held) {
    if (!held) return 0;
    *held = 0;
    return read_held(held) ? 1 : 0;
}

const FH2PadInterface g_iface = {sizeof(FH2PadInterface), FH2_PAD_INTERFACE_VERSION, api_add_layer, api_set_modifier, api_buttons};

} // namespace

const void* pad_interface() { return &g_iface; }
FH2Mod* pad_layer_owner() { return &g_owner; }

// After every fh2mod_init: hooks only when some mod added a layer. buttons() still works without
// layers once the code is ready and as expected, so it gets the game's base either way.
void pad_layer_start(const FH2ModApi* api) {
    if (g_started) return;
    g_started = true;
    g_api     = api;
    if (g_count.load() == 0) {
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            void* game = g_api->wait_for_code(0xFFFFFFFFu);
            if (!game) return 0;
            const auto base = reinterpret_cast<uintptr_t>(game);
            if (const char* bad = code_mismatch(base); *bad) {
                say(std::string("the pad's buttons() is off: the game code doesn't match at ") + bad);
                return 0;
            }
            g_base.store(base);
            return 0;
        }, nullptr, 0, nullptr));
        return;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, deliver, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, start_thread, nullptr, 0, nullptr));
}

} // namespace fh2platform

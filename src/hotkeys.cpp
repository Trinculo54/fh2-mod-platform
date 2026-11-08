// Hotkeys (API 9, platform 2.8.0). A mod gives a keyboard key (with Ctrl, Shift or Alt) and a
// controller chord, and the platform calls it back when either is pressed. Keys are polled with
// GetAsyncKeyState, and pads with XInput (xinput1_4 from System32, loaded when the first hotkey
// is added), only while Azimuth's window is in front: nothing is taken from other programs
// (no RegisterHotKey), and the game still gets every key and button. Each hotkey gets rows on
// the Mods page in its mod's part: its key, as a choice of Off, F1 to F12 and Ctrl+F1 to F12
// (and the mod's own default if it's none of those), and its chord, Off or LB+RB with a face
// button or a d-pad direction. A change there takes effect straight away, and on_rebind tells
// the mod, which keeps the bindings.
//
// With a mod that has a HUD layer (the HUD layer preview), the same thread also follows the device the
// player used last, for the layer's button prompts (the device sweep): the whole keyboard, the mouse and
// the XInput pads, judged by hud_state.cpp. Without one, poll() is 2.8.0's exactly.
#include "hud_layer.hpp"
#include "hud_state.hpp"
#include "lang.hpp"
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

namespace fh2platform {

namespace {

struct Binding {
    uint32_t vk = 0, modifiers = 0; // modifiers: FH2_KEY_
    uint32_t chord = 0;             // XInput button bits
};

struct Hotkey {
    FH2Mod* mod;
    std::string key, label;
    FH2HotkeyFn on_press;
    FH2HotkeyRebindFn on_rebind;
    void* user;
    std::atomic<uint32_t> vk{0}, modifiers{0}, chord{0}; // the binding, changed from the UI thread
    std::vector<Binding> keys, chords;                  // what the rows offer (index = the row's value)
    std::vector<std::string> key_names, chord_names;
    bool down = false; // polling thread only
};

std::mutex g_mu;
std::deque<Hotkey> g_hotkeys; // never removed or moved: the rows' `user` points into it
bool g_started = false;
bool g_xinput_said = false;        // g_mu: the hotkeys' line about XInput is written
std::atomic<bool> g_sweep{false};  // the HUD layer's device sweep is on
std::atomic<int> g_auto_kbd{-1};   // what it judged: -1 not yet, 0 pad, 1 keyboard and mouse

// XInput's XINPUT_STATE (the part we read).
struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
XInputGetStateFn g_xinput = nullptr;

const uint32_t kLB = FH2_PAD_LB, kRB = FH2_PAD_RB;
struct ChordName {
    uint32_t bits;
    const char* name;
};
const ChordName kChords[] = {
    {kLB | kRB | FH2_PAD_Y, "LB+RB+Y"},          {kLB | kRB | FH2_PAD_X, "LB+RB+X"},
    {kLB | kRB | FH2_PAD_B, "LB+RB+B"},          {kLB | kRB | FH2_PAD_A, "LB+RB+A"},
    {kLB | kRB | FH2_PAD_DPAD_UP, "LB+RB+Up"},   {kLB | kRB | FH2_PAD_DPAD_DOWN, "LB+RB+Down"},
    {kLB | kRB | FH2_PAD_DPAD_LEFT, "LB+RB+Left"}, {kLB | kRB | FH2_PAD_DPAD_RIGHT, "LB+RB+Right"},
};

std::string vk_name(uint32_t vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num " + std::to_string(vk - VK_NUMPAD0);
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) { // extended keys, so the name is the arrow, not the numpad's
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END:
        case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: sc |= 0x100; break;
        default: break;
    }
    wchar_t name[64] = {};
    if (sc && GetKeyNameTextW(static_cast<LONG>(sc << 16), name, 64) > 0) return narrow(name);
    char hex[16];
    std::snprintf(hex, sizeof hex, "key 0x%02X", vk);
    return hex;
}

std::string binding_name(uint32_t vk, uint32_t modifiers) {
    if (!vk) return "Off";
    std::string s;
    if (modifiers & FH2_KEY_CTRL) s += "Ctrl+";
    if (modifiers & FH2_KEY_SHIFT) s += "Shift+";
    if (modifiers & FH2_KEY_ALT) s += "Alt+";
    return s + vk_name(vk);
}

std::string chord_name(uint32_t chord) {
    if (!chord) return "Off";
    for (const ChordName& c : kChords)
        if (c.bits == chord) return c.name;
    static const ChordName parts[] = {{FH2_PAD_LB, "LB"}, {FH2_PAD_RB, "RB"}, {FH2_PAD_Y, "Y"}, {FH2_PAD_X, "X"},
                                      {FH2_PAD_B, "B"}, {FH2_PAD_A, "A"}, {FH2_PAD_DPAD_UP, "Up"},
                                      {FH2_PAD_DPAD_DOWN, "Down"}, {FH2_PAD_DPAD_LEFT, "Left"},
                                      {FH2_PAD_DPAD_RIGHT, "Right"}, {FH2_PAD_START, "Menu"}, {FH2_PAD_BACK, "View"},
                                      {FH2_PAD_LTHUMB, "LS"}, {FH2_PAD_RTHUMB, "RS"}};
    std::string s;
    for (const ChordName& p : parts)
        if (chord & p.bits) s += (s.empty() ? "" : "+") + std::string(p.name);
    return s;
}

bool key_is(const char* k) {
    if (!k || !*k || std::strlen(k) > 40) return false;
    for (const char* c = k; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    return true;
}

bool in_front() {
    DWORD pid = 0;
    HWND w    = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

bool call_press(FH2HotkeyFn fn, void* user, const char* key) {
    __try {
        fn(user, key);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool call_rebind(FH2HotkeyRebindFn fn, void* user, const char* key, uint32_t vk, uint32_t mods, uint32_t chord) {
    __try {
        fn(user, key, vk, mods, chord);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool modifier_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// ---- the HUD layer's device sweep ----
struct Sweep {
    bool fresh     = true;   // the first round, and the first after coming back to the front: state only
    bool first     = true;   // the first judged state is still to come
    unsigned round = 0;
    HudKeyWatch keys;
    HudMouseMoves mouse;
    HudDeviceJudge judge;
    HudPad last[4];
    bool connected[4] = {}, tried[4] = {};
    double next_probe[4] = {};
    bool ever = false, none_logged = false;
    int lines = 0;
};

constexpr int kDeviceLines = 200;

// True while device lines may still be written; the one after the last says they stop.
bool device_line(Sweep& s) {
    if (++s.lines <= kDeviceLines) return true;
    if (s.lines == kDeviceLines + 1)
        log_line("HUD layer", "device: more than 200 device lines; no more of them this session (the prompts still follow the device)");
    return false;
}

std::string at_ms(double now) { return " at " + std::to_string(static_cast<long long>(now)) + " ms"; }

// One 15 ms round, while a window of this process is in front: the keys every other round, the mouse,
// the pads (a connected slot every round, an empty one once a second); `pads` gets their buttons for the
// hotkeys' chords.
void sweep_round(Sweep& s, bool front, WORD pads[4]) {
    hud_layer_sweep_check();
    const double now = hud_layer_now();
    if (!front) {
        s.mouse.forget();   // the pointer is elsewhere's business until the game is in front again
        s.fresh = true;
        return;
    }
    const char* kb    = nullptr;
    char key_text[32] = "";
    if ((++s.round & 1) || s.fresh) {
        bool down[256] = {};
        for (int vk = 1; vk < 255; ++vk) down[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        std::vector<uint32_t> hot;   // the hotkeys' current keys never flip the prompts
        {
            std::scoped_lock lk{g_mu};
            for (const Hotkey& h : g_hotkeys)
                if (const uint32_t vk = h.vk.load()) hot.push_back(vk);
        }
        kb = s.keys.round(down, hot.data(), hot.size(), key_text, sizeof key_text);
    }
    POINT p{};
    const bool read = GetCursorPos(&p) != 0;
    if (s.mouse.round(read, p.x, p.y) && !kb) kb = "mouse moved";
    const char* pad   = nullptr;
    char pad_text[48] = "";
    int first_pad     = -1;
    for (DWORD i = 0; i < 4 && g_xinput; ++i) {
        if (!s.connected[i] && now < s.next_probe[i]) continue;
        s.tried[i] = true;
        PadState st{};
        if (g_xinput(i, &st) != ERROR_SUCCESS) {
            if (s.connected[i] && device_line(s)) log_line("HUD layer", "device: XInput pad " + std::to_string(i) + " disconnected" + at_ms(now));
            s.connected[i]  = false;
            s.next_probe[i] = now + 1000;
            continue;
        }
        const HudPad a{st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry};
        pads[i] = st.buttons;
        if (first_pad < 0) first_pad = static_cast<int>(i);
        if (!s.connected[i]) {
            if (device_line(s)) log_line("HUD layer", "device: XInput pad " + std::to_string(i) + " connected" + at_ms(now));
            s.ever = true;
        } else if (const char* what = !pad ? hud_pad_input(a, s.last[i]) : nullptr) {
            if (a.buttons != s.last[i].buttons) std::snprintf(pad_text, sizeof pad_text, "pad %lu, buttons 0x%04X", i, a.buttons);
            else std::snprintf(pad_text, sizeof pad_text, "pad %lu, %s", i, what);
            pad = pad_text;
        }
        s.last[i]      = a;
        s.connected[i] = true;
    }
    if (!s.none_logged && !s.ever && s.tried[0] && s.tried[1] && s.tried[2] && s.tried[3]) {
        s.none_logged = true;
        if (device_line(s))
            log_line("HUD layer", "device: no XInput pad in slots 0-3 (pads that aren't XInput, such as a DualSense without Steam Input, "
                                  "aren't seen: choose Controller on the Mods page)");
    }
    if (s.first) {   // the first judged state: a pad connected in the first round, else keyboard and mouse
        s.first     = false;
        s.judge.kbd = first_pad < 0;
        g_auto_kbd  = s.judge.kbd ? 1 : 0;
        static const char* const kChoice[3] = {"Automatic", "Controller", "Keyboard and mouse"};
        const int choice                    = hud_layer_choice();
        if (choice != 0)
            log_line("HUD layer", std::string("button prompts: ") + kChoice[choice] + "; the device is followed for the log only");
        else if (first_pad < 0)
            log_line("HUD layer", "button prompts: Automatic, starting on key caps (no XInput pad connected)");
        else
            log_line("HUD layer", "button prompts: Automatic, starting on pad pictures (XInput pad " + std::to_string(first_pad) + ")");
    }
    if (s.fresh) {   // the state is taken; nothing is judged this round
        s.fresh = false;
        return;
    }
    if (!s.judge.round(kb, pad, now)) return;
    g_auto_kbd = s.judge.kbd ? 1 : 0;
    if (device_line(s))
        log_line("HUD layer", std::string("device: ") + (s.judge.kbd ? "keyboard and mouse (" : "pad (") + s.judge.cause + ")" + at_ms(now));
}

DWORD WINAPI poll(LPVOID) {
    WORD last_pads[4] = {};
    Sweep sweep;   // the device sweep's, used only while g_sweep is on
    for (;;) {
        Sleep(15);
        const bool front = in_front();
        uint32_t mods    = 0;
        if (front) {
            if (modifier_down(VK_CONTROL)) mods |= FH2_KEY_CTRL;
            if (modifier_down(VK_SHIFT)) mods |= FH2_KEY_SHIFT;
            if (modifier_down(VK_MENU)) mods |= FH2_KEY_ALT;
        }
        WORD pads[4] = {};
        if (g_sweep.load())
            sweep_round(sweep, front, pads);
        else if (front && g_xinput)
            for (DWORD i = 0; i < 4; ++i) {
                PadState s{};
                if (g_xinput(i, &s) == ERROR_SUCCESS) pads[i] = s.buttons;
            }
        std::vector<Hotkey*> fire;
        {
            std::scoped_lock lk{g_mu};
            for (Hotkey& h : g_hotkeys) {
                const uint32_t vk = h.vk.load(), want = h.modifiers.load(), chord = h.chord.load();
                bool now = front && vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) && mods == want;
                for (int i = 0; i < 4 && chord && front; ++i)
                    if ((pads[i] & chord) == chord && (last_pads[i] & chord) != chord) now = true;   // a new press
                bool pad_held = false;
                for (int i = 0; i < 4 && chord && front; ++i)
                    if ((pads[i] & chord) == chord) pad_held = true;
                if (now && !h.down) fire.push_back(&h);
                h.down = now || pad_held;
            }
        }
        for (int i = 0; i < 4; ++i) last_pads[i] = pads[i];
        for (Hotkey* h : fire)
            if (!call_press(h->on_press, h->user, h->key.c_str()))
                log_line(h->mod->name, "crashed in its on_press for hotkey " + h->key);
    }
}

// The poll thread and XInput, once, for the hotkeys and the device sweep alike (g_mu held).
void start_poll() {
    if (g_started) return;
    g_started = true;
    HMODULE x = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!x) x = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    g_xinput = x ? reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState"))) : nullptr;
    if (HANDLE t = CreateThread(nullptr, 0, poll, nullptr, 0, nullptr)) CloseHandle(t);
}

// A row on the Mods page changed (UI thread): the new binding, live, and the mod told.
void on_row(void* user, const char* row, int value) {
    auto* h         = static_cast<Hotkey*>(user);
    const bool pad  = std::strncmp(row, "HotkeyPad_", 10) == 0;
    const auto& opt = pad ? h->chords : h->keys;
    if (value < 0 || value >= static_cast<int>(opt.size())) return;
    if (pad) h->chord = opt[value].chord;
    else {
        h->vk        = opt[value].vk;
        h->modifiers = opt[value].modifiers;
    }
    log_line(h->mod->name, "hotkey " + h->key + ": " + (pad ? "controller " + h->chord_names[value] : h->key_names[value]));
    if (h->on_rebind && !call_rebind(h->on_rebind, h->user, h->key.c_str(), h->vk, h->modifiers, h->chord))
        log_line(h->mod->name, "crashed in its on_rebind for hotkey " + h->key);
}

} // namespace

int hotkey_add(FH2Mod* m, const FH2Hotkey* k) {
    if (!m || !k) return 0;
    if (k->size < sizeof(FH2Hotkey)) {
        log_line(m->name, "add_hotkey: the FH2Hotkey's size is too small (set size = sizeof(FH2Hotkey))");
        return 0;
    }
    if (!key_is(k->key)) {
        log_line(m->name, std::string("add_hotkey: \"") + (k->key ? k->key : "") + "\" isn't a usable key (letters, digits, _)");
        return 0;
    }
    if (!k->label || !*k->label || !k->on_press) {
        log_line(m->name, std::string("add_hotkey: ") + k->key + " needs a label and an on_press");
        return 0;
    }
    if (k->vk > 0xFE || (k->modifiers & ~7u)) {
        log_line(m->name, std::string("add_hotkey: ") + k->key + "'s key isn't a virtual-key code with FH2_KEY_ modifiers");
        return 0;
    }
    Hotkey* h = nullptr;
    {
        std::scoped_lock lk{g_mu};
        for (const Hotkey& o : g_hotkeys)
            if (o.mod == m && o.key == k->key) {
                log_line(m->name, std::string("add_hotkey: there's already a hotkey called ") + k->key);
                return 0;
            }
        h            = &g_hotkeys.emplace_back();
        h->mod       = m;
        h->key       = k->key;
        h->label     = k->label;
        h->on_press  = k->on_press;
        h->on_rebind = k->on_rebind;
        h->user      = k->user;
        h->vk        = k->vk;
        h->modifiers = k->vk ? k->modifiers : 0;
        h->chord     = k->pad_chord;
        // the rows' choices: Off, the mod's own binding if it's none of the usual ones, F1-F12, Ctrl+F1-F12
        h->keys.push_back({});
        const bool usual = !k->vk || (k->vk >= VK_F1 && k->vk <= VK_F12 && (h->modifiers == 0 || h->modifiers == FH2_KEY_CTRL));
        if (!usual) h->keys.push_back({k->vk, h->modifiers, 0});
        for (uint32_t mods : {0u, static_cast<uint32_t>(FH2_KEY_CTRL)})
            for (uint32_t f = VK_F1; f <= VK_F12; ++f) h->keys.push_back({f, mods, 0});
        for (const Binding& b : h->keys) h->key_names.push_back(binding_name(b.vk, b.modifiers));
        h->chords.push_back({});
        bool listed = !k->pad_chord;
        for (const ChordName& c : kChords) listed = listed || c.bits == k->pad_chord;
        if (!listed) h->chords.push_back({0, 0, k->pad_chord});
        for (const ChordName& c : kChords) h->chords.push_back({0, 0, c.bits});
        for (const Binding& b : h->chords) h->chord_names.push_back(chord_name(b.chord));
        start_poll();
        if (!g_xinput && !g_xinput_said) {
            g_xinput_said = true;
            plog("hotkeys: XInput isn't available, so controller chords are off");
        }
    }
    // the Mods page's rows, in the mod's own part
    auto row = [&](const std::string& key, const std::string& label, const std::vector<std::string>& names, uint32_t value) {
        std::vector<const char*> c;
        for (const std::string& n : names) c.push_back(n.c_str());
        FH2Setting s{};
        s.size         = sizeof s;
        s.key          = key.c_str();
        s.label        = label.c_str();
        s.description  = "A hotkey. Off turns it off; the game still gets the key or buttons as ever.";
        s.kind         = FH2_SETTING_CHOICE;
        s.choices      = c.data();
        s.choice_count = static_cast<uint32_t>(c.size());
        s.value        = static_cast<int>(value);
        s.on_change    = on_row;
        s.user         = h;
        settings_add(m, &s);
    };
    uint32_t kv = 0, cv = 0;
    for (std::size_t i = 0; i < h->keys.size(); ++i)
        if (h->keys[i].vk == h->vk && h->keys[i].modifiers == h->modifiers) kv = static_cast<uint32_t>(i);
    for (std::size_t i = 0; i < h->chords.size(); ++i)
        if (h->chords[i].chord == h->chord) cv = static_cast<uint32_t>(i);
    row("Hotkey_" + h->key, h->label + ": key", h->key_names, kv);
    row("HotkeyPad_" + h->key, h->label + ": controller", h->chord_names, cv);
    log_line(m->name, "hotkey " + h->key + " (" + h->label + "): " + binding_name(h->vk, h->modifiers) + ", controller " +
                          chord_name(h->chord));
    return 1;
}

void device_sweep_start() {
    std::scoped_lock lk{g_mu};
    g_sweep = true;
    start_poll();
}

int device_auto_kbd() { return g_auto_kbd.load(); }

// The rows' text, in the game's language now: before the pages are written.
void hotkeys_refresh_text() {
    struct Text {
        FH2Mod* mod;
        std::string key, label;
    };
    std::vector<Text> rows;
    {
        std::scoped_lock lk{g_mu};
        for (const Hotkey& h : g_hotkeys) rows.push_back({h.mod, h.key, h.label});
    }
    const std::string desc = tr("A hotkey. Off turns it off; the game still gets the key or buttons as ever.");
    for (const Text& r : rows) {
        settings_set_text(r.mod, ("Hotkey_" + r.key).c_str(), trf("{0}: key", {r.label}).c_str(), desc.c_str(), nullptr, 0);
        settings_set_text(r.mod, ("HotkeyPad_" + r.key).c_str(), trf("{0}: controller", {r.label}).c_str(), desc.c_str(), nullptr, 0);
    }
}

} // namespace fh2platform

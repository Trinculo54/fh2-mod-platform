// The HUD layer preview's state (hud_state.hpp). Every mod's calls stage a converted copy of what its
// layer shows; once a frame the pump (hud_layer.cpp, on the game's UI thread) runs HudEngine::frame,
// which picks the layer that owns the screen, works out what each of the page's properties should be
// (the targets), publishes the changed ones (Strings through the game) and tells the live page what
// changed, within a budget, in an order that never shows a part before its contents and never holds a
// hide back. Nothing here calls the game directly: HudGame does.
#include "hud_state.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace fh2platform {

// ---- text and key caps ------------------------------------------------------------------------------

bool hud_letter_ok(uint32_t c) {
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) || (c >= 0x104 && c <= 0x107) || c == 0x118 || c == 0x119 ||
           (c >= 0x141 && c <= 0x144) || c == 0x15A || c == 0x15B || c == 0x160 || c == 0x161 || (c >= 0x179 && c <= 0x17E) ||
           c == 0x2013 || c == 0x2014 || c == 0x2018 || c == 0x2019 || (c >= 0x201C && c <= 0x201E) || c == 0x2026;
}

namespace {

bool high_surrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool low_surrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

void cut(std::wstring& w, std::size_t limit, unsigned* flags) {
    if (w.size() <= limit) return;
    std::size_t n = limit;
    if (n > 0 && high_surrogate(w[n - 1])) --n;   // never half a pair
    w.resize(n);
    *flags |= kHudTextCut;
}

std::wstring upper(const std::wstring& w) {
    if (w.empty()) return w;
    std::wstring u(w.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(), static_cast<int>(w.size()), u.data(),
                                static_cast<int>(u.size()), nullptr, nullptr, 0);
    return n == static_cast<int>(w.size()) ? u : w;
}

void letters(const std::wstring& w, unsigned* flags, uint32_t* odd) {
    for (std::size_t i = 0; i < w.size(); ++i) {
        uint32_t c = w[i];
        if (high_surrogate(w[i]) && i + 1 < w.size() && low_surrogate(w[i + 1])) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(w[i + 1]) - 0xDC00);
            ++i;
        }
        if (!hud_letter_ok(c)) {
            if (!(*flags & kHudTextOdd) && odd) *odd = c;
            *flags |= kHudTextOdd;
            return;
        }
    }
}

bool ascii_is(const std::wstring& a, const wchar_t* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        wchar_t x = a[i], y = b[i];
        if (x >= L'A' && x <= L'Z') x = static_cast<wchar_t>(x - L'A' + L'a');
        if (y >= L'A' && y <= L'Z') y = static_cast<wchar_t>(y - L'A' + L'a');
        if (x != y) return false;
    }
    return i == a.size() && !b[i];
}

} // namespace

std::wstring hud_text(const char* utf8, std::size_t limit, bool caps, unsigned* flags, uint32_t* odd) {
    std::wstring w;
    if (utf8 && *utf8) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (n > 1) {
            w.assign(static_cast<std::size_t>(n - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
        }
    }
    for (wchar_t& c : w)
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029) {
            c = L' ';
            *flags |= kHudTextControl;
        }
    cut(w, limit, flags);
    if (caps) w = upper(w);
    letters(w, flags, odd);
    return w;
}

HudKeyCaps hud_key_caps(const std::wstring& key, unsigned* flags) {
    HudKeyCaps k;
    if (key.empty()) return k;
    k.given = true;
    std::size_t at = std::wstring::npos;
    for (std::size_t i = 1; i + 1 < key.size(); ++i)
        if (key[i] == L'+' || key[i] == L'/') {
            at = i;
            break;
        }
    const auto one = [flags](std::wstring s, std::wstring& text, int& angle) {
        static const struct {
            const wchar_t* name;
            int angle;
        } arrows[] = {{L"Up", 270}, {L"Down", 90}, {L"Left", 180}, {L"Right", 0}};
        for (const auto& a : arrows)
            if (ascii_is(s, a.name)) {
                text.clear();
                angle = a.angle;
                return;
            }
        cut(s, 12, flags);
        text  = upper(s);
        angle = -1;
    };
    if (at == std::wstring::npos) {
        one(key, k.k0, k.angle0);
    } else {
        one(key.substr(0, at), k.k0, k.angle0);
        k.join = key.substr(at, 1);
        one(key.substr(at + 1), k.k1, k.angle1);
        k.two = true;
    }
    return k;
}

// ---- the device -------------------------------------------------------------------------------------

const char* hud_pad_input(const HudPad& a, const HudPad& b) {
    if (a.buttons != b.buttons) return "buttons";
    const auto trigger = [](int x, int was) {
        const bool past = x > 30, was_past = was > 30;
        return past != was_past || (past && std::abs(x - was) >= 8);
    };
    const auto stick = [](int x, int y, int wx, int wy) {
        const bool past = std::abs(x) > 7849 || std::abs(y) > 7849, was_past = std::abs(wx) > 7849 || std::abs(wy) > 7849;
        return past != was_past || (past && (std::abs(x - wx) >= 1024 || std::abs(y - wy) >= 1024));
    };
    if (trigger(a.lt, b.lt) || trigger(a.rt, b.rt)) return "trigger";
    if (stick(a.lx, a.ly, b.lx, b.ly)) return "left stick";
    if (stick(a.rx, a.ry, b.rx, b.ry)) return "right stick";
    return nullptr;
}

bool HudMouseMoves::round(bool read, long nx, long ny) {
    const long dd = read && have ? std::abs(nx - x) + std::abs(ny - y) : 0;
    if (read) {
        x    = nx;
        y    = ny;
        have = true;
    }
    const bool counted = dd > 0 && run >= 1 && dd + last >= 3;
    run                = dd > 0 ? run + 1 : 0;
    last               = dd;
    return counted;
}

void HudMouseMoves::forget() {
    have = false;
    run  = 0;
    last = 0;
}

const char* HudKeyWatch::round(const bool* down, const uint32_t* hotkeys, std::size_t n_hotkeys, char* buf, std::size_t cap) {
    const bool win    = down[VK_LWIN] || down[VK_RWIN];
    const char* found = nullptr;
    for (int vk = 1; vk < 255; ++vk) {
        const bool now = down[vk];
        if (now && !was[vk] && !found && !win) {
            bool counts = (vk >= 0x01 && vk <= 0x06) || (vk >= 0x08 && vk <= 0xFE);
            switch (vk) {
                case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
                case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: counts = false; break;
                default: break;
            }
            for (std::size_t i = 0; i < n_hotkeys && counts; ++i)
                if (hotkeys[i] == static_cast<uint32_t>(vk)) counts = false;
            if (counts) {
                static const int kMouse[] = {0, 1, 2, 0, 3, 4, 5};   // VK_LBUTTON 1, RBUTTON 2, MBUTTON 4, XBUTTON1 5, XBUTTON2 6
                if (vk <= 6 && kMouse[vk]) std::snprintf(buf, cap, "mouse button %d", kMouse[vk]);
                else std::snprintf(buf, cap, "key 0x%02X", vk);
                found = buf;
            }
        }
        was[vk] = now;
    }
    return found;
}

bool HudDeviceJudge::round(const char* kb, const char* pad, double now) {
    if (!kb != !pad) {
        pending = kb ? 1 : 0;
        std::snprintf(cause, sizeof cause, "%s", kb ? kb : pad);
    }
    if (pending < 0) return false;
    if ((pending == 1) == kbd) {
        pending = -1;
        return false;
    }
    if (now - flipped_at < 150) return false;
    kbd        = pending == 1;
    pending    = -1;
    flipped_at = now;
    return true;
}

// ---- the runtime ------------------------------------------------------------------------------------

namespace {

constexpr int kMaxLayers         = FH2_HUD_MAX_LAYERS;
constexpr double kClearMs        = 250;    // a part going out: <P>Vis goes false after its fade
constexpr double kPulseMs        = 400;    // the shutter's and the countdown pop's switches stay true this long
constexpr double kTextMs         = 100;    // one text slot is published at most 10 times a second
constexpr double kRetireMs       = 2000;   // a retired String is let go after this at the latest
constexpr double kSoundLateMs    = 500;
constexpr double kSoundRepeatMs  = 100;
constexpr int kBudget            = 24;     // notifies a frame, besides the hides, the per-frame values and pops
constexpr std::size_t kRetiredMax = 256;
constexpr uint32_t kDefaultLease = 5000;
constexpr uint32_t kNoEpoch      = 0xFFFFFFFFu;

const char* const kSoundNames[12] = {"", "", "", "", "", "ERROR", "NOTIFY", "INFO", "TICK", "TICK_LOW", "TICK_END", "SHUTTER"};
bool world_only(uint32_t s) {
    return s == FH2_SOUND_NOTIFY || s == FH2_SOUND_TICK || s == FH2_SOUND_TICK_LOW || s == FH2_SOUND_TICK_END;
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[1024];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

uint32_t fbits(float f) {
    uint32_t b = 0;
    std::memcpy(&b, &f, 4);
    return b;
}
float bitsf(uint32_t b) {
    float f = 0;
    std::memcpy(&f, &b, 4);
    return f;
}

// What a layer shows, converted on the caller's thread.
struct PromptData {
    uint32_t glyph = 0;
    bool chord     = false;
    HudKeyCaps key;
    std::wstring label;
    bool hold = false;
};
struct MarkData {
    float at       = 0;
    int shape      = 0, style = 0;
    uint16_t number = 0;
};
struct ChipData {
    std::wstring text;
    int dots = 0, filled = 0;
};
struct RowData {
    std::wstring label, value;
    int style = 0;
};
struct Data {
    std::wstring tabs[8];
    int tab_count = 0;
    uint32_t tab  = 0;
    std::wstring tab_keys[2];
    bool tab_keys_given = false;
    std::wstring line1, line1_after, line2;
    int line1_after_style = 0, centre = 0;
    int strip = 0;
    bool strip_row = false, playhead = false;
    std::wstring strip_left, strip_right, strip_after;
    MarkData marks[16];
    int mark_count = 0;
    PromptData prompts[5];
    int prompt_count = 0;
    ChipData chips[5];   // [4] is the corner
    std::wstring tip_before, tip_after;
    PromptData tip;
    RowData rows[8];
    int row_count         = 0;
    uint32_t row_selected = 0;
    std::wstring footer;
    uint32_t countdown = 0;
    std::wstring countdown_label;
};

struct QSound {
    uint32_t sound;
    double at;
    int layer;
};

struct Rate {
    double start = -1e18;
    int n = 0, dropped = 0;
};

struct Layer {
    // set by add_layer before the count is raised, read only afterwards
    void* mod = nullptr;
    std::string name;
    uint32_t parts = 0, lease = kDefaultLease;
    FH2HudStatusFn on_status = nullptr;
    void* user               = nullptr;
    // staged by the mod's calls
    std::mutex mu;
    Data staged;                        // mu
    std::atomic<uint64_t> gen{0};       // bumped under mu
    std::atomic<uint32_t> playhead{0}, holdv{0};   // float bits
    std::atomic<uint64_t> show_seq{0};  // the newest show_hud_layer's sequence number
    std::atomic<uint64_t> parts_seq{0}; // the newest one with parts
    std::atomic<uint32_t> show_parts{0};
    std::atomic<uint32_t> epoch{0};     // show_hud_layer with parts, and hold_game_hud(1): LAPSED goes
    std::atomic<uint32_t> hold_epoch{0};
    std::atomic<int> wish{0};
    std::atomic<uint32_t> lapsed_epoch{kNoEpoch};   // LAPSED while it equals epoch
    std::atomic<uint32_t> wish_killed{kNoEpoch};    // the wish doesn't count while it equals hold_epoch
    std::atomic<bool> stopped{false};   // its mod was stopped for online play (stop_layer): lapsed at every frame
    std::atomic<uint32_t> pulses{0};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint32_t> status{0};
    std::mutex snd_mu;
    std::deque<QSound> sounds;          // snd_mu
    double sound_at[12] = {-1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18};   // snd_mu
    std::mutex log_mu;
    std::set<std::string> logged;       // log_mu
    Rate rates[3];                      // log_mu
    // the pump's own
    Data copy;
    uint64_t copy_gen = 0, seen_calls = 0, seen_show = 0;
    uint32_t seen_pulses = 0;
    std::atomic<uint64_t> killed{0};    // the parts_seq whose show the platform ended (pulse_hud reads it on any thread)
    uint64_t taken = 0;                 // the parts_seq another mod's show took the screen from
    double idle   = 0;
    bool fresh    = false;              // a call was seen this frame: the time before it isn't idle
    std::atomic<bool> status_off{false};   // its on_status crashed
    bool end_logged = false;            // the end of its show was logged already (lapse, crash, taken)
};

struct Prop {
    char type      = 'b';
    HudRole role   = HudRole::content;
    int part       = -1;
    uint32_t tgt   = 0;
    std::wstring tgt_text;
    std::atomic<uint32_t> pub{0};
    std::wstring pub_text;
    double text_at     = -1e18;
    uint64_t pub_frame = 0;   // the frame it was last published in (0: never: still its default)
    void* cache        = nullptr;   // the String for pub_text (str_lock); nullptr for "": the shared empty one
    bool told_valid    = true;
    uint32_t told      = 0;
    std::wstring told_text;
    uint64_t told_n = 0;
    bool dirty      = false;
    uint64_t seq    = 0;
};

struct Retired {
    void* s;
    int prop;
    uint64_t told_n;
    double at;
};

// The properties the targets set, by name (-1: not in this page).
struct PromptIx {
    int Shown = -1, Hold = -1, Kbd = -1, Chord = -1, G = -1, K0 = -1, K0Arrow = -1, K0Angle = -1, K1Shown = -1, Join = -1, K1 = -1,
        K1Arrow = -1, K1Angle = -1, Label = -1;
};
struct Ix {
    int Shown = -1, Shutter = -1, HoldWord = -1, HoldAngle = -1, HoldFill = -1, TabsShown = -1, TabKbd = -1, TabKeyL = -1, TabKeyR = -1;
    int Tab[8], TabShown[8], TabSel[8];
    int Alert = -1, L1 = -1, L1After = -1, L1Style = -1, L2 = -1, Dots = -1, InlineAfter = -1, StripRow = -1, Rail = -1, RailLeft = -1,
        RailRight = -1, RowAfter = -1, Playhead = -1, PlayheadX = -1;
    int Dot[8];
    int MarkShown[16], MarkX[16], MarkAngle[16], MarkStyle[16], MarkNum[16];
    PromptIx P[5], Tip;
    int TipBefore = -1, TipAfter = -1;
    int CShown[5], C[5], CD[5][5];
    int RShown[8], RSel[8], RLabel[8], RStyle[8], RHasValue[8], RValue[8];
    int PanelFooter = -1, CountText = -1, CountPop = -1, CountLabel = -1, CountLabelShown = -1;
    int FShown[kHudFeedLines], FHasName[kHudFeedLines], FName[kHudFeedLines], FText[kHudFeedLines], FStyle[kHudFeedLines];
    int fade[kHudParts], vis[kHudParts];
};

// A feed line: staged by post_feed, then the pump's own while it's on screen.
struct FeedLine {
    std::wstring name, text;
    int style     = 0;
    double remain = 0;   // ms of time with the HUD page live left
};
constexpr std::size_t kFeedStaged = 16;   // posted lines waiting for the pump; more drops the oldest

constexpr int kOff = 0, kIn = 1, kOut = 2;
constexpr int kPartBar = 0, kPartTip = 2, kPartPanel = 3, kPartCount = 4, kPartFeed = 5;   // FH2_HUD_PART_ bit order
static_assert(kPartFeed == kHudParts - 1, "the feed is the last part");
static_assert((1u << kPartFeed) == FH2_HUD_PART_FEED, "the feed's bit");
struct Part {
    bool declared = false;
    int state     = kOff;
    double clear  = 0;
    bool instant  = false;
};

} // namespace

struct HudEngine::Impl {
    HudEnv env;
    Layer layers[kMaxLayers];
    std::atomic<int> count{0};
    std::atomic<uint64_t> seq{0};
    std::atomic<HudOff> off{HudOff::none};
    std::atomic<bool> hold_ok{true}, sounds_ok{true}, live_now{false}, strings_fault{false};
    std::mutex once_mu;
    std::set<std::string> once_keys;   // once_mu: the platform's own
    // the table
    std::vector<HudProp> table;
    std::map<std::string, int> index;
    std::unique_ptr<Prop[]> p;
    int np = 0;
    Ix ix;
    // Strings
    SRWLOCK str_lock = SRWLOCK_INIT;
    std::atomic<void*> empty{nullptr};
    std::deque<Retired> retired;
    std::atomic<uint64_t> made{0};
    // Released by the platform but not freed: still held by the game. While the page is off screen (a menu,
    // the map) a binding keeps the String it showed until the page is told again, and frees it then, so
    // those are counted apart (off_n); left_n is what the game still held while the page was live.
    uint64_t freed_n = 0, left_n = 0, off_n = 0;
    // the pump's
    Part part[kHudParts];
    int owner_      = -1;
    uint64_t owner_seq = 0;
    uint32_t wanted = 0;
    bool force_instant = false;
    const void* told_page = nullptr;
    uint64_t told_gen     = 0;
    int target_owner = -2;
    uint64_t target_gen = 0;
    bool target_kbd = false, held_glyph = false, held_caps = false, playhead_on = false;
    uint32_t count_target = 0, count_shown = 0;
    bool pop_pending = false;
    double pop_until = 0;
    uint64_t count_in_frame = 0;   // the frame the countdown last came in: its first pop waits two frames
    bool shutter_req = false, shutter_again = false;
    double shutter_req_at = 0, shutter_until = 0;
    bool shown_told = false, ever_shown = false;
    uint64_t clean_from = 0, frame = 0, last_notify = 0;
    double now = 0, last_now = -1;
    int ndirty = 0;
    bool allowed[kHudParts] = {}, pending[kHudParts] = {};
    // the feed: posted lines (feed_mu), the pump's lines on screen (newest at the back), and whether it's kept
    // out of the way (a show_hud_layer with FH2_HUD_PART_INSTANT, until the next one without)
    std::mutex feed_mu;
    std::deque<FeedLine> feed_staged;   // feed_mu
    std::atomic<uint64_t> feed_posts{0};
    uint64_t feed_seen = 0;
    std::deque<FeedLine> feed;
    bool feed_changed = true, feed_quiet = false;
    std::deque<QSound> pq;
    uint32_t notified = 0;
    bool teardown_done = false, last_hide_done = false, off_status_done = false;
    Stats st;

    explicit Impl(HudEnv e) : env(std::move(e)) {}

    double clock() const { return env.clock ? env.clock() : 0.0; }
    void plog(const std::string& t) const {
        if (env.log) env.log("HUD layer", t);
    }
    void mlog(int layer, const std::string& t) const {
        if (env.log) env.log(layers[layer].name, t);
    }
    bool once_l(int layer, const std::string& key) {
        if (layer < 0) {
            std::scoped_lock lk{once_mu};
            return once_keys.insert(key).second;
        }
        Layer& L = layers[layer];
        std::scoped_lock lk{L.log_mu};
        return L.logged.insert(key).second;
    }
    bool off_now() const { return off.load() != HudOff::none; }

    // ---- properties ---------------------------------------------------------------------------------
    bool valid(int i) const { return i >= 0 && i < np; }
    bool pubb(int i) const { return valid(i) && p[i].pub.load(std::memory_order_relaxed) != 0; }
    void mark(int i) {
        Prop& q       = p[i];
        const bool dd = !q.told_valid || (q.type == 's' ? q.pub_text != q.told_text : q.pub.load(std::memory_order_relaxed) != q.told);
        if (dd && !q.dirty) {
            q.seq = frame;
            ++ndirty;
        } else if (!dd && q.dirty) {
            --ndirty;
        }
        q.dirty = dd;
    }
    // a switch (Shown, Shutter, CountPop, <P>, <P>Vis): published by the screen itself
    void set_switch(int i, bool v) {
        if (!valid(i) || pubb(i) == v) return;
        p[i].pub.store(v ? 1u : 0u, std::memory_order_relaxed);
        p[i].pub_frame = frame;
        mark(i);
    }
    void setb(int i, bool v) {
        if (valid(i)) p[i].tgt = v ? 1u : 0u;
    }
    void seti(int i, int v) {
        if (valid(i)) p[i].tgt = static_cast<uint32_t>(v);
    }
    void setf(int i, float v) {
        if (valid(i)) p[i].tgt = fbits(v);
    }
    void sets(int i, const std::wstring& v) {
        if (valid(i)) p[i].tgt_text = v;
    }

    // ---- Strings ------------------------------------------------------------------------------------
    // False if not published this frame (the rate limit, no memory, or a fault).
    bool publish_text(int i, const std::wstring& text, bool limited) {
        Prop& q = p[i];
        if (q.pub_text == text) return true;
        if (limited && now - q.text_at < kTextMs) return false;
        void* s = nullptr;
        if (!text.empty()) {
            if (!env.game->make_string(text.data(), static_cast<int>(text.size()), &s)) {
                fault_strings();
                return false;
            }
            if (!s) {
                if (once_l(-1, "no memory")) plog("the game gave no memory for a String; trying again next frame");
                return false;
            }
            ++made;
        }
        AcquireSRWLockExclusive(&str_lock);
        void* old = q.cache;
        q.cache   = s;
        ReleaseSRWLockExclusive(&str_lock);
        if (old) retired.push_back({old, i, q.told_n, now});
        q.pub_text  = text;
        q.text_at   = now;
        q.pub_frame = frame;
        mark(i);
        return true;
    }
    void fault_strings() {
        strings_fault = true;
        HudOff none   = HudOff::none;
        off.compare_exchange_strong(none, HudOff::strings_fault);
    }
    bool release(void* s) {
        bool freed = false;
        if (!env.game->release(s, &freed)) {
            fault_strings();
            return false;
        }
        if (freed) ++freed_n;
        else if (live_now.load()) ++left_n;
        else ++off_n;
        return true;
    }
    // A retired String is let go once its slot has been told since and the game holds no reference of its
    // own on it (we still hold ours, so its count is safe to read), or after kRetireMs. In demo run 1,
    // letting go as soon as the slot was told counted most Strings as "left with the game": the game drops
    // its reference a little after the notify, so waiting for that tells a real leak from timing.
    bool retire_due(const Retired& r) {
        if (now - r.at >= kRetireMs) return true;
        return p[r.prop].told_n > r.told_n && env.game->refs(r.s) <= 1;
    }
    void retire() {
        while (!retired.empty() && !strings_fault.load()) {
            const Retired& r = retired.front();
            const bool due   = retired.size() > kRetiredMax || retire_due(r);
            if (!due) {
                // the oldest isn't due: look for later ones that are (told since), keeping the order of the rest
                break;
            }
            void* s = r.s;
            retired.pop_front();
            if (!release(s)) return;
        }
        // later entries whose slot has been told since
        for (auto it = retired.begin(); it != retired.end() && !strings_fault.load();) {
            if (retire_due(*it)) {
                void* s = it->s;
                it      = retired.erase(it);
                if (!release(s)) return;
            } else {
                ++it;
            }
        }
    }
    void teardown() {
        std::vector<void*> all;
        AcquireSRWLockExclusive(&str_lock);
        for (int i = 0; i < np; ++i)
            if (p[i].cache) {
                all.push_back(p[i].cache);
                p[i].cache = nullptr;
            }
        ReleaseSRWLockExclusive(&str_lock);
        for (const Retired& r : retired) all.push_back(r.s);
        retired.clear();
        for (void* s : all)
            if (!release(s)) return;
    }
    void* make_empty() {
        void* s = nullptr;
        if (!env.game->make_string(L"", 0, &s)) {
            fault_strings();
            return nullptr;
        }
        if (!s) return nullptr;
        void* none = nullptr;
        if (empty.compare_exchange_strong(none, s)) {
            env.game->pin(s);
            ++made;
            return s;
        }
        bool freed = false;
        env.game->release(s, &freed);
        return none;
    }

    // ---- telling the page ---------------------------------------------------------------------------
    bool tell(int i) {
        if (!env.game->notify(i)) {
            HudOff none = HudOff::none;
            off.compare_exchange_strong(none, HudOff::notify_fault);
            return false;
        }
        Prop& q      = p[i];
        q.told_valid = true;
        if (q.type == 's') q.told_text = q.pub_text;
        else q.told = q.pub.load(std::memory_order_relaxed);
        ++q.told_n;
        if (q.dirty) {
            q.dirty = false;
            --ndirty;
        }
        ++notified;
        last_notify = frame;
        if (i == ix.Shown) {
            if (q.told) {
                shown_told = ever_shown = true;
            } else {
                shown_told = false;
                clean_from = frame + 3;
            }
        }
        // the pop right after its number, outside the budget; a number told while the part is still coming in
        // pops once the part's Visibility and fade are told (notify step d), not on a collapsed ring
        if (i == ix.CountText && pop_pending && part[kPartCount].state != kOff && !start_pop()) return false;
        return true;
    }
    bool start_pop() {
        pop_pending = false;
        set_switch(ix.CountPop, true);
        if (!tell(ix.CountPop)) return false;
        pop_until = now + kPulseMs;
        return true;
    }

    // The tip and the panel sit on the bar's live height (its strip row included). While either fades out,
    // the bar's height is kept as it is (the strip row's switch, the bar coming in, the bar collapsing), so
    // a card that is fading out never jumps; the change follows within the fade's 250 ms.
    bool card_fading() const { return part[kPartTip].state == kOut || part[kPartPanel].state == kOut; }

    // A new page: the switches are told again, hides first; content the platform ever published too
    // (what was never published is still the default the page read).
    void refresh() {
        for (Part& pt : part) {
            pt.state   = kOff;
            pt.instant = false;
        }
        set_switch(ix.Shown, false);
        set_switch(ix.Shutter, false);
        set_switch(ix.CountPop, false);
        for (int k = 0; k < kHudParts; ++k) {
            set_switch(ix.fade[k], false);
            set_switch(ix.vis[k], false);
        }
        shutter_again = false;
        shutter_until = 0;
        pop_until     = 0;
        for (int i = 0; i < np; ++i) {
            Prop& q = p[i];
            const bool sw = q.role == HudRole::part_fade || q.role == HudRole::part_vis || q.role == HudRole::root ||
                            q.role == HudRole::shutter || q.role == HudRole::count_pop;
            q.told_valid = !sw && q.pub_frame == 0;
            mark(i);
        }
        if (ever_shown) shown_told = true;   // what the page read is unknown: not CLEAN until Shown is told false
    }

    // ---- the frame's steps --------------------------------------------------------------------------
    bool wished(const Layer& L) const { return L.wish.load() != 0 && L.wish_killed.load() != L.hold_epoch.load(); }
    // A stopped layer's (stop_layer) doesn't count from the moment it's stopped; stops() marks it lapsed.
    bool wish_counts(const Layer& L) const { return wished(L) && !L.stopped.load(); }

    void collect(int n) {
        for (int i = 0; i < n; ++i) {
            Layer& L = layers[i];
            const uint64_t calls = L.calls.load();
            if (calls != L.seen_calls) {
                L.seen_calls = calls;
                L.idle       = 0;
                L.fresh      = true;
            }
            if (L.gen.load() != L.copy_gen) {
                if (L.mu.try_lock()) {
                    L.copy     = L.staged;
                    L.copy_gen = L.gen.load();
                    L.mu.unlock();
                } else {
                    ++st.skipped;
                }
            }
            std::scoped_lock lk{L.snd_mu};
            while (!L.sounds.empty()) {
                pq.push_back(L.sounds.front());
                L.sounds.pop_front();
            }
        }
        if (pq.size() > 1)
            std::stable_sort(pq.begin(), pq.end(), [](const QSound& a, const QSound& b) { return a.at < b.at; });
    }

    bool candidate(const Layer& L, uint64_t* ps_out) const {
        const uint64_t s = L.show_seq.load(std::memory_order_acquire), ps = L.parts_seq.load();
        *ps_out          = ps;
        return s != 0 && s == ps && (L.show_parts.load() & FH2_HUD_PART_ALL & L.parts) != 0 && L.killed != ps;
    }

    void choose_owner(int n) {
        int best = -1;
        uint64_t best_seq = 0, newest = 0;
        int newest_layer = -1;
        for (int i = 0; i < n; ++i) {
            Layer& L         = layers[i];
            const uint64_t s = L.show_seq.load(std::memory_order_acquire);
            if (s != L.seen_show) {
                L.seen_show = s;
                if (s > newest) newest = s, newest_layer = i;
            }
            uint64_t ps = 0;
            if (candidate(L, &ps) && ps > best_seq) best = i, best_seq = ps;
        }
        for (int i = 0; i < n; ++i) {   // another mod's newer show takes the screen
            Layer& L    = layers[i];
            uint64_t ps = 0;
            if (i == best || !candidate(L, &ps) || ps >= best_seq) continue;
            L.killed     = ps;
            L.taken      = ps;
            L.end_logged = true;
            if (owner_ == i || ps > 0) rated(i, 1, "HUD layer: " + layers[best].name + " showed its layer, so this one is hidden");
        }
        const uint32_t want = best >= 0 ? layers[best].show_parts.load() & FH2_HUD_PART_ALL & layers[best].parts : 0;
        const bool inst     = force_instant || (newest_layer >= 0 && (layers[newest_layer].show_parts.load() & FH2_HUD_PART_INSTANT));
        if (newest_layer >= 0) feed_quiet = (layers[newest_layer].show_parts.load() & FH2_HUD_PART_INSTANT) != 0;
        force_instant       = false;
        if (best != owner_ || (best >= 0 && best_seq != owner_seq)) {
            if (best >= 0) {
                layers[best].end_logged = false;
                rated(best, 1, fmt("HUD layer shown: %s at frame %llu", hud_parts_text(want).c_str(), static_cast<unsigned long long>(frame)));
            } else if (owner_ >= 0 && !layers[owner_].end_logged) {
                layers[owner_].end_logged = true;
                if (inst)
                    rated(owner_, 1, fmt("HUD layer hidden at once at frame %llu; clean from frame %llu", static_cast<unsigned long long>(frame),
                                         static_cast<unsigned long long>(frame + 3)));
                else
                    rated(owner_, 1, fmt("HUD layer hidden at frame %llu (fading)", static_cast<unsigned long long>(frame)));
            }
        }
        for (int k = 0; k < kHudParts; ++k) {
            const uint32_t b = 1u << k;
            if (want & b) part[k].instant = false;
            else if (inst) part[k].instant = true;
        }
        owner_    = best;
        owner_seq = best_seq;
        wanted    = want;
    }

    // A layer shown or holding with no call for its lease: hidden (with a fade), its wish removed, LAPSED.
    // The frame that sees a call adds nothing: the time since the frame before came (partly) before the
    // call, so counting it would lapse a layer up to a frame early.
    bool leases(const FrameIn& in, int n, double dt) {
        bool ended = false;
        for (int i = 0; i < n; ++i) {
            Layer& L         = layers[i];
            const bool fresh = L.fresh;
            L.fresh          = false;
            const bool shown = owner_ == i && wanted != 0;
            const bool wish  = wish_counts(L);
            if (!shown && !wish) continue;
            if (in.live && !fresh) L.idle += dt;
            if (L.idle < static_cast<double>(L.lease)) continue;
            if (shown) {
                L.killed     = L.parts_seq.load();
                L.end_logged = true;
                ended        = true;
            }
            L.lapsed_epoch.store(L.epoch.load());
            L.wish_killed.store(L.hold_epoch.load());
            mlog(i, fmt("HUD layer: no call for %.1f s while on screen, so it's hidden and its hold on the game's HUD is let go", L.idle / 1000.0));
            L.idle = 0;
        }
        return ended;
    }

    // A layer whose mod was stopped for online play (stop_layer): lapsed at once, on screen or not, whatever the
    // mod still calls. hud_status keeps a lease, but a stopped mod can only hide its layer and let go of the hold.
    void stops(int n) {
        for (int i = 0; i < n; ++i) {
            Layer& L = layers[i];
            if (!L.stopped.load()) continue;
            uint64_t ps     = 0;
            const bool show = candidate(L, &ps);
            const bool wish = wished(L);
            if (!show && !wish) continue;
            if (show) {
                L.killed     = ps;
                L.end_logged = true;
            }
            L.lapsed_epoch.store(L.epoch.load());
            L.wish_killed.store(L.hold_epoch.load());
            L.idle = 0;
            mlog(i, "HUD layer: this mod was stopped for online play, so its layer is hidden and its hold on the game's HUD is let go");
        }
    }

    void targets(const Data& s, bool kbd) {
        // tabs
        for (int i = 0; i < 8; ++i) {
            const bool on = i < s.tab_count;
            sets(ix.Tab[i], on ? s.tabs[i] : std::wstring());
            setb(ix.TabShown[i], on);
            setb(ix.TabSel[i], on && static_cast<uint32_t>(i) == s.tab);
        }
        setb(ix.TabsShown, s.tab_count > 0);
        setb(ix.TabKbd, kbd && s.tab_keys_given);
        sets(ix.TabKeyL, s.tab_keys[0]);
        sets(ix.TabKeyR, s.tab_keys[1]);
        // the middle
        sets(ix.L1, s.line1);
        sets(ix.L1After, s.line1_after);
        seti(ix.L1Style, s.line1_after_style);
        sets(ix.L2, s.line2);
        setb(ix.Alert, s.centre == FH2_HUD_CENTRE_ALERT);
        // the strip
        const bool dots = s.strip == FH2_HUD_STRIP_DOTS && !s.strip_row;
        const bool rail = s.strip == FH2_HUD_STRIP_RAIL;
        const bool row  = rail || (s.strip == FH2_HUD_STRIP_DOTS && s.strip_row);
        setb(ix.Dots, dots);
        const int nd = std::min(s.mark_count, 8);
        for (int j = 0; j < 8; ++j) seti(ix.Dot[j], dots && j < nd ? s.marks[j].style + 1 : 0);
        sets(ix.InlineAfter, dots ? s.strip_after : std::wstring());
        setb(ix.StripRow, row);
        setb(ix.Rail, rail);
        sets(ix.RailLeft, rail ? s.strip_left : std::wstring());
        sets(ix.RailRight, rail ? s.strip_right : std::wstring());
        sets(ix.RowAfter, row ? s.strip_after : std::wstring());
        const int nm = row ? std::min(s.mark_count, 16) : 0;
        for (int k = 0; k < 16; ++k) {
            setb(ix.MarkShown[k], k < nm);
            if (k >= nm) continue;   // a mark past the count keeps its other values (not told)
            const MarkData& m = s.marks[k];
            float x           = 0;
            if (rail) {
                const float at = std::isnan(m.at) ? 0.0f : std::clamp(m.at, 0.0f, 1.0f);
                x              = at * 1104.0f - 20.0f;
            } else {
                // dots centred on the row, which is the screen's middle: the timeline's column starts at x 204,
                // so its x 756 is the page's 960
                x = 756.0f + (static_cast<float>(k) - static_cast<float>(nm - 1) / 2.0f) * 24.0f - 20.0f;
            }
            setf(ix.MarkX[k], x);
            setf(ix.MarkAngle[k], rail && m.shape == FH2_HUD_MARK_DIAMOND ? 45.0f : 0.0f);
            seti(ix.MarkStyle[k], m.style);
            sets(ix.MarkNum[k], rail && m.number ? std::to_wstring(m.number) : std::wstring());
        }
        playhead_on = rail && s.playhead;
        setb(ix.Playhead, playhead_on);
        // the prompts
        held_glyph = held_caps = false;
        const auto prompt = [&](const PromptIx& r, const PromptData& q, bool full) {
            const uint32_t g = q.glyph;
            seti(r.G, static_cast<int>(g));
            setb(r.Chord, q.chord && g != 0);
            sets(r.K0, q.key.k0);
            setb(r.K0Arrow, q.key.angle0 >= 0);
            setf(r.K0Angle, static_cast<float>(std::max(q.key.angle0, 0)));
            setb(r.K1Shown, q.key.two);
            sets(r.Join, q.key.join);
            sets(r.K1, q.key.k1);
            setb(r.K1Arrow, q.key.angle1 >= 0);
            setf(r.K1Angle, static_cast<float>(std::max(q.key.angle1, 0)));
            const bool caps = q.key.given && (kbd || g == 0);
            setb(r.Kbd, caps);
            if (!full) return;
            setb(r.Hold, q.hold);
            sets(r.Label, q.label);
            if (q.hold) (caps ? held_caps : held_glyph) = true;
        };
        const int npr = std::min(s.prompt_count, 5);
        for (int i = 0; i < 5; ++i) {
            setb(ix.P[i].Shown, i < npr);
            if (i < npr) prompt(ix.P[i], s.prompts[i], true);
        }
        // the chips
        for (int i = 0; i < 5; ++i) {
            const ChipData& c = s.chips[i];
            setb(ix.CShown[i], !c.text.empty() || c.dots > 0);
            sets(ix.C[i], c.text);
            for (int j = 0; j < 5; ++j) seti(ix.CD[i][j], j < c.dots ? (j < c.filled ? 2 : 1) : 0);
        }
        // the tip
        sets(ix.TipBefore, s.tip_before);
        sets(ix.TipAfter, s.tip_after);
        prompt(ix.Tip, s.tip, false);
        // the panel
        const int nr = std::min(s.row_count, 8);
        for (int r = 0; r < 8; ++r) {
            setb(ix.RShown[r], r < nr);
            if (r >= nr) continue;
            const RowData& w = s.rows[r];
            sets(ix.RLabel[r], w.label);
            seti(ix.RStyle[r], w.style == FH2_ROW_DIM ? 1 : w.style == FH2_ROW_HEADING ? 2 : 0);
            setb(ix.RHasValue[r], !w.value.empty());
            sets(ix.RValue[r], w.value);
            setb(ix.RSel[r], static_cast<uint32_t>(r) == s.row_selected && w.style != FH2_ROW_HEADING);
        }
        sets(ix.PanelFooter, s.footer);
        // the countdown (CountText through countdown())
        count_target = s.countdown;
        sets(ix.CountLabel, s.countdown_label);
        setb(ix.CountLabelShown, !s.countdown_label.empty());
    }

    // The feed, once a frame after the owner is chosen: takes the posted lines, ages the lines while the page
    // is live, drops the expired, and adds the feed to what's wanted while it has lines and isn't kept out of
    // the way. The bottom slot (F0) is the newest line.
    void feed_step(const FrameIn& in, double dt) {
        if (!part[kPartFeed].declared) return;
        const uint64_t posts = feed_posts.load();
        if (posts != feed_seen) {
            std::unique_lock lk{feed_mu, std::try_to_lock};
            if (lk.owns_lock()) {
                feed_seen = posts;
                while (!feed_staged.empty()) {
                    feed.push_back(std::move(feed_staged.front()));
                    feed_staged.pop_front();
                    if (feed.size() > static_cast<std::size_t>(kHudFeedLines)) feed.pop_front();
                    feed_changed = true;
                }
            }
        }
        if (in.live && dt > 0)
            for (FeedLine& f : feed) f.remain -= dt;
        while (!feed.empty() && feed.front().remain <= 0) {
            feed.pop_front();
            feed_changed = true;
        }
        const bool covered = (wanted & (FH2_HUD_PART_BAR | FH2_HUD_PART_PANEL)) != 0 || in.game_hidden || feed_quiet;
        if (!feed.empty() && !covered) {
            wanted |= FH2_HUD_PART_FEED;
        } else if (feed_quiet && part[kPartFeed].state != kOff) {
            part[kPartFeed].instant = true;   // a clean frame: out at once, as the owner's parts go
        }
        if (!feed_changed) return;
        feed_changed  = false;
        const int n   = static_cast<int>(feed.size());
        for (int i = 0; i < kHudFeedLines; ++i) {
            const bool on = i < n;
            setb(ix.FShown[i], on);
            if (!on) continue;   // a slot going out keeps its words (not told)
            const FeedLine& f = feed[static_cast<std::size_t>(n - 1 - i)];
            setb(ix.FHasName[i], !f.name.empty());
            sets(ix.FName[i], f.name);
            sets(ix.FText[i], f.text);
            seti(ix.FStyle[i], f.style);
        }
    }

    void frame_values(float playhead, float hold) {
        if (std::isnan(playhead)) playhead = 0;
        if (std::isnan(hold)) hold = 0;
        if (playhead_on) setf(ix.PlayheadX, std::clamp(playhead, 0.0f, 1.0f) * 1104.0f - 1.5f);
        if (held_glyph) setf(ix.HoldAngle, std::clamp(hold, 0.0f, 1.0f) * 360.0f);
        if (held_caps) setf(ix.HoldFill, std::clamp(hold, 0.0f, 1.0f));
    }

    // the parts, the shutter and the root, acted on only while the page is live. The bar comes last, so its
    // collapse can wait for a tip or panel that is fading out above it (card_fading) and still go in the
    // frame that card clears.
    void screen() {
        bool any = false;
        static const int kOrder[kHudParts] = {1, 2, 3, 4, kPartFeed, kPartBar};
        for (const int k : kOrder) {
            Part& pt = part[k];
            if (!pt.declared) continue;
            const bool want = (wanted & (1u << k)) != 0;
            if (want) {
                if (pt.state == kOut) {   // wanted again before its clear time: in place
                    set_switch(ix.fade[k], true);
                    pt.state = kIn;
                }
            } else if (pt.state == kIn) {
                set_switch(ix.fade[k], false);
                if (pt.instant) {
                    set_switch(ix.vis[k], false);
                    pt.state = kOff;
                } else {
                    pt.clear = now + kClearMs;
                    pt.state = kOut;
                }
            } else if (pt.state == kOut && (pt.instant || (now >= pt.clear && !(k == kPartBar && card_fading())))) {
                set_switch(ix.vis[k], false);
                pt.state = kOff;
            }
            if (pt.state == kOff) pt.instant = false;
            if (pt.state != kOff) any = true;
        }
        // the shutter: true, then false 400 ms later; a pulse while it's true goes false now and true next frame
        const auto start_shutter = [this] {
            if (!pubb(ix.Shown)) set_switch(ix.Shown, true);   // never a blink on a collapsed root
            set_switch(ix.Shutter, true);
            shutter_until = now + kPulseMs;
        };
        if (shutter_again) {
            shutter_again = false;
            start_shutter();
        } else if (shutter_req) {
            shutter_req = false;
            if (pubb(ix.Shutter)) {
                set_switch(ix.Shutter, false);
                shutter_again = true;
            } else {
                start_shutter();
            }
        } else if (pubb(ix.Shutter) && now >= shutter_until) {
            set_switch(ix.Shutter, false);
        }
        if (!any && !pubb(ix.Shutter) && !shutter_again) set_switch(ix.Shown, false);
    }

    // The countdown's number and its pop.
    void countdown() {
        const int ct = ix.CountText;
        if (!valid(ct)) return;
        if (pubb(ix.CountPop) && now >= pop_until) set_switch(ix.CountPop, false);
        if (!allowed[kPartCount]) return;
        const uint32_t m = std::min<uint32_t>(count_target, 999);
        if (m == count_shown) return;
        if (m == 0) {
            if (publish_text(ct, std::wstring(), false)) count_shown = 0, pop_pending = false;
        } else if (pubb(ix.CountPop)) {
            set_switch(ix.CountPop, false);   // told this frame; the number and its pop next frame
        } else if (publish_text(ct, std::to_wstring(m), true)) {
            count_shown = m;
            pop_pending = true;
        }
        if (m != count_shown) pending[kPartCount] = true;
    }

    void publish() {   // also the countdown's number (countdown())
        for (int k = 0; k < kHudParts; ++k) {
            allowed[k] = part[k].declared && (wanted & (1u << k)) && part[k].state != kOut;
            pending[k] = false;
        }
        const bool hold_row = card_fading();   // the strip row's switch waits (not pending: the bar isn't held back)
        for (int i = 0; i < np && !off_now(); ++i) {
            Prop& q = p[i];
            if (q.role != HudRole::content && q.role != HudRole::kbd && q.role != HudRole::frame_value) continue;
            if (i == ix.CountText || q.part < 0 || !allowed[q.part]) continue;
            if (i == ix.StripRow && hold_row) continue;
            if (q.type == 's') {
                if (q.tgt_text != q.pub_text && !publish_text(i, q.tgt_text, true)) pending[q.part] = true;
            } else if (q.tgt != q.pub.load(std::memory_order_relaxed)) {
                q.pub.store(q.tgt, std::memory_order_relaxed);
                q.pub_frame = frame;
                mark(i);
            }
        }
        if (!off_now()) countdown();
    }

    bool content(const Prop& q) const { return q.role == HudRole::content || q.role == HudRole::kbd; }

    // Dirty content of the parts in `mask`, the other content first, then the key caps switches; oldest
    // change first and in template order within a change.
    void dirty_content(uint32_t mask, std::vector<int>& out) const {
        out.clear();
        for (int i = 0; i < np; ++i) {
            const Prop& q = p[i];
            if (q.dirty && content(q) && q.part >= 0 && (mask & (1u << q.part))) out.push_back(i);
        }
        std::stable_sort(out.begin(), out.end(), [this](int a, int b) {
            const bool ka = p[a].role == HudRole::kbd, kb = p[b].role == HudRole::kbd;
            if (ka != kb) return kb;
            return p[a].seq < p[b].seq;
        });
    }

    // False if a notify faulted.
    bool notify(bool* carried_any) {
        // a: hides, uncapped: the root, the parts' fades and Visibility, the pop; then the shutter
        if (valid(ix.Shown) && p[ix.Shown].dirty && !pubb(ix.Shown) && !tell(ix.Shown)) return false;
        for (int k = 0; k < kHudParts; ++k)
            if (valid(ix.fade[k]) && p[ix.fade[k]].dirty && !pubb(ix.fade[k]) && !tell(ix.fade[k])) return false;
        for (int k = 0; k < kHudParts; ++k)
            if (valid(ix.vis[k]) && p[ix.vis[k]].dirty && !pubb(ix.vis[k]) && !tell(ix.vis[k])) return false;
        if (valid(ix.CountPop) && p[ix.CountPop].dirty && !pubb(ix.CountPop) && !tell(ix.CountPop)) return false;
        if (valid(ix.Shutter) && p[ix.Shutter].dirty) {
            if (pubb(ix.Shutter) && p[ix.Shown].dirty && pubb(ix.Shown) && !tell(ix.Shown)) return false;
            if (!tell(ix.Shutter)) return false;
        }
        for (int k = 0; k < kHudParts; ++k)   // a part wanted again while it was going out
            if (valid(ix.fade[k]) && part[k].state == kIn && p[ix.fade[k]].dirty && pubb(ix.fade[k]) && !tell(ix.fade[k])) return false;
        // b: the per-frame values, uncapped
        uint32_t on = 0, coming = 0;
        for (int k = 0; k < kHudParts; ++k) {
            if (part[k].state != kOff) on |= 1u << k;
            else if (part[k].declared && (wanted & (1u << k))) coming |= 1u << k;
        }
        for (const int i : {ix.PlayheadX, ix.HoldAngle, ix.HoldFill})
            if (valid(i) && p[i].dirty && ((on | coming) & (1u << p[i].part)) && !tell(i)) return false;
        // c: the content of parts in or going out, within the budget
        int budget = kBudget;
        std::vector<int> c;
        dirty_content(on, c);
        for (int i : c) {
            if (budget == 0) break;
            if (!tell(i)) return false;
            --budget;
        }
        // d: parts coming in: their content, then (once all of it is told) the root, <P>Vis and <P>; the bar
        // waits while a tip or panel above it fades out (card_fading)
        for (int k = 0; k < kHudParts; ++k) {
            if (!(coming & (1u << k))) continue;
            dirty_content(1u << k, c);
            for (int i : c) {
                if (budget == 0) break;
                if (!tell(i)) return false;
                --budget;
            }
            dirty_content(1u << k, c);
            if (!c.empty() || pending[k] || (k == kPartBar && card_fading())) continue;
            if (!pubb(ix.Shown)) set_switch(ix.Shown, true);
            if (p[ix.Shown].dirty && !tell(ix.Shown)) return false;
            set_switch(ix.vis[k], true);
            if (p[ix.vis[k]].dirty && !tell(ix.vis[k])) return false;
            set_switch(ix.fade[k], true);
            if (p[ix.fade[k]].dirty && !tell(ix.fade[k])) return false;
            part[k].state = kIn;
            if (k == kPartCount) count_in_frame = frame;   // its first number pops at step e
        }
        // e: the first number's pop, two frames after the ring came in, once the page has laid it out (in demo
        // run 1 a pop told in the frame the ring came in didn't run)
        if (pop_pending && part[kPartCount].state == kIn && frame >= count_in_frame + 2 && valid(ix.CountText) &&
            !p[ix.CountText].dirty && !start_pop())
            return false;
        // f: what's left waits for the next frame
        dirty_content(on | coming, c);
        st.carried += c.size();
        *carried_any = !c.empty();
        return true;
    }

    uint32_t bits_for(int i, const FrameIn& in, bool on) const {
        const Layer& L = layers[i];
        uint32_t b     = 0;
        if (on && in.available) b |= FH2_HUD_AVAILABLE;
        if (on && owner_ == i && wanted) b |= FH2_HUD_SHOWN;
        if (in.live) b |= FH2_HUD_ON_SCREEN;
        if (!shown_told && frame >= clean_from) b |= FH2_HUD_CLEAN;
        if (in.kbd) b |= FH2_HUD_KEYBOARD;
        if (in.game_hidden) b |= FH2_HUD_GAME_HIDDEN;
        if (L.taken && L.taken == L.parts_seq.load()) b |= FH2_HUD_TAKEN;
        if (in.device_forced) b |= FH2_HUD_DEVICE_FORCED;
        if (L.lapsed_epoch.load() == L.epoch.load()) b |= FH2_HUD_LAPSED;
        return b;
    }

    void statuses(const FrameIn& in, int n, bool on) {
        for (int i = 0; i < n; ++i) {
            Layer& L        = layers[i];
            const uint32_t b = bits_for(i, in, on);
            if (b == L.status.load()) continue;
            L.status.store(b);
            if (!L.on_status || L.status_off || !env.call_status) continue;
            uint32_t code = 0;
            if (env.call_status(L.on_status, L.user, b, &code)) continue;
            L.status_off = true;
            L.killed     = L.parts_seq.load();
            L.end_logged = true;
            if (owner_ == i) force_instant = true;
            L.wish_killed.store(L.hold_epoch.load());
            mlog(i, fmt("HUD layer: its on_status crashed (exception 0x%08X); on_status is off, the layer hidden and its hold let go", code));
        }
    }

    void sounds(const FrameIn& in, FrameOut& out) {
        if (!sounds_ok.load()) {
            for (const QSound& q : pq)
                if (once_l(q.layer, "sounds unavailable"))
                    mlog(q.layer, "play_ui_sound: the game's sound code isn't as expected, or sounds are off after a crash; not played");
            pq.clear();
            return;
        }
        while (!pq.empty()) {
            const QSound q = pq.front();
            pq.pop_front();
            if (now - q.at > kSoundLateMs) {
                if (once_l(q.layer, std::string("late ") + kSoundNames[q.sound]))
                    mlog(q.layer, fmt("sound: %s dropped: the game's frame didn't come within 0.5 s", kSoundNames[q.sound]));
                continue;
            }
            if (world_only(q.sound) && !in.live) {
                if (once_l(q.layer, std::string("gone ") + kSoundNames[q.sound]))
                    mlog(q.layer, fmt("sound: %s dropped: the free-roam HUD page went off screen before it could play", kSoundNames[q.sound]));
                continue;
            }
            out.sound       = static_cast<int>(q.sound);
            out.sound_layer = q.layer;
            return;
        }
    }

    // once the layer is off: the String teardown, the last hide, and one status change.
    void off_frame(const FrameIn& in, int n) {
        const HudOff why = off.load();
        if (!teardown_done) {
            teardown_done = true;
            pq.clear();
            if (why != HudOff::notify_fault && why != HudOff::strings_fault && !strings_fault.load()) teardown();
        }
        if (!last_hide_done) {
            last_hide_done = true;
            if ((why == HudOff::value_box_fault || why == HudOff::strings_fault) && in.live && in.page == told_page && in.page_gen == told_gen &&
                shown_told &&
                valid(ix.Shown)) {
                set_switch(ix.Shown, false);
                if (env.game->notify(ix.Shown)) {
                    shown_told = false;
                    clean_from = frame + 3;
                }
            }
        }
        if (!off_status_done) {
            off_status_done = true;
            owner_          = -1;
            wanted          = 0;
            statuses(in, n, false);
        }
    }

    void rated(int layer, int kind, const std::string& text) {
        Layer& L = layers[layer];
        const double t = clock();
        std::string before;
        bool write = false;
        {
            std::scoped_lock lk{L.log_mu};
            Rate& r = L.rates[kind];
            if (t - r.start >= 10000) {
                if (r.dropped) before = fmt("%d more line(s) like the ones above weren't written in those 10 s", r.dropped);
                r.start   = t;
                r.n       = 0;
                r.dropped = 0;
            }
            if (r.n < 20) {
                ++r.n;
                write = true;
            } else {
                ++r.dropped;
            }
        }
        if (!before.empty()) mlog(layer, before);
        if (write) mlog(layer, text);
    }

    void call(Layer& L) { L.calls.fetch_add(1, std::memory_order_relaxed); }
};

// ---- HudEngine --------------------------------------------------------------------------------------

HudEngine::HudEngine(HudEnv env) : d_(new Impl(std::move(env))) {}
HudEngine::~HudEngine() = default;

const char* HudEngine::sound_name(uint32_t sound) { return sound < 12 ? kSoundNames[sound] : ""; }

int HudEngine::add_layer(void* mod, const std::string& name, const FH2HudLayer* l) {
    Impl& d            = *d_;
    const auto refuse = [&](const std::string& why) {
        if (d.env.log) d.env.log(name, "add_hud_layer: " + why + "; no HUD layer");
        return -1;
    };
    if (!l || l->size < sizeof(FH2HudLayer)) return refuse("its FH2HudLayer is too small: set size = sizeof(FH2HudLayer)");
    if (l->parts & ~kHudPartsKnown && d.env.log) d.env.log(name, fmt("add_hud_layer: unknown part bits 0x%X; ignored", l->parts & ~kHudPartsKnown));
    const uint32_t parts = l->parts & kHudPartsKnown;
    if (!parts) return refuse("it asks for no parts");
    const int n = d.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (d.layers[i].mod == mod) return refuse("this mod already has one");
    if (n >= kMaxLayers) return refuse("four mods already have one");
    uint32_t lease = l->lease_ms ? l->lease_ms : kDefaultLease;
    if (lease < 250 || lease > 60000) {
        lease = std::clamp<uint32_t>(lease, 250, 60000);
        if (d.env.log) d.env.log(name, fmt("add_hud_layer: lease_ms %u is out of range; using %u", l->lease_ms, lease));
    }
    Layer& L    = d.layers[n];
    L.mod       = mod;
    L.name      = name;
    L.parts     = parts;
    L.lease     = lease;
    L.on_status = l->on_status;
    L.user      = l->user;
    d.count.store(n + 1, std::memory_order_release);   // only now can a lookup find it
    return n;
}

int HudEngine::layer_of(const void* mod) const {
    if (!mod) return -1;
    const int n = d_->count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (d_->layers[i].mod == mod) return i;
    return -1;
}
int HudEngine::layer_count() const { return d_->count.load(std::memory_order_acquire); }
uint32_t HudEngine::layer_parts(int i) const { return d_->layers[i].parts; }
uint32_t HudEngine::layer_lease(int i) const { return d_->layers[i].lease; }
const std::string& HudEngine::layer_name(int i) const { return d_->layers[i].name; }

int HudEngine::set_state(int layer, const FH2HudState* s) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (!s || s->size < sizeof(FH2HudState)) {
        if (d.once_l(layer, "state size"))
            d.mlog(layer, "set_hud_state: the FH2HudState is too small: set size = sizeof(FH2HudState); ignored");
        return 0;
    }
    std::vector<std::pair<std::string, std::string>> notes;   // (once key, line)
    uint32_t odd = 0;
    std::string odd_field;
    const auto note = [&](const std::string& key, const std::string& line) { notes.emplace_back(key, line); };
    const auto text = [&](const std::string& field, const std::string& kind, const char* utf8, std::size_t limit, bool caps) {
        unsigned f = 0;
        uint32_t o = 0;
        std::wstring w = hud_text(utf8, limit, caps, &f, &o);
        if (f & kHudTextControl)
            note("control " + kind, "set_hud_state: " + field + " has a line break or another control character; shown as a space");
        if (f & kHudTextCut)
            note("cut " + kind, "set_hud_state: " + field + " is longer than " + std::to_string(limit) + " characters; the rest is cut");
        if ((f & kHudTextOdd) && !odd) odd = o, odd_field = field;
        return w;
    };
    const auto enum_of = [&](const std::string& field, const std::string& kind, long long v, int max) {
        if (v >= 0 && v <= max) return static_cast<int>(v);
        note("enum " + kind, "set_hud_state: " + field + " = " + std::to_string(v) + " isn't one of its values; 0 used");
        return 0;
    };
    const auto keycaps = [&](const std::string& field, const std::string& kind, const char* utf8) {
        unsigned f = 0;
        uint32_t o = 0;
        const std::wstring w = hud_text(utf8, 1000, false, &f, &o);
        if (f & kHudTextControl)
            note("control " + kind, "set_hud_state: " + field + " has a line break or another control character; shown as a space");
        if ((f & kHudTextOdd) && !odd) odd = o, odd_field = field;
        unsigned g = 0;
        HudKeyCaps k = hud_key_caps(w, &g);
        if (g & kHudTextCut) note("cut " + kind, "set_hud_state: " + field + " is longer than 12 characters; the rest is cut");
        return k;
    };
    const auto prompt = [&](const std::string& field, const std::string& kind, const FH2HudPrompt& q, PromptData& out, bool full) {
        out.glyph = static_cast<uint32_t>(enum_of(field + ".glyph", kind + ".glyph", q.glyph, 24));
        out.chord = q.chord != 0;
        out.key   = keycaps(field + ".key", kind + ".key", q.key);
        if (full) {
            out.label = text(field + ".label", kind + ".label", q.label, 24, false);
            out.hold  = q.hold != 0;
        }
        if (q.reserved[0] || q.reserved[1] || q.reserved[2]) note("reserved " + kind, "set_hud_state: " + field + "'s reserved isn't zero");
    };
    Data x;
    for (int i = 0; i < FH2_HUD_MAX_TABS; ++i) {
        if (!s->tabs[i] || !*s->tabs[i]) break;
        x.tabs[i]   = text("tabs[" + std::to_string(i) + "]", "tabs", s->tabs[i], 16, true);
        x.tab_count = i + 1;
    }
    x.tab = s->tab;
    for (int i = 0; i < 2; ++i) x.tab_keys[i] = text("tab_keys[" + std::to_string(i) + "]", "tab_keys", s->tab_keys[i], 12, true);
    x.tab_keys_given    = !x.tab_keys[0].empty() && !x.tab_keys[1].empty();
    x.line1             = text("line1", "line1", s->line1, 40, true);
    x.line1_after       = text("line1_after", "line1_after", s->line1_after, 24, false);
    x.line1_after_style = enum_of("line1_after_style", "line1_after_style", s->line1_after_style, 2);
    x.line2             = text("line2", "line2", s->line2, 64, false);
    x.centre            = enum_of("centre", "centre", s->centre, 1);
    x.strip             = enum_of("strip", "strip", s->strip, 2);
    x.strip_row         = s->strip_row != 0;
    x.strip_left        = text("strip_left", "strip_left", s->strip_left, 12, false);
    x.strip_right       = text("strip_right", "strip_right", s->strip_right, 12, false);
    x.strip_after       = text("strip_after", "strip_after", s->strip_after, 48, false);
    x.mark_count        = static_cast<int>(std::min<uint32_t>(s->mark_count, FH2_HUD_MAX_MARKS));
    if (s->mark_count > FH2_HUD_MAX_MARKS)
        note("count marks", "set_hud_state: mark_count = " + std::to_string(s->mark_count) + " is more than 16; the first 16 used");
    for (int k = 0; k < x.mark_count; ++k) {
        const FH2HudMark& m = s->marks[k];
        const std::string f = "marks[" + std::to_string(k) + "]";
        x.marks[k].at       = m.at;
        x.marks[k].shape    = enum_of(f + ".shape", "marks.shape", m.shape, 1);
        x.marks[k].style    = enum_of(f + ".style", "marks.style", m.style, 2);
        x.marks[k].number   = static_cast<uint16_t>(std::min<unsigned>(m.number, 999));
        if (m.number > 999) note("number marks", "set_hud_state: " + f + ".number = " + std::to_string(m.number) + " is more than 999; 999 used");
        if (m.reserved) note("reserved marks", "set_hud_state: " + f + "'s reserved isn't zero");
    }
    x.playhead     = s->playhead != 0;
    x.prompt_count = static_cast<int>(std::min<uint32_t>(s->prompt_count, FH2_HUD_MAX_PROMPTS));
    if (s->prompt_count > FH2_HUD_MAX_PROMPTS)
        note("count prompts", "set_hud_state: prompt_count = " + std::to_string(s->prompt_count) + " is more than 5; the first 5 used");
    for (int i = 0; i < x.prompt_count; ++i) prompt("prompts[" + std::to_string(i) + "]", "prompts", s->prompts[i], x.prompts[i], true);
    for (int i = 0; i < 5; ++i) {
        const FH2HudChip& c = i < 4 ? s->chips[i] : s->corner;
        const std::string f = i < 4 ? "chips[" + std::to_string(i) + "]" : std::string("corner");
        const std::string kind = i < 4 ? "chips" : "corner";
        x.chips[i].text     = text(f + ".text", kind + ".text", c.text, 24, true);
        x.chips[i].dots     = std::min<int>(c.dots, FH2_HUD_MAX_CHIP_DOTS);
        x.chips[i].filled   = std::min<int>(c.dots_filled, x.chips[i].dots);
        if (c.dots > FH2_HUD_MAX_CHIP_DOTS)
            note("dots " + kind, "set_hud_state: " + f + ".dots = " + std::to_string(c.dots) + " is more than 5; 5 used");
        if (c.dots_filled > std::min<int>(c.dots, FH2_HUD_MAX_CHIP_DOTS))
            note("filled " + kind, "set_hud_state: " + f + ".dots_filled is more than its dots; all of them filled");
        bool r = false;
        for (uint8_t b : c.reserved) r = r || b;
        if (r) note("reserved " + kind, "set_hud_state: " + f + "'s reserved isn't zero");
    }
    x.tip_before = text("tip_before", "tip_before", s->tip_before, 48, false);
    x.tip_after  = text("tip_after", "tip_after", s->tip_after, 48, false);
    prompt("tip_button", "tip_button", s->tip_button, x.tip, false);
    x.row_count = static_cast<int>(std::min<uint32_t>(s->row_count, FH2_HUD_MAX_ROWS));
    if (s->row_count > FH2_HUD_MAX_ROWS)
        note("count rows", "set_hud_state: row_count = " + std::to_string(s->row_count) + " is more than 8; the first 8 used");
    for (int r = 0; r < x.row_count; ++r) {
        const FH2HudRow& w  = s->rows[r];
        const std::string f = "rows[" + std::to_string(r) + "]";
        x.rows[r].label     = text(f + ".label", "rows.label", w.label, 32, true);
        x.rows[r].value     = text(f + ".value", "rows.value", w.value, 32, false);
        x.rows[r].style     = enum_of(f + ".style", "rows.style", w.style, 2);
        if (w.reserved[0] || w.reserved[1] || w.reserved[2]) note("reserved rows", "set_hud_state: " + f + "'s reserved isn't zero");
    }
    x.row_selected    = s->row_selected;
    x.footer          = text("panel_footer", "panel_footer", s->panel_footer, 96, false);
    x.countdown       = std::min<uint32_t>(s->countdown, 999);
    if (s->countdown > 999) note("countdown", "set_hud_state: countdown = " + std::to_string(s->countdown) + " is more than 999; 999 used");
    x.countdown_label = text("countdown_label", "countdown_label", s->countdown_label, 48, false);
    // logged once each, outside the layer's lock
    for (const auto& [key, line] : notes)
        if (d.once_l(layer, key)) d.mlog(layer, line);
    if (odd && d.once_l(layer, "letters"))
        d.mlog(layer, fmt("set_hud_state: %s has letters outside the set seen working in the HUD's fonts (U+%04X); they may show as boxes",
                          odd_field.c_str(), odd));
    {
        std::scoped_lock lk{L.mu};
        L.staged = std::move(x);
        L.gen.fetch_add(1);
    }
    return 1;
}

int HudEngine::set_value(int layer, int which, float value) {
    Layer& L = d_->layers[layer];
    d_->call(L);
    if (which != FH2_HUD_PLAYHEAD && which != FH2_HUD_HOLD) return 0;
    if (std::isnan(value)) value = 0;
    value = std::clamp(value, 0.0f, 1.0f);
    (which == FH2_HUD_PLAYHEAD ? L.playhead : L.holdv).store(fbits(value));
    return 1;
}

int HudEngine::show(int layer, uint32_t parts) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (d.off_now()) {
        if (d.once_l(layer, "show off")) d.mlog(layer, "show_hud_layer: the HUD layer is off this session (see the lines above)");
        return 0;
    }
    const uint32_t extra = parts & ~(L.parts | FH2_HUD_PART_INSTANT);
    if (extra && d.once_l(layer, "show bits"))
        d.mlog(layer, fmt("show_hud_layer: parts 0x%X aren't in this mod's layer; left out", extra));
    const uint32_t p = (parts & L.parts) | (parts & FH2_HUD_PART_INSTANT);
    const uint64_t s = d.seq.fetch_add(1) + 1;
    L.show_parts.store(p);
    if (p & FH2_HUD_PART_ALL) {
        L.parts_seq.store(s);
        L.epoch.fetch_add(1);   // LAPSED goes; TAKEN goes with the new parts_seq
    }
    L.show_seq.store(s, std::memory_order_release);
    return 1;
}

int HudEngine::pulse(int layer, int pulse) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (pulse != FH2_HUD_PULSE_SHUTTER) {
        // one line for the first unknown pulse, whatever comes after it, so a changing value can't flood the log
        if (d.once_l(layer, "pulse bad"))
            d.mlog(layer, "pulse_hud: " + std::to_string(pulse) + " isn't a pulse this preview has");
        return 0;
    }
    // shown: its newest show had parts, the platform hasn't ended that show (a lapse, a crash in on_status),
    // and no other mod's show with parts came after it
    const uint64_t ps = L.parts_seq.load();
    bool shown        = !d.off_now() && ps != 0 && L.show_seq.load() == ps && L.killed.load() != ps;
    const int n       = d.count.load(std::memory_order_acquire);
    for (int i = 0; i < n && shown; ++i)
        if (i != layer && d.layers[i].parts_seq.load() > ps) shown = false;
    if (!shown) {
        if (d.once_l(layer, "pulse not shown")) d.mlog(layer, "pulse_hud: the layer isn't shown, so there's nothing to blink");
        return 0;
    }
    L.pulses.fetch_add(1);
    return 1;
}

void HudEngine::stop_layer(int layer) {
    if (layer < 0 || layer >= d_->count.load(std::memory_order_acquire)) return;
    d_->layers[layer].stopped.store(true);
}

uint32_t HudEngine::status(int layer) {
    Layer& L = d_->layers[layer];
    d_->call(L);
    return L.status.load();
}

int HudEngine::hold(int layer, int h) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (d.off_now()) {
        if (d.once_l(layer, "hold off")) d.mlog(layer, "hold_game_hud: the HUD layer is off this session (see the lines above)");
        return 0;
    }
    if (!d.hold_ok.load()) {
        if (d.once_l(layer, "hold code"))
            d.mlog(layer, "hold_game_hud: the game's HUD code isn't as expected, or the hold crashed earlier this session; the game's HUD stays");
        return 0;
    }
    const bool before = d.wish_counts(L);
    if (h) {
        L.hold_epoch.fetch_add(1);
        L.epoch.fetch_add(1);   // LAPSED goes
        L.wish.store(1);
    } else {
        L.wish.store(0);
    }
    if (before != d.wish_counts(L)) d.rated(layer, 2, h ? "hold_game_hud: held" : "hold_game_hud: let go");
    return 1;
}

int HudEngine::play_sound(int layer, uint32_t sound) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (d.off_now()) {
        if (d.once_l(layer, "sound off")) d.mlog(layer, "play_ui_sound: the HUD layer is off this session (see the lines above)");
        return 0;
    }
    if (sound < FH2_SOUND_ERROR || sound > FH2_SOUND_SHUTTER) {
        // one line for the first unknown sound, whatever comes after it
        if (d.once_l(layer, "sound bad"))
            d.mlog(layer, "play_ui_sound: " + std::to_string(sound) + " isn't a sound this preview plays");
        return 0;
    }
    if (!d.sounds_ok.load()) {
        if (d.once_l(layer, "sounds unavailable"))
            d.mlog(layer, "play_ui_sound: the game's sound code isn't as expected, or sounds are off after a crash; not played");
        return 0;
    }
    const double t = d.clock();
    std::scoped_lock lk{L.snd_mu};
    if (t - L.sound_at[sound] < kSoundRepeatMs) return 0;
    if (world_only(sound) && !d.live_now.load()) {
        if (d.once_l(layer, std::string("world ") + kSoundNames[sound]))
            d.mlog(layer, fmt("play_ui_sound: %s only sounds in the world, and the free-roam HUD page isn't on screen (a menu or the map is "
                              "open); not played",
                              kSoundNames[sound]));
        return 0;
    }
    L.sound_at[sound] = t;
    if (L.sounds.size() >= 4) L.sounds.pop_front();
    L.sounds.push_back({sound, t, layer});
    return 1;
}

int HudEngine::post_feed(int layer, const FH2HudFeedLine* line) {
    Impl& d  = *d_;
    Layer& L = d.layers[layer];
    d.call(L);
    if (!(L.parts & FH2_HUD_PART_FEED)) {
        if (d.once_l(layer, "feed none")) d.mlog(layer, "post_hud_feed: this mod's layer has no FH2_HUD_PART_FEED; not shown");
        return 0;
    }
    if (d.off_now()) {
        if (d.once_l(layer, "feed off")) d.mlog(layer, "post_hud_feed: the HUD layer is off this session (see the lines above)");
        return 0;
    }
    if (!line || line->size < sizeof(FH2HudFeedLine)) {
        if (d.once_l(layer, "feed size")) d.mlog(layer, "post_hud_feed: the FH2HudFeedLine is too small: set size = sizeof(FH2HudFeedLine); not shown");
        return 0;
    }
    const auto text = [&](const char* field, const char* utf8, std::size_t limit) {
        unsigned f = 0;
        uint32_t o = 0;
        std::wstring w = hud_text(utf8, limit, false, &f, &o);
        if ((f & kHudTextControl) && d.once_l(layer, std::string("feed control ") + field))
            d.mlog(layer, std::string("post_hud_feed: ") + field + " has a line break or another control character; shown as a space");
        if ((f & kHudTextCut) && d.once_l(layer, std::string("feed cut ") + field))
            d.mlog(layer, std::string("post_hud_feed: ") + field + " is longer than " + std::to_string(limit) + " characters; the rest is cut");
        if ((f & kHudTextOdd) && d.once_l(layer, "feed odd"))
            d.mlog(layer, fmt("post_hud_feed: %s has letters outside the set seen working in the HUD's fonts (U+%04X); they may show as boxes", field, o));
        return w;
    };
    FeedLine f;
    f.text = text("text", line->text, 64);
    if (f.text.empty()) {
        if (d.once_l(layer, "feed empty")) d.mlog(layer, "post_hud_feed: the line has no text; not shown");
        return 0;
    }
    f.name = text("name", line->name, 24);
    f.style = line->style <= FH2_HUD_FEED_YELLOW ? static_cast<int>(line->style) : 0;
    if (line->style > FH2_HUD_FEED_YELLOW && d.once_l(layer, "feed style"))
        d.mlog(layer, "post_hud_feed: style " + std::to_string(line->style) + " isn't one of its values; FH2_HUD_FEED_PLAIN used");
    const uint32_t ms = line->ms ? std::clamp<uint32_t>(line->ms, 2000, 30000) : 8000;
    f.remain = static_cast<double>(ms);
    {
        std::scoped_lock lk{d.feed_mu};
        d.feed_staged.push_back(std::move(f));
        if (d.feed_staged.size() > kFeedStaged) d.feed_staged.pop_front();
    }
    d.feed_posts.fetch_add(1);
    return 1;
}

uint32_t HudEngine::parts() const {
    uint32_t u  = 0;
    const int n = d_->count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) u |= d_->layers[i].parts;
    return u;
}

void HudEngine::build(const std::vector<HudProp>& props) {
    Impl& d = *d_;
    d.table = props;
    d.np    = static_cast<int>(props.size());
    d.p.reset(new Prop[static_cast<std::size_t>(d.np)]);
    d.index.clear();
    for (int i = 0; i < d.np; ++i) {
        const HudProp& h = d.table[static_cast<std::size_t>(i)];
        Prop& q          = d.p[i];
        q.type           = h.type;
        q.role           = h.role;
        q.part           = h.part;
        if (h.type == 's') {
            q.tgt_text.assign(h.def.begin(), h.def.end());   // the defaults are ASCII ("HOLD")
            q.told_text.clear();
        }
        d.index[h.short_name] = i;
    }
    const auto at = [&](const std::string& n) {
        const auto it = d.index.find(n);
        return it == d.index.end() ? -1 : it->second;
    };
    Ix& x = d.ix;
    x.Shown = at("Shown"), x.Shutter = at("Shutter"), x.HoldWord = at("HoldWord"), x.HoldAngle = at("HoldAngle"), x.HoldFill = at("HoldFill");
    x.TabsShown = at("TabsShown"), x.TabKbd = at("TabKbd"), x.TabKeyL = at("TabKeyL"), x.TabKeyR = at("TabKeyR");
    for (int i = 0; i < 8; ++i) {
        const std::string t = "Tab" + std::to_string(i);
        x.Tab[i] = at(t), x.TabShown[i] = at(t + "Shown"), x.TabSel[i] = at(t + "Sel");
        x.Dot[i] = at("Dot" + std::to_string(i));
    }
    x.Alert = at("Alert"), x.L1 = at("L1"), x.L1After = at("L1After"), x.L1Style = at("L1Style"), x.L2 = at("L2"), x.Dots = at("Dots");
    x.InlineAfter = at("InlineAfter"), x.StripRow = at("StripRow"), x.Rail = at("Rail"), x.RailLeft = at("RailLeft");
    x.RailRight = at("RailRight"), x.RowAfter = at("RowAfter"), x.Playhead = at("Playhead"), x.PlayheadX = at("PlayheadX");
    for (int k = 0; k < 16; ++k) {
        const std::string m = "Mark" + std::to_string(k);
        x.MarkShown[k] = at(m + "Shown"), x.MarkX[k] = at(m + "X"), x.MarkAngle[k] = at(m + "Angle"), x.MarkStyle[k] = at(m + "Style");
        x.MarkNum[k] = at(m + "Num");
    }
    const auto prompt_ix = [&](const std::string& p, PromptIx& r) {
        r.Shown = at(p + "Shown"), r.Hold = at(p + "Hold"), r.Kbd = at(p + "Kbd"), r.Chord = at(p + "Chord"), r.G = at(p + "G");
        r.K0 = at(p + "K0"), r.K0Arrow = at(p + "K0Arrow"), r.K0Angle = at(p + "K0Angle"), r.K1Shown = at(p + "K1Shown");
        r.Join = at(p + "Join"), r.K1 = at(p + "K1"), r.K1Arrow = at(p + "K1Arrow"), r.K1Angle = at(p + "K1Angle"), r.Label = at(p + "Label");
    };
    for (int i = 0; i < 5; ++i) prompt_ix("P" + std::to_string(i), x.P[i]);
    prompt_ix("Tip", x.Tip);
    x.Tip.Shown = x.Tip.Hold = x.Tip.Label = -1;
    x.TipBefore = at("TipBefore"), x.TipAfter = at("TipAfter");
    for (int i = 0; i < 5; ++i) {
        const std::string c = "C" + std::to_string(i);
        x.CShown[i] = at(c + "Shown"), x.C[i] = at(c);
        for (int j = 0; j < 5; ++j) x.CD[i][j] = at(c + "D" + std::to_string(j));
    }
    for (int r = 0; r < 8; ++r) {
        const std::string w = "R" + std::to_string(r);
        x.RShown[r] = at(w + "Shown"), x.RSel[r] = at(w + "Sel"), x.RLabel[r] = at(w + "Label"), x.RStyle[r] = at(w + "Style");
        x.RHasValue[r] = at(w + "HasValue"), x.RValue[r] = at(w + "Value");
    }
    x.PanelFooter = at("PanelFooter"), x.CountText = at("CountText"), x.CountPop = at("CountPop"), x.CountLabel = at("CountLabel");
    x.CountLabelShown = at("CountLabelShown");
    for (int i = 0; i < kHudFeedLines; ++i) {
        const std::string f = "F" + std::to_string(i);
        x.FShown[i] = at(f + "Shown"), x.FHasName[i] = at(f + "HasName"), x.FName[i] = at(f + "Name"), x.FText[i] = at(f + "Text");
        x.FStyle[i] = at(f + "Style");
    }
    for (int k = 0; k < kHudParts; ++k) {
        x.fade[k]          = at(kHudPartProps[k]);
        x.vis[k]           = at(std::string(kHudPartProps[k]) + "Vis");
        d.part[k].declared = x.fade[k] >= 0;
    }
}

const std::vector<HudProp>& HudEngine::props() const { return d_->table; }
int HudEngine::prop_index(const std::string& n) const {
    const auto it = d_->index.find(n);
    return it == d_->index.end() ? -1 : it->second;
}

void HudEngine::set_off(HudOff why) {
    if (why == HudOff::none) return;
    if (why == HudOff::strings_fault) d_->strings_fault = true;
    HudOff none = HudOff::none;
    d_->off.compare_exchange_strong(none, why);
}
bool HudEngine::off() const { return d_->off_now(); }
HudOff HudEngine::off_reason() const { return d_->off.load(); }
void HudEngine::set_hold_available(bool ok) { d_->hold_ok = ok; }
void HudEngine::set_sounds_available(bool ok) { d_->sounds_ok = ok; }

HudEngine::FrameOut HudEngine::frame(const FrameIn& in) {
    Impl& d = *d_;
    FrameOut out;
    d.frame         = in.frame;
    const double dt = d.last_now < 0 ? 0.0 : std::max(0.0, in.now - d.last_now);
    d.last_now      = in.now;
    d.now           = in.now;
    d.notified      = 0;
    d.live_now.store(in.live);
    ++d.st.frames;
    const int n = d.count.load(std::memory_order_acquire);
    if (d.off_now()) {
        d.off_frame(in, n);
        return out;
    }
    if (d.np == 0) return out;
    // a new live page: a full refresh, hides first
    if (in.live && (in.page != d.told_page || in.page_gen != d.told_gen)) {
        d.told_page = in.page;
        d.told_gen  = in.page_gen;
        d.refresh();
    }
    d.collect(n);
    d.stops(n);
    d.choose_owner(n);
    if (d.leases(in, n, dt)) d.choose_owner(n);
    d.feed_step(in, dt);
    // the owner's pulses; the others' are dropped (pulse_hud only says 1 to the owner)
    for (int i = 0; i < n; ++i) {
        Layer& L         = d.layers[i];
        const uint32_t c = L.pulses.load();
        if (c == L.seen_pulses) continue;
        L.seen_pulses = c;
        if (i == d.owner_ && d.wanted) {
            d.shutter_req    = true;
            d.shutter_req_at = in.now;
        }
    }
    if (d.shutter_req && !in.live && in.now - d.shutter_req_at > kPulseMs) d.shutter_req = false;   // never seen: dropped
    // targets, when the owner's state, the owner or the device changed
    if (d.owner_ >= 0) {
        Layer& L = d.layers[d.owner_];
        if (d.owner_ != d.target_owner || L.copy_gen != d.target_gen || in.kbd != d.target_kbd) {
            d.targets(L.copy, in.kbd);
            d.target_owner = d.owner_;
            d.target_gen   = L.copy_gen;
            d.target_kbd   = in.kbd;
        }
        d.frame_values(bitsf(L.playhead.load()), bitsf(L.holdv.load()));
    }
    if (in.live) d.screen();
    d.publish();
    bool carried = false;
    if (in.live && !d.off_now()) {
        if (!d.notify(&carried)) {
            // a notify faulted (logged by the glue): nothing more is told; the next frame turns the rest off
        }
    }
    if (!d.off_now()) d.retire();
    if (d.notified) {
        ++d.st.busy_frames;
        d.st.notifies += d.notified;
        d.st.max_notifies = std::max<uint64_t>(d.st.max_notifies, d.notified);
    }
    out.notified = d.notified;
    d.sounds(in, out);
    d.statuses(in, n, true);
    return out;
}

bool HudEngine::hold_wanted() const {
    const Impl& d = *d_;
    if (d.off_now() || !d.hold_ok.load()) return false;
    const int n = d.count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (d.wish_counts(d.layers[i])) return true;
    return false;
}

std::string HudEngine::holders() const {
    std::string s;
    const int n = d_->count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (d_->wish_counts(d_->layers[i])) s += (s.empty() ? "" : ", ") + d_->layers[i].name;
    return s;
}

int HudEngine::owner() const { return d_->owner_; }
uint32_t HudEngine::shown_parts() const {
    uint32_t b = 0;
    for (int k = 0; k < kHudParts; ++k)
        if (d_->part[k].state != kOff) b |= 1u << k;
    return b;
}
uint64_t HudEngine::last_notify_frame() const { return d_->last_notify; }
bool HudEngine::clean() const { return !d_->shown_told && d_->frame >= d_->clean_from; }

HudEngine::Stats HudEngine::take_stats() {
    Stats s = d_->st;
    d_->st  = Stats{};
    return s;
}

std::size_t HudEngine::strings_in_use() const {
    std::size_t n = 0;
    for (int i = 0; i < d_->np; ++i) n += d_->p[i].cache != nullptr;
    return n;
}

std::string HudEngine::strings_line() const {
    const Impl& d = *d_;
    std::size_t in_use = 0;
    long long refs     = 0;
    int most           = 0;
    for (int i = 0; i < d.np; ++i)
        if (void* s = d.p[i].cache) {
            ++in_use;
            const int r = d.env.game->refs(s) - 1;
            refs += r;
            most = std::max(most, r);
        }
    return fmt("Strings: %zu in use (one per text shown), %zu waiting to be let go; made %llu, freed %llu, left with the game %llu, left with "
               "the page while off screen %llu; the game holds %lld references on the ones in use (at most %d on one)",
               in_use, d.retired.size(), static_cast<unsigned long long>(d.made.load()), static_cast<unsigned long long>(d.freed_n),
               static_cast<unsigned long long>(d.left_n), static_cast<unsigned long long>(d.off_n), refs, most);
}

void HudEngine::log_rated(int layer, int kind, const std::string& text) { d_->rated(layer, kind, text); }
bool HudEngine::once(int layer, const std::string& key) { return d_->once_l(layer, key); }

uint32_t HudEngine::value(int prop, bool gated) const {
    const Impl& d = *d_;
    if (gated || d.off_now() || !d.valid(prop)) return 0;
    return d.p[prop].pub.load(std::memory_order_relaxed);
}

void* HudEngine::string(int prop, bool gated) {
    Impl& d = *d_;
    if (d.strings_fault.load()) return nullptr;
    void* e = d.empty.load(std::memory_order_acquire);
    if (!e && !(e = d.make_empty())) return nullptr;
    AcquireSRWLockShared(&d.str_lock);
    void* s = e;
    if (!gated && !d.off_now() && d.valid(prop) && d.p[prop].cache) s = d.p[prop].cache;
    d.env.game->add_ref(s);
    ReleaseSRWLockShared(&d.str_lock);
    return s;
}

} // namespace fh2platform

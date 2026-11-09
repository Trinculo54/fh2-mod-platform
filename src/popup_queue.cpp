// The pop-up queue: see popup_queue.hpp for the rules. No game, no Windows calls and no clock here; popups.cpp is
// the glue, and tests/ui_queue_test.cpp drives it with a fake clock.
#include "popup_queue.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace fh2platform {
namespace {

constexpr double kShowGapMs      = 5000;     // after any Show the watch saw: the game's 4 s and its fade
constexpr double kOwnGapMs       = 5500;     // after the platform's own, when there's no watch
constexpr double kOnScreenMs     = 5000;     // a pop-up shown this long ago may still be on screen
constexpr double kReplyWindowMs  = 1000;     // REPLY: show_toast this soon after one of the mod's callbacks
constexpr double kReplyWaitMs    = 6000;     // REPLY: dropped if it couldn't show within this
constexpr double kUrgentExpireMs = 600000;   // URGENT: 10 minutes of time it could have shown
constexpr int kUrgentPerSession  = 2;
constexpr double kTickStepMs     = 250;      // at most this much time is counted from one tick to the next
constexpr double kTipGapMs       = 180000;   // 3 minutes between tips, from any mod
constexpr double kAnnounceMs     = 12000;    // the platform's own notice, after the HUD is first on screen
constexpr int kTipsPerSession    = 3;
constexpr double kRateMs         = 10000;
constexpr std::size_t kTitleChars = 28, kLineChars = 45, kKeyChars = 32, kKeyNameChars = 12, kRecent = 10;

// What a waiting pop-up waited for (Item::held), for the line that says it was shown.
enum : uint32_t {
    kHeldOffScreen = 1,
    kHeldSuspended = 2,
    kHeldLane      = 4,
    kHeldAhead     = 8,
    kHeldNotReady  = 16,
};

// The platform's words in tips' buttons (word(); lang.cpp's table has each).
enum : int { kSpace, kUp, kDown, kLeft, kRight, kDpad };
const char* const kWords[] = {"Space", "Up", "Down", "Left", "Right", "D-pad"};

const char* const kIconNames[] = {"",        "Achievement",    "RoadDiscovery",  "PhotoCollection", "RewardBoards",
                                  "BucketList", "CarMeet",     "ClubTierPayout", "DrivatarPayout",  "BarnFindDiscovered"};

bool key_ok(const char* k) {
    if (!k || !*k || std::strlen(k) > kKeyChars) return false;
    for (const char* c = k; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    return true;
}

std::string seconds(double ms) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f s", ms / 1000.0);
    return b;
}

std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return s;
}

std::string ascii_upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// The next code point of UTF-8 `s` from `i` (moved past it); a byte that isn't UTF-8 comes back as 0xFFFD.
uint32_t next_code_point(const std::string& s, std::size_t& i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned char c = b(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    const int n = c >= 0xC2 && c < 0xE0 ? 1 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xF0 && c < 0xF5 ? 3 : 0;
    if (!n || i + static_cast<std::size_t>(n) >= s.size()) {
        ++i;
        return 0xFFFD;
    }
    uint32_t cp = c & (n == 1 ? 0x1F : n == 2 ? 0x0F : 0x07);
    for (std::size_t k = 1; k <= static_cast<std::size_t>(n); ++k) {
        if ((b(i + k) & 0xC0) != 0x80) {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b(i + k) & 0x3F);
    }
    i += static_cast<std::size_t>(n) + 1;
    return cp;
}

// Latin-1's letters, U+00C0 to U+00FF.
const char* const kLatin1[64] = {"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I",  "I",
                                 "D", "N", "O", "O", "O", "O", "O",  "x", "O", "U", "U", "U", "U", "Y", "TH", "ss",
                                 "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i",  "i",
                                 "d", "n", "o", "o", "o", "o", "o",  "/", "o", "u", "u", "u", "u", "y", "th", "y"};
// Latin Extended-A, U+0100 to U+017F, one letter each; '#' marks the two ligatures (IJ and OE).
const char kExtendedA[] = "AaAaAaCcCcCcCcDd"
                          "DdEeEeEeEeEeGgGg"
                          "GgGgHhHhIiIiIiIi"
                          "Ii##JjKkkLlLlLlL"
                          "lLlNnNnNnnNnOoOo"
                          "Oo##RrRrRrSsSsSs"
                          "SsTtTtTtUuUuUuUu"
                          "UuUuWwYyYZzZzZzs";

// One code point folded to ASCII ("" to leave it out).
std::string fold_one(uint32_t cp) {
    if (cp < 0x80) return cp < 0x20 ? " " : std::string(1, static_cast<char>(cp));
    if (cp >= 0xC0 && cp <= 0xFF) return kLatin1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) {
        const char c = kExtendedA[cp - 0x100];
        if (c != '#') return std::string(1, c);
        if (cp == 0x132) return "IJ";
        if (cp == 0x133) return "ij";
        return cp == 0x152 ? "OE" : "oe";
    }
    if (cp >= 0x218 && cp <= 0x21B) return std::string(1, "SsTt"[cp - 0x218]);   // Romanian's comma below
    if (cp >= 0x300 && cp <= 0x36F) return "";                                  // combining marks
    switch (cp) {
        case 0xA0: case 0x2000: case 0x2001: case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006:
        case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F: case 0x3000: return " ";
        case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: return "";
        case 0xA1: return "!";
        case 0xBF: return "?";
        case 0xAB: case 0xBB: case 0x201C: case 0x201D: case 0x201E: case 0x201F: return "\"";
        case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2039: case 0x203A: return "'";
        case 0xB7: case 0x2022: case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
        case 0x2212: return "-";
        case 0xA9: return "(c)";
        case 0xAE: return "(R)";
        case 0xD7: return "x";
        case 0x2026: return "...";
        default: return "?";
    }
}

} // namespace

std::string popup_fold_ascii(const std::string& s, bool* changed) {
    std::string out;
    out.reserve(s.size());
    bool any = false;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t at = i;
        const uint32_t cp    = next_code_point(s, i);
        if (cp >= 0x20 && cp < 0x80) {
            out += static_cast<char>(cp);
            continue;
        }
        const std::string f = fold_one(cp);
        if (f != s.substr(at, i - at)) any = true;
        out += f;
    }
    if (changed) *changed = any;
    return out;
}

PopupQueue::PopupQueue(PopupEnv env) : env_(std::move(env)) {}

// ---- logging ----------------------------------------------------------------------------------------------------

void PopupQueue::log(const std::string& who, const std::string& line) const {
    if (env_.log) env_.log(who, line);
}

void PopupQueue::log_once(const PopupMod& m, const std::string& what, const std::string& line) {
    if (once_[m.name + '\x1f' + what]) return;
    once_[m.name + '\x1f' + what] = true;
    log(m.name, line);
}

void PopupQueue::log_rated(const std::string& who, const std::string& what, const std::string& key, const std::string& line, double now) {
    Rated& r = rated_[who + '\x1f' + what + '\x1f' + key];
    if (now - r.since < kRateMs) {
        ++r.more;
        return;
    }
    if (r.more) log(who, "pop-ups " + what + " " + std::to_string(r.more) + " more time" + (r.more == 1 ? "" : "s") + " in the last 10 s" +
                             (key.empty() ? "" : " (key " + key + ")"));
    r       = Rated{};
    r.since = now;
    r.who   = who;
    r.what  = what;
    r.key   = key;
    log(who, line);
}

// The counts held back, once their 10 s are over (or all of them). An entry whose 10 s are over has nothing left to
// hold back, so it goes: a mod with a new key for each pop-up mustn't grow the map the tick walks.
void PopupQueue::flush_rated(double now, bool all) {
    for (auto i = rated_.begin(); i != rated_.end();) {
        Rated& r        = i->second;
        const bool over = now - r.since >= kRateMs;
        if (r.more && (all || over)) {
            log(r.who, "pop-ups " + r.what + " " + std::to_string(r.more) + " more time" + (r.more == 1 ? "" : "s") + " in the last 10 s" +
                           (r.key.empty() ? "" : " (key " + r.key + ")"));
            r.more = 0;
        }
        i = over && !r.more ? rated_.erase(i) : std::next(i);
    }
}

std::string PopupQueue::text_of(const Item& it) const {
    if (it.tip) return one_line(it.line);
    const std::string key = it.key.empty() ? "" : "key " + it.key;
    if (it.flags & FH2_TOAST_FLAG_PRIVATE) return key.empty() ? "no key" : key;
    return one_line(it.title) + (it.line.empty() ? "" : " / " + one_line(it.line)) + (key.empty() ? "" : " (" + key + ")");
}

std::string PopupQueue::tip_name(const Item& it) const { return it.key.empty() ? "a tip" : "tip " + it.key; }

std::string PopupQueue::held_text(uint32_t held) const {
    std::string s;
    const auto add = [&](uint32_t bit, const char* what) {
        if (held & bit) s += (s.empty() ? "" : ", ") + std::string(what);
    };
    add(kHeldOffScreen, "the driving view (a menu, loading or a cutscene was up)");
    add(kHeldSuspended, "the game's HUD to come back");
    add(kHeldLane, "the game's own pop-ups");
    add(kHeldAhead, "pop-ups ahead of it");
    add(kHeldNotReady, "the game's notification to be ready");
    return s;
}

// Drops items_[i] with a line saying why.
void PopupQueue::drop(std::size_t i, const std::string& why, double now, bool rated) {
    const Item it = items_[i];
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
    const std::string line = it.tip ? tip_name(it) + " dropped: " + why + "; it isn't marked seen"
                                    : "pop-up dropped: " + why + " (" + text_of(it) + ")";
    if (rated) log_rated(it.mod_name, "dropped", it.key, line, now);
    else log(it.mod_name, line);
}

// ---- the calls --------------------------------------------------------------------------------------------------

int PopupQueue::show_toast(const PopupMod& m, const FH2Toast* t, double now) {
    std::scoped_lock lk{mu_};
    if (!t) {
        log(m.name, "show_toast: no FH2Toast was given");
        return 0;
    }
    if (t->size < offsetof(FH2Toast, subtitle)) {
        log(m.name, "show_toast: the FH2Toast's size is too small (set size = sizeof(FH2Toast))");
        return 0;
    }
    // only `size` bytes are read: a mod built against an older header has zeros for the rest
    FH2Toast c{};
    std::memcpy(&c, t, std::min<std::size_t>(t->size, sizeof c));
    if (c.key && !key_ok(c.key)) {
        log(m.name, std::string("show_toast: \"") + c.key + "\" isn't a usable key (up to 32 letters, digits and _)");
        return 0;
    }
    if (!c.title || !*c.title) {
        log(m.name, "show_toast: a pop-up needs a title");
        return 0;
    }
    Item it;
    it.mod      = m.id;
    it.mod_name = m.name;
    it.dll      = m.dll;
    it.key      = c.key ? c.key : "";
    it.title    = c.title;
    it.line     = c.subtitle ? c.subtitle : "";
    it.flags    = c.flags & (FH2_TOAST_FLAG_URGENT | FH2_TOAST_FLAG_REPLY | FH2_TOAST_FLAG_PRIVATE);
    if (!(m.uses & FH2_UI_USES_TOASTS))
        log_once(m, "undeclared toasts", "shows pop-ups without saying so: declare them in fh2mod_init (ui_uses with FH2_UI_USES_TOASTS) so players get a row for them");
    if (c.flags & ~7u) log_once(m, "toast flags", "show_toast: flags beyond FH2_TOAST_FLAG_ (" + std::to_string(c.flags & ~7u) + ") are ignored");
    // whether it can show at all
    std::string off;
    if (available_ == 0) off = why_off_;
    else if (!platform_toasts_) off = "the player turned mod pop-ups off";
    else if (mod_off_[m.id]) off = "the player turned this mod's pop-ups off";
    else if (option_ == 0) off = "the game's Notifications setting is off (Settings > HUD)";
    if (!off.empty()) {
        log_rated(m.name, "not shown (" + off + ")", it.key, "pop-up not shown: " + off + " (" + text_of(it) + ")", now);
        return 0;
    }
    // the flags each mod is held to. An URGENT one is counted only once it's queued anew: the same one re-sent while
    // it's on screen, or one taking the place of a waiting URGENT one, doesn't use up another.
    bool count_urgent = false;
    if (it.flags & FH2_TOAST_FLAG_URGENT) {
        if (urgent_used_[m.id] >= kUrgentPerSession) {
            it.flags &= ~static_cast<uint32_t>(FH2_TOAST_FLAG_URGENT);
            log_once(m, "urgent cap", "has sent more than 2 urgent pop-ups this session, so later ones are shown as normal ones");
        } else {
            count_urgent = true;
        }
    }
    if (it.flags & FH2_TOAST_FLAG_REPLY) {
        const auto cb = last_callback_.find(m.id);
        if (cb == last_callback_.end() || now - cb->second > kReplyWindowMs) {
            it.flags &= ~static_cast<uint32_t>(FH2_TOAST_FLAG_REPLY);
            log_once(m, "reply window", "sent a reply pop-up more than 1 s after the platform last called one of its hotkeys, commands or "
                                        "callbacks, so it's shown as a normal one (and so are later ones like it)");
        }
    }
    it.icon = c.icon;
    if (it.icon > FH2_ICON_BARNFIND) {
        log_once(m, "icon", "show_toast: icon " + std::to_string(c.icon) + " isn't one of the game's nine pictures, so the pop-up has none");
        it.icon = FH2_ICON_NONE;
    }
    it.sound = c.sound;
    if (it.sound == FH2_SOUND_NOTIFY) {
        log_once(m, "notify sound", "show_toast: the game's notification chimes by itself, so FH2_SOUND_NOTIFY isn't played with a pop-up");
        it.sound = 0;
    } else if (it.sound > FH2_SOUND_SHUTTER) {
        log_once(m, "sound", "show_toast: sound " + std::to_string(c.sound) + " isn't one of FH2_SOUND_, so none plays");
        it.sound = 0;
    }
    uint32_t expire = c.expire_s ? c.expire_s : 120;
    if (expire > 600) {
        log_once(m, "expire", "show_toast: expire_s is at most 600, so 600 is used");
        expire = 600;
    }
    it.expire_ms = (it.flags & FH2_TOAST_FLAG_URGENT) ? kUrgentExpireMs : expire * 1000.0;
    // the same one on screen now: nothing to queue
    if (!it.key.empty())
        for (const OnScreen& s : on_screen_items_)
            if (s.mod == m.id && s.key == it.key && now - s.at < kOnScreenMs && s.title == it.title && s.line == it.line && s.icon == it.icon) {
                log_rated(m.name, "not queued again", it.key, "pop-up not queued again: the same one is on screen now (" + text_of(it) + ")", now);
                return 1;
            }
    // replaced where it stands
    if (!it.key.empty())
        for (Item& w : items_)
            if (!w.tip && w.mod == m.id && w.key == it.key) {
                if (count_urgent && !(w.flags & FH2_TOAST_FLAG_URGENT)) ++urgent_used_[m.id];
                it.id        = w.id;
                it.seq       = w.seq;
                it.queued_at = w.queued_at;
                w            = it;
                log_rated(m.name, "replaced", it.key, "pop-up replaced while waiting: " + text_of(it), now);
                return 1;
            }
    // too many waiting: the oldest that isn't URGENT goes
    std::size_t mine = 0, oldest = items_.size(), oldest_any = items_.size();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const Item& w = items_[i];
        if (w.tip || w.mod != m.id) continue;
        ++mine;
        if (oldest_any == items_.size()) oldest_any = i;
        if (oldest == items_.size() && !(w.flags & FH2_TOAST_FLAG_URGENT)) oldest = i;
    }
    if (mine >= FH2_TOAST_MAX_WAITING) drop(oldest < items_.size() ? oldest : oldest_any, "a 5th was sent while 4 were waiting", now, true);
    if (count_urgent) ++urgent_used_[m.id];
    it.id        = next_id_++;
    it.seq       = next_seq_++;
    it.queued_at = now;
    const char* kind = (it.flags & FH2_TOAST_FLAG_URGENT) ? "urgent pop-up" : (it.flags & FH2_TOAST_FLAG_REPLY) ? "reply pop-up" : "pop-up";
    log_rated(m.name, "queued", it.key, std::string(kind) + " queued: " + text_of(it), now);
    items_.push_back(std::move(it));
    return 1;
}

void PopupQueue::clear_toast(const PopupMod& m, const char* key) {
    std::scoped_lock lk{mu_};
    int n = 0;
    for (std::size_t i = items_.size(); i-- > 0;) {
        const Item& w = items_[i];
        if (w.tip || w.mod != m.id || (key && w.key != key)) continue;
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        ++n;
    }
    if (!n) return;
    if (key) log(m.name, std::string("pop-up taken down while waiting (key ") + key + ")");
    else log(m.name, std::to_string(n) + " waiting pop-up" + (n == 1 ? "" : "s") + " taken down");
}

int PopupQueue::show_tip(const PopupMod& m, const FH2Tip* t, double now) {
    std::scoped_lock lk{mu_};
    if (!t) {
        log(m.name, "show_tip: no FH2Tip was given");
        return 0;
    }
    if (t->size < offsetof(FH2Tip, button)) {
        log(m.name, "show_tip: the FH2Tip's size is too small (set size = sizeof(FH2Tip))");
        return 0;
    }
    FH2Tip c{};
    std::memcpy(&c, t, std::min<std::size_t>(t->size, sizeof c));
    if (t->size < offsetof(FH2Tip, button) + sizeof c.button) c.button = FH2Prompt{};
    if (c.key && !key_ok(c.key)) {
        log(m.name, std::string("show_tip: \"") + c.key + "\" isn't a usable key (up to 32 letters, digits and _)");
        return 0;
    }
    if (!c.text || !*c.text) {
        log(m.name, "show_tip: a tip needs its text");
        return 0;
    }
    Item it;
    it.tip      = true;
    it.mod      = m.id;
    it.mod_name = m.name;
    it.dll      = m.dll;
    it.key      = c.key ? c.key : "";
    it.line     = c.text;
    it.button   = c.button;
    it.hotkey   = c.button.hotkey && *c.button.hotkey ? c.button.hotkey : "";
    it.button.hotkey = nullptr;   // the mod's pointer isn't kept
    it.button.label  = nullptr;
    it.has_button    = c.button.glyph || c.button.vk || !it.hotkey.empty();
    const std::string name = tip_name(it);
    if (!(m.uses & FH2_UI_USES_TIPS))
        log_once(m, "undeclared tips", "shows tips without saying so: declare them in fh2mod_init (ui_uses with FH2_UI_USES_TIPS) so the "
                                       "controller is watched from the start");
    const bool placeholder = it.line.find("{button}") != std::string::npos;
    if (it.has_button && !placeholder) {
        log(m.name, "show_tip: " + name + " has a button, but its text has no {button} to put it in");
        return 0;
    }
    if (!it.has_button && placeholder) {
        log(m.name, "show_tip: " + name + "'s text has {button}, but the tip has no button");
        return 0;
    }
    if (c.button.glyph > FH2_GLYPH_RS_LEFTRIGHT || c.button.vk > 0xFE) {
        log(m.name, "show_tip: " + name + "'s button isn't an FH2_GLYPH_ with a virtual-key code");
        return 0;
    }
    std::string off;
    if (available_ == 0) off = why_off_;
    else if (!tips_) off = "the player turned tips off";
    else if (option_ == 0) off = "the game's Notifications setting is off (Settings > HUD)";
    if (!off.empty()) {
        log_rated(m.name, "not shown (" + off + ")", it.key, name + " not shown: " + off + "; it isn't marked seen", now);
        return 0;
    }
    it.tip_flags    = c.flags & FH2_TIP_FLAG_UNTIL_DONE;
    it.max_sessions = std::min<uint32_t>(c.max_sessions ? c.max_sessions : 1, 10);
    if (!it.key.empty() && !m.dll.empty() && env_.seen_get) {
        const std::string v = env_.seen_get(m.dll, "tip." + it.key);
        if (v == "seen") {
            log_rated(m.name, "not shown (seen)", it.key, name + " not shown: it was seen already", now);
            return 0;
        }
        const long shown = std::strtol(v.c_str(), nullptr, 10);
        if ((it.tip_flags & FH2_TIP_FLAG_UNTIL_DONE) && shown >= static_cast<long>(it.max_sessions)) {
            log_rated(m.name, "not shown (sessions)", it.key,
                      name + " not shown: it has shown in " + std::to_string(shown) + " session" + (shown == 1 ? "" : "s") +
                          ", as many as its max_sessions",
                      now);
            return 0;
        }
    }
    if (tipped_[m.id]) {
        log_rated(m.name, "not shown (one a session)", it.key, name + " not shown: this mod has shown a tip this session already (one a session)", now);
        return 0;
    }
    if (tips_shown_ >= kTipsPerSession) {
        log_rated(m.name, "not shown (three a session)", it.key, name + " not shown: three tips have shown this session already", now);
        return 0;
    }
    const uint32_t delay = c.delay_s ? std::min<uint32_t>(c.delay_s, 600) : 60;
    it.delay_ms          = delay * 1000.0;
    if (text_chars(it.line) > kLineChars) log_once(m, "tip length " + it.key, "show_tip: " + name + "'s text is longer than 45 characters, so it's cut");
    // one waiting tip per mod: a new one takes the place of the last
    for (Item& w : items_)
        if (w.tip && w.mod == m.id) {
            it.id        = w.id;
            it.seq       = w.seq;
            it.queued_at = w.queued_at;
            if (w.key == it.key) it.drive = w.drive;
            const std::string old = tip_name(w);
            w                     = it;
            log_rated(m.name, "replaced", it.key, name + (old == name ? " replaced while waiting: " : " took the place of " + old + ": ") + one_line(it.line),
                      now);
            return 1;
        }
    it.id        = next_id_++;
    it.seq       = next_seq_++;
    it.queued_at = now;
    log(m.name, name + " queued, to show after " + std::to_string(delay) + " s of driving: " + one_line(it.line));
    items_.push_back(std::move(it));
    return 1;
}

int PopupQueue::tip_seen(const PopupMod& m, const char* key) {
    std::scoped_lock lk{mu_};
    if (!key_ok(key)) {
        log_once(m, "tip_seen key", std::string("tip_seen: \"") + (key ? key : "") + "\" isn't a usable key (up to 32 letters, digits and _)");
        return 0;
    }
    return !m.dll.empty() && env_.seen_get && env_.seen_get(m.dll, std::string("tip.") + key) == "seen";
}

void PopupQueue::set_tip_seen(const PopupMod& m, const char* key, int seen) {
    std::scoped_lock lk{mu_};
    if (!key_ok(key)) {
        log(m.name, std::string("set_tip_seen: \"") + (key ? key : "") + "\" isn't a usable key (up to 32 letters, digits and _)");
        return;
    }
    if (m.dll.empty()) return;
    const std::string k = std::string("tip.") + key;
    if (!seen) {
        if (env_.seen_clear && env_.seen_clear(m.dll, k)) log(m.name, std::string("tip ") + key + " marked not seen: it can show again");
        return;
    }
    bool dropped = false;
    for (std::size_t i = items_.size(); i-- > 0;)
        if (items_[i].tip && items_[i].mod == m.id && items_[i].key == key) {
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            dropped = true;
        }
    const bool was = env_.seen_get && env_.seen_get(m.dll, k) == "seen";
    if (env_.seen_set) env_.seen_set(m.dll, k, "seen");
    if (!was || dropped) log(m.name, std::string("tip ") + key + " marked seen" + (dropped ? "; it was waiting, so it won't show" : ""));
}

uint32_t PopupQueue::status(const void* mod, bool keyboard) const {
    std::scoped_lock lk{mu_};
    uint32_t b = 0;
    if (available_ == 1) b |= FH2_UI_TOASTS;
    const auto off = mod_off_.find(mod);
    if (!platform_toasts_ || (off != mod_off_.end() && off->second) || option_ == 0) b |= FH2_UI_TOASTS_OFF;
    if (!tips_) b |= FH2_UI_TIPS_OFF;
    if (on_screen_) b |= FH2_UI_ON_SCREEN;
    if (suspended_) b |= FH2_UI_GAME_HUD_HIDDEN;
    if (keyboard) b |= FH2_UI_KEYBOARD;
    return b;
}

// ---- the platform -----------------------------------------------------------------------------------------------

void PopupQueue::note_callback(const void* mod, double now) {
    std::scoped_lock lk{mu_};
    last_callback_[mod] = now;
}

void PopupQueue::faulted(const PopupMod& m) {
    std::scoped_lock lk{mu_};
    int n = 0;
    for (std::size_t i = items_.size(); i-- > 0;)
        if (items_[i].mod == m.id) {
            items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
            ++n;
        }
    if (n) log(m.name, m.name + " faulted, so its waiting pop-ups were dropped (" + std::to_string(n) + ")");
}

void PopupQueue::set_available(int state, const std::string& why) {
    std::scoped_lock lk{mu_};
    if (state == available_) return;
    available_ = state;
    if (state != 0) return;
    why_off_ = why.empty() ? "pop-ups can't show this session" : why;
    for (const Item& it : items_)
        log(it.mod_name, it.tip ? tip_name(it) + " dropped: " + why_off_ + "; it isn't marked seen" : "pop-up dropped: " + why_off_ + " (" + text_of(it) + ")");
    items_.clear();
}

int PopupQueue::available() const {
    std::scoped_lock lk{mu_};
    return available_;
}

void PopupQueue::set_watch(bool on) {
    std::scoped_lock lk{mu_};
    watch_ = on;
}

void PopupQueue::set_platform_toasts(bool on) {
    std::scoped_lock lk{mu_};
    platform_toasts_ = on;
}

void PopupQueue::set_mod_toasts(const void* mod, bool on) {
    std::scoped_lock lk{mu_};
    mod_off_[mod] = !on;
}

void PopupQueue::set_tips(bool on) {
    std::scoped_lock lk{mu_};
    tips_ = on;
}

int PopupQueue::tips_again() {
    std::scoped_lock lk{mu_};
    const int n = env_.seen_clear_tips ? env_.seen_clear_tips() : 0;
    log("Pop-ups", "every mod's tips can show again (" + std::to_string(n) + " forgotten)");
    return n;
}

bool PopupQueue::lane_free(double now) const { return watch_ ? now - last_show_ >= kShowGapMs : now - last_own_show_ >= kOwnGapMs; }

void PopupQueue::show_seen(double now) {
    std::scoped_lock lk{mu_};
    last_show_ = now;
}

// ---- text -------------------------------------------------------------------------------------------------------

std::string PopupQueue::prepare(const std::string& s, std::size_t limit, bool capitals, bool wide) {
    std::string t = one_line(s);
    if (capitals) t = env_.upper ? env_.upper(t) : ascii_upper(t);
    if (!wide) t = popup_fold_ascii(t);
    if (text_chars(t) > limit) t = cut_text(t, limit - (wide ? 1 : 3)) + (wide ? "\xE2\x80\xA6" : "...");
    return t;
}

std::string PopupQueue::tr(const char* english) { return env_.tr ? env_.tr(english) : english; }

std::string PopupQueue::caps(const std::string& s) { return env_.upper ? env_.upper(s) : ascii_upper(s); }

std::string PopupQueue::word(int which, bool wide) {
    const std::string w = caps(tr(kWords[which]));
    if (wide) return w;
    const auto marks = [](const std::string& s) { return std::count(s.begin(), s.end(), '?'); };
    return marks(popup_fold_ascii(w)) == marks(w) ? w : ascii_upper(kWords[which]);
}

std::string PopupQueue::key_text(uint32_t vk, uint32_t modifiers, bool wide) {
    // The name on the player's keyboard layout, as the keycap prints it ("ENTF" on a German keyboard, whatever the
    // game's language) and as the hotkey rows name the key; "" if there's none or it isn't plain ASCII (a Cyrillic or
    // CJK name would be boxes or question marks).
    const auto on_layout = [&]() -> std::string {
        const std::string n = env_.key_name ? env_.key_name(vk) : "";
        for (const char c : n)
            if (c < 0x20 || c > 0x7E) return "";
        return ascii_upper(n);
    };
    const auto layout_or = [&](const char* english) {
        const std::string n = on_layout();
        return n.empty() ? std::string(english) : n;
    };
    std::string name;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) name = std::string(1, static_cast<char>(vk));
    else if (vk >= 0x70 && vk <= 0x87) name = "F" + std::to_string(vk - 0x70 + 1);
    else if (vk >= 0x60 && vk <= 0x69) name = "NUM " + std::to_string(vk - 0x60);
    else
        switch (vk) {
            // the space bar has no label, and the arrows' keycaps carry arrows the game's fonts haven't got: the
            // platform's words, in the game's language
            case 0x20: name = word(kSpace, wide); break;
            case 0x25: name = word(kLeft, wide); break;
            case 0x26: name = word(kUp, wide); break;
            case 0x27: name = word(kRight, wide); break;
            case 0x28: name = word(kDown, wide); break;
            // keys whose keycaps differ from one keyboard to another: the layout's name, else the English one
            case 0x08: name = layout_or("BACKSPACE"); break;
            case 0x0D: name = layout_or("ENTER"); break;
            case 0x14: name = layout_or("CAPS LOCK"); break;
            case 0x21: name = layout_or("PAGE UP"); break;
            case 0x22: name = layout_or("PAGE DOWN"); break;
            case 0x23: name = layout_or("END"); break;
            case 0x24: name = layout_or("HOME"); break;
            case 0x2D: name = layout_or("INSERT"); break;
            case 0x2E: name = layout_or("DELETE"); break;
            case 0x90: name = layout_or("NUM LOCK"); break;
            case 0x91: name = layout_or("SCROLL LOCK"); break;
            // the same everywhere, or (Pause, Print Screen) keys Windows can't name from their virtual-key code
            case 0x09: name = "TAB"; break;
            case 0x13: name = "PAUSE"; break;
            case 0x1B: name = "ESC"; break;
            case 0x2C: name = "PRINT SCREEN"; break;
            case 0x10: case 0xA0: case 0xA1: name = "SHIFT"; break;
            case 0x11: case 0xA2: case 0xA3: name = "CTRL"; break;
            case 0x12: case 0xA4: case 0xA5: name = "ALT"; break;
            case 0x6A: name = "NUM *"; break;
            case 0x6B: name = "NUM +"; break;
            case 0x6D: name = "NUM -"; break;
            case 0x6E: name = "NUM ."; break;
            case 0x6F: name = "NUM /"; break;
            default: {
                // any other key: its name on the layout, otherwise its code
                name = on_layout();
                if (name.empty()) {
                    char b[8];
                    std::snprintf(b, sizeof b, "0x%02X", vk);
                    name = b;
                }
            }
        }
    if (text_chars(name) > kKeyNameChars) name = cut_text(name, kKeyNameChars);
    std::string s;
    if (modifiers & FH2_KEY_CTRL) s += "[CTRL] + ";
    if (modifiers & FH2_KEY_SHIFT) s += "[SHIFT] + ";
    if (modifiers & FH2_KEY_ALT) s += "[ALT] + ";
    return s + "[" + name + "]";
}

std::string PopupQueue::glyph_text(uint32_t g, bool wide) {
    static const char* const kNames[] = {"", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "MENU", "VIEW"};
    if (g >= 1 && g <= 10) return std::string("(") + kNames[g] + ")";
    const std::string up = word(kUp, wide), down = word(kDown, wide), left = word(kLeft, wide), right = word(kRight, wide);
    const std::string up_down = up + "/" + down, left_right = left + "/" + right;
    // the d-pad as the game's own text names it in each language ("Steuerkreuz", "BMD", "Cruceta")
    const std::string dpad = "(" + word(kDpad, wide) + " ";
    switch (g) {
        case FH2_GLYPH_DPAD_UP: return dpad + up + ")";
        case FH2_GLYPH_DPAD_DOWN: return dpad + down + ")";
        case FH2_GLYPH_DPAD_LEFT: return dpad + left + ")";
        case FH2_GLYPH_DPAD_RIGHT: return dpad + right + ")";
        case FH2_GLYPH_DPAD_UPDOWN: return dpad + up_down + ")";
        case FH2_GLYPH_DPAD_LEFTRIGHT: return dpad + left_right + ")";
        case FH2_GLYPH_L3: case FH2_GLYPH_LS: return "(LS)";
        case FH2_GLYPH_R3: case FH2_GLYPH_RS: return "(RS)";
        case FH2_GLYPH_LS_UPDOWN: return "(LS " + up_down + ")";
        case FH2_GLYPH_LS_LEFTRIGHT: return "(LS " + left_right + ")";
        case FH2_GLYPH_RS_UPDOWN: return "(RS " + up_down + ")";
        case FH2_GLYPH_RS_LEFTRIGHT: return "(RS " + left_right + ")";
        default: return "";
    }
}

// A controller chord as up to three glyphs, in the hotkey rows' order; a fourth button and beyond as "…".
std::string PopupQueue::chord_text(uint32_t chord, bool wide) {
    static const uint32_t kOrder[][2] = {
        {FH2_PAD_LB, FH2_GLYPH_LB},         {FH2_PAD_RB, FH2_GLYPH_RB},           {FH2_PAD_Y, FH2_GLYPH_Y},
        {FH2_PAD_X, FH2_GLYPH_X},           {FH2_PAD_B, FH2_GLYPH_B},             {FH2_PAD_A, FH2_GLYPH_A},
        {FH2_PAD_DPAD_UP, FH2_GLYPH_DPAD_UP}, {FH2_PAD_DPAD_DOWN, FH2_GLYPH_DPAD_DOWN}, {FH2_PAD_DPAD_LEFT, FH2_GLYPH_DPAD_LEFT},
        {FH2_PAD_DPAD_RIGHT, FH2_GLYPH_DPAD_RIGHT}, {FH2_PAD_START, FH2_GLYPH_MENU}, {FH2_PAD_BACK, FH2_GLYPH_VIEW},
        {FH2_PAD_LTHUMB, FH2_GLYPH_L3},     {FH2_PAD_RTHUMB, FH2_GLYPH_R3},
    };
    std::string s;
    int n = 0;
    for (const auto& o : kOrder) {
        if (!(chord & o[0])) continue;
        if (++n > 3) {
            s += " + \xE2\x80\xA6";
            break;
        }
        s += (s.empty() ? "" : " + ") + glyph_text(o[1], wide);
    }
    return s;
}

// The tip's button for the device used last: a hotkey's binding now, else the prompt's key or glyph. "" when there's
// nothing to write (a hotkey bound to nothing).
std::string PopupQueue::button_text(const Item& it, bool keyboard, bool wide) {
    uint32_t vk = it.button.vk, mods = it.button.modifiers, chord = 0, glyph = it.button.glyph;
    if (!it.hotkey.empty()) {
        vk = mods = glyph = 0;
        if (!env_.hotkey || !env_.hotkey(it.mod, it.hotkey, &vk, &mods, &chord)) {
            PopupMod m;
            m.name = it.mod_name;
            log_once(m, "tip hotkey " + it.hotkey, tip_name(it) + ": there's no hotkey called " + it.hotkey + ", so its button can't be written");
            return "";
        }
    }
    if (keyboard && vk) return key_text(vk, mods, wide);
    if (chord) return chord_text(chord, wide);
    if (glyph) return glyph_text(glyph, wide);
    return vk ? key_text(vk, mods, wide) : "";
}

void PopupQueue::note_shown(const Item& it, double now, bool private_title) {
    PopupRecent r;
    r.mod   = it.mod_name;
    r.when  = env_.clock_text ? env_.clock_text() : "";
    r.title = private_title ? "(private)" : it.tip ? it.shown_line : it.shown_title;
    r.tip   = it.tip;
    r.priv  = private_title;
    recent_.insert(recent_.begin(), r);
    if (recent_.size() > kRecent) recent_.resize(kRecent);
    on_screen_items_.erase(std::remove_if(on_screen_items_.begin(), on_screen_items_.end(), [&](const OnScreen& s) { return now - s.at >= kOnScreenMs; }),
                           on_screen_items_.end());
    if (!it.tip) on_screen_items_.push_back({it.mod, it.mod_name, it.key, it.title, it.line, it.icon, private_title, now});
}

// ---- the tick ---------------------------------------------------------------------------------------------------

PopupShow PopupQueue::tick(const PopupTick& in) {
    std::scoped_lock lk{mu_};
    const double now = in.now;
    const double dt  = last_tick_ < 0 ? 0 : std::clamp(now - last_tick_, 0.0, kTickStepMs);
    last_tick_       = now;
    on_screen_       = in.on_screen;
    suspended_       = in.suspended;
    option_          = in.option;
    if (in.on_screen && first_screen_ < 0) first_screen_ = now;
    flush_rated(now, false);
    if (available_ != 1 || inflight_.id) return {};
    const bool could     = in.on_screen && !in.suspended;
    const bool any_toast = std::any_of(items_.begin(), items_.end(), [](const Item& w) { return !w.tip; });
    const bool quiet     = now - last_show_ >= kShowGapMs && now - last_own_show_ >= kShowGapMs;
    // time counted, and expiry, only while a pop-up could have shown
    for (std::size_t i = items_.size(); i-- > 0;) {
        Item& w = items_[i];
        if (!could) {
            w.held |= in.on_screen ? kHeldSuspended : kHeldOffScreen;
            continue;
        }
        if (w.tip) {
            if (quiet && !any_toast) w.drive += dt;
            continue;
        }
        w.waited += dt;
        if ((w.flags & FH2_TOAST_FLAG_REPLY) && w.waited >= kReplyWaitMs) drop(i, "a reply that couldn't show within 6 s", now, false);
        else if (w.waited >= w.expire_ms) drop(i, "it could have shown for " + seconds(w.expire_ms) + " and didn't", now, false);
    }
    if (!could || items_.empty()) return {};
    if (!lane_free(now)) {
        for (Item& w : items_)
            if (!w.tip) w.held |= kHeldLane;
        return {};
    }
    if (!in.ready) {
        for (Item& w : items_)
            if (!w.tip) w.held |= kHeldNotReady;
        return {};
    }
    // the game's own option: with it off the game shows nothing, so nothing is sent
    if (option_ == 0) {
        while (!items_.empty()) drop(items_.size() - 1, "the game's Notifications setting is off (Settings > HUD)", now, true);
        return {};
    }
    // the next pop-up: URGENT, then REPLY, then the rest as they came
    const auto rank = [](const Item& w) { return (w.flags & FH2_TOAST_FLAG_URGENT) ? 0 : (w.flags & FH2_TOAST_FLAG_REPLY) ? 1 : 2; };
    std::size_t pick = items_.size();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const Item& w = items_[i];
        if (w.tip) continue;
        if (pick == items_.size() || rank(w) < rank(items_[pick]) || (rank(w) == rank(items_[pick]) && w.seq < items_[pick].seq)) pick = i;
    }
    PopupShow out;
    if (pick == items_.size()) {
        // no pop-up waiting: a tip, if one is due
        if (!tips_ || first_screen_ < 0 || now - first_screen_ < kAnnounceMs || now - last_tip_ < kTipGapMs || tips_shown_ >= kTipsPerSession) return {};
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item& w = items_[i];
            if (!w.tip || w.drive < w.delay_ms || tipped_[w.mod]) continue;
            if (pick == items_.size() || w.seq < items_[pick].seq) pick = i;
        }
        if (pick == items_.size()) return {};
        const std::string button = items_[pick].has_button ? button_text(items_[pick], in.keyboard, in.wide) : "";
        if (items_[pick].has_button && button.empty()) {
            drop(pick, "its button can't be written (its hotkey is bound to nothing now)", now, false);
            return {};
        }
        std::string line = items_[pick].line;
        for (std::size_t at; (at = line.find("{button}")) != std::string::npos;) line.replace(at, 8, button);
        out.tip   = true;
        out.title = prepare(items_[pick].mod_name, kTitleChars, true, in.wide);
        out.line  = prepare(line, kLineChars, false, in.wide);
    } else {
        const Item& w = items_[pick];
        out.title     = prepare(w.title, kTitleChars, true, in.wide);
        out.line      = prepare(w.line, kLineChars, false, in.wide);
        out.icon      = kIconNames[w.icon];
        out.sound     = w.sound;
        for (Item& o : items_)
            if (!o.tip && o.id != w.id) o.held |= kHeldAhead;
    }
    inflight_ = items_[pick];
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(pick));
    out.id  = inflight_.id;
    out.mod = inflight_.mod_name;
    if (!in.wide) {
        bool folded = false;
        popup_fold_ascii(out.tip ? inflight_.line : inflight_.title + inflight_.line, &folded);
        if (folded) {
            PopupMod m;
            m.name = inflight_.mod_name;
            log_once(m, "folded", "has text beyond ASCII in a pop-up: the game's notification gets it with plain letters (\"\xC5\x82\" as \"l\"), "
                                  "since the platform's wide text isn't on");
        }
    }
    inflight_.shown_title = out.title;   // for the log line once it's settled
    inflight_.shown_line  = out.line;
    last_own_show_ = now;
    return out;
}

void PopupQueue::outcome(uint64_t id, PopupOutcome what, double now) {
    std::scoped_lock lk{mu_};
    if (!id || inflight_.id != id) return;
    Item it   = inflight_;
    inflight_ = Item{};
    if (what == PopupOutcome::failed) {
        log(it.mod_name, it.tip ? tip_name(it) + " not shown: showing it failed (see the platform's line); it isn't marked seen"
                                : "pop-up not shown: showing it failed (see the platform's line) (" + text_of(it) + ")");
        return;
    }
    if (what == PopupOutcome::refused) {
        log(it.mod_name, it.tip ? tip_name(it) + " not shown: the game didn't draw it (its Notifications setting is off?); it isn't marked seen"
                                : "pop-up not shown: the game didn't draw it (its Notifications setting is off?) (" + text_of(it) + ")");
        return;
    }
    const double waited = now - it.queued_at;
    const std::string after =
        it.held && waited >= 1000 ? "; it waited " + seconds(waited) + " for " + held_text(it.held) : std::string();
    if (!it.tip) {
        const bool priv = (it.flags & FH2_TOAST_FLAG_PRIVATE) != 0;
        const std::string key = it.key.empty() ? "" : "key " + it.key;
        const std::string shown =
            priv ? (key.empty() ? "no key" : key) : it.shown_title + (it.shown_line.empty() ? "" : " / " + it.shown_line) + (key.empty() ? "" : " (" + key + ")");
        log(it.mod_name, std::string("pop-up shown (game's look") + (priv ? ", private" : "") + "): " + shown + after);
        note_shown(it, now, priv);
        if (!watch_) last_show_ = std::max(last_show_, now);
        return;
    }
    ++tips_shown_;
    tipped_[it.mod] = true;
    last_tip_       = now;
    note_shown(it, now, false);
    const bool drawn = what == PopupOutcome::shown || option_ == 1;
    const std::string name = tip_name(it);
    if (it.key.empty() || it.dll.empty() || !env_.seen_set) {
        log(it.mod_name, name + " shown: " + it.shown_line + after);
    } else if (!drawn) {
        log(it.mod_name, name + " sent: " + it.shown_line +
                             "; whether the game drew it can't be told (its Notifications setting can't be read), so it isn't marked seen" + after);
    } else if (it.tip_flags & FH2_TIP_FLAG_UNTIL_DONE) {
        const std::string k = "tip." + it.key;
        const long n        = std::strtol((env_.seen_get ? env_.seen_get(it.dll, k) : std::string()).c_str(), nullptr, 10) + 1;
        env_.seen_set(it.dll, k, std::to_string(n));
        log(it.mod_name, name + " shown (session " + std::to_string(n) + " of " + std::to_string(it.max_sessions) +
                             "), seen once the mod says it's done: " + it.shown_line + after);
    } else {
        env_.seen_set(it.dll, "tip." + it.key, "seen");
        log(it.mod_name, name + " shown, now seen: " + it.shown_line + after);
    }
    if (!watch_) last_show_ = std::max(last_show_, now);
}

// ---- reading it -------------------------------------------------------------------------------------------------

std::vector<PopupRecent> PopupQueue::recent() const {
    std::scoped_lock lk{mu_};
    return recent_;
}

std::size_t PopupQueue::waiting() const {
    std::scoped_lock lk{mu_};
    return items_.size();
}

std::string PopupQueue::describe(double now) const {
    std::scoped_lock lk{mu_};
    std::string s = "pop-ups: ";
    s += available_ == 1 ? "can show" : available_ == 0 ? "off this session: " + why_off_ : "not ready yet";
    s += std::string("; the watch on the game's own: ") + (watch_ ? "in" : "not in (5.5 s between the platform's)");
    s += std::string("; the game's Notifications setting: ") + (option_ == 1 ? "on" : option_ == 0 ? "off" : "not known");
    s += std::string("; the HUD page: ") + (on_screen_ ? "on screen" : "not on screen") + (suspended_ ? ", suspended" : "") + "\n";
    if (!platform_toasts_) s += "the player turned mod pop-ups off\n";
    if (!tips_) s += "the player turned tips off\n";
    // what's on screen, a PRIVATE one by its key only, as the log has it (this goes into bug reports)
    for (const OnScreen& o : on_screen_items_) {
        if (now - o.at >= kOnScreenMs) continue;
        const std::string key = o.key.empty() ? "" : "key " + o.key;
        const std::string text = o.priv ? (key.empty() ? "no key" : key) + " (private)"
                                        : one_line(o.title) + (o.line.empty() ? "" : " / " + one_line(o.line)) + (key.empty() ? "" : " (" + key + ")");
        s += o.mod_name + ": on screen: " + text + "\n";
    }
    if (now - last_show_ < kShowGapMs) s += "a Show was seen " + seconds(now - last_show_) + " ago\n";
    for (const Item& w : items_) {
        if (w.tip) {
            s += w.mod_name + ": " + tip_name(w) + " waiting, " + seconds(w.drive) + " of " + seconds(w.delay_ms) + " of driving\n";
            continue;
        }
        const Item& c = w;
        s += c.mod_name + ": pop-up waiting " + seconds(now - c.queued_at) + " (" + seconds(c.waited) + " of " + seconds(c.expire_ms) +
             " counted): " + text_of(c) + (c.held ? "; held by " + held_text(c.held) : "") + "\n";
    }
    if (items_.empty()) s += "nothing waiting\n";
    s += std::to_string(tips_shown_) + " of 3 tips shown this session\n";
    return s;
}

// ---- the Recent pop-ups page -------------------------------------------------------------------------------------

std::vector<RecentRow> recent_page(const std::vector<PopupRecent>& recent, const std::function<std::string(const char*)>& tr) {
    std::vector<RecentRow> rows;
    for (const PopupRecent& r : recent) {
        RecentRow row;
        row.label       = r.priv ? tr("(private)") : r.title;
        row.value       = r.when;
        row.description = fill(r.tip ? tr("A tip from {0}.") : tr("A pop-up from {0}."), {r.mod});
        rows.push_back(row);
    }
    if (rows.empty())
        rows.push_back({tr("No pop-ups yet"), "", tr("The pop-ups and tips mods show while you drive are listed here, newest first, with the mod "
                                                       "that sent each.")});
    return rows;
}

} // namespace fh2platform

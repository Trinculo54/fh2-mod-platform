// The UI templates' rules for message boxes, UI sounds and page help and hints: see ui_rules.hpp.
#include "ui_rules.hpp"

#include "text.hpp"

#include <algorithm>
#include <cstddef>

namespace fh2platform {

std::string cut_ellipsis(const std::string& s, std::size_t chars) {
    if (text_chars(s) <= chars) return s;
    return chars ? cut_text(s, chars - 1) + "\xE2\x80\xA6" : std::string();
}

// ---- message boxes -----------------------------------------------------------------------------------------------

namespace {

// A member of a struct that starts with `size`: there only when the caller's struct is big enough.
#define HAS_MEMBER(s, S, m) ((s)->size >= offsetof(S, m) + sizeof((s)->m))

std::string cut_noted(const char* given, std::size_t chars, const std::string& what, std::vector<std::string>& cut) {
    const std::string s = given ? given : "";
    const std::string c = cut_ellipsis(s, chars);
    if (c != s) cut.push_back(what);
    return c;
}

} // namespace

bool read_box(const FH2MessageBox* box, BoxSpec& out, std::string& why) {
    out = BoxSpec{};
    if (!box || !HAS_MEMBER(box, FH2MessageBox, choice_count)) {
        why = "the FH2MessageBox is missing or its size is too small (set size = sizeof(FH2MessageBox))";
        return false;
    }
    if (!box->title || !*box->title) {
        why = "the box has no title";
        return false;
    }
    if (!box->choices || box->choice_count < 1) {
        why = "the box has no choices (give at least one; the platform adds Back)";
        return false;
    }
    if (box->choice_count > kBoxMaxChoices) {
        why = "a box can have 1 to " + std::to_string(kBoxMaxChoices) + " choices (the platform adds Back), not " + std::to_string(box->choice_count);
        return false;
    }
    for (uint32_t i = 0; i < box->choice_count; ++i)
        if (!box->choices[i] || !*box->choices[i]) {
            why = "choice " + std::to_string(i) + " has no text";
            return false;
        }
    out.key     = box->key ? box->key : "";
    out.title   = cut_noted(box->title, kBoxTitleChars, "the title", out.cut);
    out.message = cut_noted(box->message, kBoxMessageChars, "the message", out.cut);
    for (uint32_t i = 0; i < box->choice_count; ++i)
        out.choices.push_back(cut_noted(box->choices[i], kBoxChoiceChars, "choice " + std::to_string(i), out.cut));
    if (HAS_MEMBER(box, FH2MessageBox, focus)) out.focus = box->focus < box->choice_count ? box->focus : 0;
    if (HAS_MEMBER(box, FH2MessageBox, on_choose)) out.on_choose = box->on_choose;
    if (HAS_MEMBER(box, FH2MessageBox, user)) out.user = box->user;
    return true;
}

int box_pages(std::size_t n) {
    return n <= kBoxFits ? 1 : static_cast<int>((n + kBoxPageSize - 1) / kBoxPageSize);
}

int box_page_of(std::size_t n, uint32_t choice) {
    if (n <= kBoxFits || choice >= n) return 0;
    return static_cast<int>(choice / kBoxPageSize);
}

std::vector<int> box_page_items(std::size_t n, int p) {
    std::vector<int> items;
    const int pages = box_pages(n);
    p               = std::clamp(p, 0, pages - 1);
    std::size_t from = 0, to = n;
    if (pages > 1) {
        from = static_cast<std::size_t>(p) * kBoxPageSize;
        to   = std::min(n, from + kBoxPageSize);
        if (p > 0) items.push_back(kBoxItemPrevious);
    }
    for (std::size_t i = from; i < to; ++i) items.push_back(static_cast<int>(i));
    if (to < n) items.push_back(kBoxItemNext);
    items.push_back(kBoxItemBack);
    return items;
}

// ---- UI sounds ---------------------------------------------------------------------------------------------------

// SELECT, OPEN and CLOSE are what the platform's menus play, BACK what a page's B plays; the other seven are the
// game's own name slots (S4), each checked against the exe on disk before it's used.
const UiSoundInfo kUiSounds[FH2_SOUND_SHUTTER + 1] = {
    {"", "", 0, 0, false},
    {"SELECT", "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_Select_01", 0, 0, false},
    {"BACK", "/Master/UI/PopUpMenu/HZ2_SFX_HUD_PopUpMenu_Back_01", 0, 0, false},
    {"OPEN", "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_SubMenuOpen_01", 0, 0, false},
    {"CLOSE", "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_SubMenuClose_01", 0, 0, false},
    {"ERROR", "/Master/UI/PauseMenu/HZ2_SFX_UI_PauseMenu_Fail_01", 0x2C5EFB8, 0x1E0A350, false},
    {"NOTIFY", "/Master/UI/Notification/HZ2_SFX_HUD_Notification_GenericOn_01", 0x2C5F008, 0x1E0A5F8, true},
    {"INFO", "/Master/UI/PopUpInfo/HZ2_SFX_HUD_PopUpInfo_Open_01", 0x2C5F068, 0x1E0A568, false},
    {"TICK", "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_High_01", 0x2C5F208, 0x1E0B018, true},
    {"TICK_LOW", "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_Low_01", 0x2C5F210, 0x1E0B050, true},
    {"TICK_END", "/Master/UI/Countdown/HZ2_SFX_HUD_Countdown_End_01", 0x2C5F218, 0x1E0B088, true},
    {"SHUTTER", "/Master/UI/Promo/HZ2_SFX_HUD_Promo_TakePhoto_01", 0x2C5F0C0, 0x1E0A9E8, false},
};

UiSoundRules::Ask UiSoundRules::ask(const void* mod, uint32_t sound, double now, bool in_callback, int on_screen) {
    if (!ui_sound_known(sound)) return Ask::unknown;
    if (kUiSounds[sound].world_only && (in_callback || on_screen == 0)) return Ask::world_only;
    std::scoped_lock lk{mu_};
    const auto key = std::make_pair(mod, sound);
    if (const auto it = last_.find(key); it != last_.end() && now - it->second < kRepeatMs) return Ask::repeat;
    last_[key] = now;
    if (in_callback) return Ask::play_now;
    std::size_t mine = 0;
    for (const Waiting& w : queue_) mine += w.mod == mod;
    if (mine >= kPerMod)
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (it->mod == mod) {   // the oldest of its own goes
                queue_.erase(it);
                break;
            }
    queue_.push_back({mod, sound, now});
    return Ask::queued;
}

bool UiSoundRules::tick(double now, int on_screen, Due& out, std::vector<Dropped>& dropped) {
    std::scoped_lock lk{mu_};
    while (!queue_.empty()) {
        const Waiting w = queue_.front();
        queue_.pop_front();
        if (now - w.at > kLateMs) {
            dropped.push_back({w.mod, w.sound, Drop::late});
            continue;
        }
        if (kUiSounds[w.sound].world_only && on_screen == 0) {
            dropped.push_back({w.mod, w.sound, Drop::off_screen});
            continue;
        }
        out = {w.mod, w.sound};
        return true;
    }
    return false;
}

std::size_t UiSoundRules::waiting() const {
    std::scoped_lock lk{mu_};
    return queue_.size();
}

std::size_t UiSoundRules::forget(const void* mod) {
    std::scoped_lock lk{mu_};
    const std::size_t before = queue_.size();
    std::erase_if(queue_, [mod](const Waiting& w) { return w.mod == mod; });
    return before - queue_.size();
}

// ---- page help and hint lines ------------------------------------------------------------------------------------

const char* glyph_name(uint32_t glyph) {
    static const char* const kNames[] = {"none", "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Menu", "View", "d-pad up", "d-pad down",
                                         "d-pad left", "d-pad right", "d-pad up/down", "d-pad left/right", "L3", "R3", "the left stick",
                                         "the left stick up/down", "the left stick left/right", "the right stick", "the right stick up/down",
                                         "the right stick left/right"};
    return glyph < sizeof kNames / sizeof kNames[0] ? kNames[glyph] : "an unknown glyph";
}

std::vector<HelpPrompt> accept_prompts(const FH2Prompt* prompts, uint32_t count, const HelpPage& page, std::vector<std::string>& refused,
                                       std::vector<std::string>& notes) {
    HelpPrompt by_glyph[FH2_GLYPH_RB + 1];
    bool ignored = false;
    for (uint32_t i = 0; prompts && i < count; ++i) {
        const FH2Prompt& p = prompts[i];
        const std::string which = "prompt " + std::to_string(i) + " (" + glyph_name(p.glyph) + ")";
        bool reserved = false;
        for (uint32_t r : p.reserved) reserved = reserved || r;
        if (reserved) {
            refused.push_back(which + ": its reserved members aren't zero");
            continue;
        }
        if (p.glyph < FH2_GLYPH_A || p.glyph > FH2_GLYPH_RB) {
            refused.push_back(which + ": no callback of a page can answer that button");
            continue;
        }
        if (!page.custom && p.glyph != FH2_GLYPH_A && p.glyph != FH2_GLYPH_B) {
            refused.push_back(which + ": a page of rows answers only A and B");
            continue;
        }
        if (page.custom && !page.on_button && p.glyph != FH2_GLYPH_B) {
            refused.push_back(which + ": the page has no on_button to answer it");
            continue;
        }
        if (!page.custom && p.glyph == FH2_GLYPH_A && !page.on_select) {
            refused.push_back(which + ": the page has no on_select to answer A");
            continue;
        }
        if (p.word > FH2_WORD_NEXT) {
            refused.push_back(which + ": " + std::to_string(p.word) + " isn't one of the game's words (FH2_WORD_)");
            continue;
        }
        if (!p.word && (!p.label || !*p.label)) {
            refused.push_back(which + ": it has neither a word nor a label");
            continue;
        }
        if (p.vk || p.modifiers || p.hotkey || p.hold) ignored = true;
        HelpPrompt h;
        h.glyph = p.glyph;
        h.word  = p.word;
        if (!p.word) {
            h.label = cut_ellipsis(p.label, kPromptLabelChars);
            if (h.label != p.label) notes.push_back(which + "'s label is cut to \"" + h.label + "\"");
        }
        by_glyph[p.glyph] = h;
    }
    if (ignored) notes.push_back("vk, modifiers, hotkey and hold are ignored on pages, which the controller drives");
    // the game's order: A, B, X, Y, LB, RB (Global_Controls' ControllerButtonPanel)
    std::vector<HelpPrompt> out;
    for (uint32_t g = FH2_GLYPH_A; g <= FH2_GLYPH_RB; ++g)
        if (by_glyph[g].glyph) out.push_back(by_glyph[g]);
    return out;
}

std::vector<std::string> read_hints(const char* const* texts, uint32_t count, std::vector<std::string>& notes) {
    std::vector<std::string> out;
    if (count > kMaxHints) notes.push_back(std::to_string(count) + " hints given; a page keeps the first " + std::to_string(kMaxHints));
    for (uint32_t i = 0; texts && i < count && i < kMaxHints; ++i) {
        const std::string t = texts[i] ? texts[i] : "";
        const std::string c = cut_ellipsis(t, kHintChars);
        if (c != t) notes.push_back("hint " + std::to_string(i) + " is cut to " + std::to_string(kHintChars) + " characters");
        out.push_back(c);
    }
    return out;
}

} // namespace fh2platform

// The rules of message boxes, UI sounds and pages' help bars and hint lines (src/ui_rules.cpp), with no game: what a
// box keeps and cuts, its pages, which sound plays at once, waits, repeats or is refused, which prompts a page takes,
// and how many hints. A fake clock drives the sounds. Exits 0 when every check passes.
#include "ui_rules.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

FH2Prompt prompt(uint32_t glyph, const char* label, uint32_t word = 0) {
    FH2Prompt p{};
    p.glyph = glyph;
    p.label = label;
    p.word  = word;
    return p;
}

bool items_are(const std::vector<int>& got, const std::vector<int>& want) { return got == want; }

} // namespace

int main() {
    // ---- the ellipsis ----
    check(cut_ellipsis("Short", 10) == "Short", "text that fits isn't cut");
    check(cut_ellipsis("0123456789AB", 10) == "012345678\xE2\x80\xA6", "longer text is cut to the limit, ending in an ellipsis");
    check(cut_ellipsis("\xC5\xBC\xC3\xB3\xC5\x82\xC5\xBC\xC3\xB3\xC5\x82", 4) == "\xC5\xBC\xC3\xB3\xC5\x82\xE2\x80\xA6",
          "limits count characters, not bytes (Polish letters are one each)");

    // ---- message boxes ----
    {
        static const char* const two[] = {"Keep my progress", "Reset"};
        FH2MessageBox b{sizeof b};
        b.key = "Reset"; b.title = "Reset your progress?"; b.message = "Every achievement goes back to locked."; b.choices = two;
        b.choice_count = 2; b.focus = 1;
        BoxSpec s;
        std::string why;
        check(read_box(&b, s, why) && s.key == "Reset" && s.choices.size() == 2 && s.focus == 1 && s.cut.empty(), "a box as given is read whole");
        b.focus = 7;
        check(read_box(&b, s, why) && s.focus == 0, "a focus past the choices starts on the first");
        b.title = "";
        check(!read_box(&b, s, why) && why == "the box has no title", "a box needs a title");
        b.title = "This title is a good deal longer than thirty characters";
        check(read_box(&b, s, why) && s.title.size() > 0 && s.cut.size() == 1 && s.cut[0] == "the title", "a long title is cut, and said so");
        b.choice_count = 0;
        check(!read_box(&b, s, why), "a box needs a choice");
        static const char* const blank[] = {"Fine", nullptr};
        b.choices = blank; b.choice_count = 2;
        check(!read_box(&b, s, why) && why == "choice 1 has no text", "every choice needs text");
        b.choices = two; b.choice_count = static_cast<uint32_t>(kBoxMaxChoices + 1);
        check(!read_box(&b, s, why), "a box has at most 99 choices, Back being the 100th");
        FH2MessageBox small{offsetof(FH2MessageBox, choices)};
        check(!read_box(&small, s, why) && !read_box(nullptr, s, why), "a struct too small (or none) is refused");
        // an older, shorter struct: on_choose and user aren't read
        FH2MessageBox older{offsetof(FH2MessageBox, focus) + sizeof(uint32_t)};
        older.title = "Old"; older.choices = two; older.choice_count = 2;
        older.on_choose = [](void*, const char*, uint32_t) {};
        check(read_box(&older, s, why) && s.on_choose == nullptr && s.key.empty(), "only `size` bytes of the struct are read");
    }
    check(box_pages(1) == 1 && box_pages(10) == 1 && box_pages(11) == 2 && box_pages(20) == 3 && box_pages(99) == 13, "pages: 10 fit, then 8 a page");
    check(items_are(box_page_items(2, 0), {0, 1, kBoxItemBack}), "a short box: its choices, then Back");
    check(items_are(box_page_items(20, 0), {0, 1, 2, 3, 4, 5, 6, 7, kBoxItemNext, kBoxItemBack}), "the first page: Next before Back");
    check(items_are(box_page_items(20, 1), {kBoxItemPrevious, 8, 9, 10, 11, 12, 13, 14, 15, kBoxItemNext, kBoxItemBack}),
          "a middle page: Previous first, Next before Back");
    check(items_are(box_page_items(20, 2), {kBoxItemPrevious, 16, 17, 18, 19, kBoxItemBack}), "the last page: no Next");
    check(items_are(box_page_items(20, 9), box_page_items(20, 2)), "a page past the end is the last");
    check(box_page_of(20, 17) == 2 && box_page_of(20, 0) == 0 && box_page_of(5, 4) == 0, "the focus's page");

    // ---- UI sounds ----
    {
        int a = 0, b = 0;
        UiSoundRules r;
        check(r.ask(&a, 0, 0, false, -1) == UiSoundRules::Ask::unknown && r.ask(&a, 12, 0, false, -1) == UiSoundRules::Ask::unknown,
              "sounds 1 to 11 only");
        check(r.ask(&a, FH2_SOUND_SELECT, 0, true, -1) == UiSoundRules::Ask::play_now, "from a callback: at once");
        check(r.ask(&a, FH2_SOUND_SELECT, 50, true, -1) == UiSoundRules::Ask::repeat, "the same sound within 100 ms: refused");
        check(r.ask(&b, FH2_SOUND_SELECT, 50, true, -1) == UiSoundRules::Ask::play_now, "another mod's isn't a repeat");
        check(r.ask(&a, FH2_SOUND_BACK, 50, true, -1) == UiSoundRules::Ask::play_now, "nor is another sound");
        check(r.ask(&a, FH2_SOUND_SELECT, 150, true, -1) == UiSoundRules::Ask::play_now, "after 100 ms it plays again");
        check(r.ask(&a, FH2_SOUND_NOTIFY, 200, true, -1) == UiSoundRules::Ask::world_only, "NOTIFY isn't played from a callback (a menu is up)");
        check(r.ask(&a, FH2_SOUND_TICK, 200, false, 0) == UiSoundRules::Ask::world_only, "nor a countdown beep with the driving view off screen");
        check(r.ask(&a, FH2_SOUND_ERROR, 200, true, 0) == UiSoundRules::Ask::play_now && r.ask(&a, FH2_SOUND_SHUTTER, 200, true, 0) == UiSoundRules::Ask::play_now,
              "ERROR and SHUTTER play under the menus");
        check(r.waiting() == 0, "nothing waits after callbacks");
        check(r.ask(&a, FH2_SOUND_TICK, 300, false, -1) == UiSoundRules::Ask::queued, "from a thread, with the page unknown: it waits for the tick");
        UiSoundRules::Due due;
        std::vector<UiSoundRules::Dropped> dropped;
        check(r.tick(316, 1, due, dropped) && due.mod == &a && due.sound == FH2_SOUND_TICK && dropped.empty(), "the tick plays it");
        check(!r.tick(332, 1, due, dropped), "once");
        r.ask(&a, FH2_SOUND_INFO, 1000, false, 1);
        r.ask(&b, FH2_SOUND_INFO, 1001, false, 1);
        check(r.tick(1010, 1, due, dropped) && due.mod == &a && r.tick(1020, 1, due, dropped) && due.mod == &b, "one a tick, oldest first");
        r.ask(&a, FH2_SOUND_NOTIFY, 2000, false, 1);
        check(!r.tick(2016, 0, due, dropped) && dropped.size() == 1 && dropped[0].why == UiSoundRules::Drop::off_screen,
              "a world-only sound whose page went off screen is dropped");
        dropped.clear();
        r.ask(&a, FH2_SOUND_SHUTTER, 3000, false, 1);
        check(!r.tick(3600, 1, due, dropped) && dropped.size() == 1 && dropped[0].why == UiSoundRules::Drop::late,
              "a sound that waited more than 0.5 s is dropped");
        dropped.clear();
        const uint32_t five[] = {FH2_SOUND_SELECT, FH2_SOUND_BACK, FH2_SOUND_OPEN, FH2_SOUND_CLOSE, FH2_SOUND_ERROR};
        for (uint32_t s : five) r.ask(&a, s, 4000, false, 1);
        check(r.waiting() == UiSoundRules::kPerMod, "four wait for a mod at most");
        check(r.tick(4010, 1, due, dropped) && due.sound == FH2_SOUND_BACK, "a fifth drops its oldest");
        // a mod stopped for online play (platform 2.9.0): its waiting sounds go, another mod's stay
        r.ask(&b, FH2_SOUND_INFO, 4020, false, 1);
        check(r.forget(&a) == 3 && r.waiting() == 1 && r.tick(4030, 1, due, dropped) && due.mod == &b && r.forget(&a) == 0,
              "forget: the stopped mod's waiting sounds go, another mod's play");
        check(kUiSounds[FH2_SOUND_NOTIFY].world_only && kUiSounds[FH2_SOUND_TICK_END].world_only && !kUiSounds[FH2_SOUND_INFO].world_only &&
                  kUiSounds[FH2_SOUND_SHUTTER].slot == 0x2C5F0C0 && std::string(kUiSounds[FH2_SOUND_SELECT].name) == "SELECT",
              "the sound table: the world-only four, the game's name slots");
    }

    // ---- page help ----
    {
        std::vector<std::string> refused, notes;
        FH2Prompt rows[] = {prompt(FH2_GLYPH_A, "Pick"), prompt(FH2_GLYPH_X, "Sort"), prompt(FH2_GLYPH_B, nullptr, FH2_WORD_BACK)};
        auto got = accept_prompts(rows, 3, {false, true, false}, refused, notes);
        check(got.size() == 2 && got[0].glyph == FH2_GLYPH_A && got[0].label == "Pick" && got[1].glyph == FH2_GLYPH_B && got[1].word == FH2_WORD_BACK,
              "a page of rows takes A and B");
        check(refused.size() == 1 && refused[0] == "prompt 1 (X): a page of rows answers only A and B", "and says why X isn't");
        refused.clear();
        got = accept_prompts(rows, 1, {false, false, false}, refused, notes);
        check(got.empty() && refused.size() == 1 && refused[0] == "prompt 0 (A): the page has no on_select to answer A", "A needs on_select");
        refused.clear();
        FH2Prompt own[] = {prompt(FH2_GLYPH_RB, "Next tab"), prompt(FH2_GLYPH_Y, "Reset"), prompt(FH2_GLYPH_LB, "Previous tab"), prompt(FH2_GLYPH_Y, "Clear"),
                           prompt(FH2_GLYPH_LT, "Zoom"), prompt(FH2_GLYPH_DPAD_LEFT, "Left")};
        got = accept_prompts(own, 6, {true, false, true}, refused, notes);
        check(got.size() == 3 && got[0].glyph == FH2_GLYPH_Y && got[0].label == "Clear" && got[1].glyph == FH2_GLYPH_LB && got[2].glyph == FH2_GLYPH_RB,
              "a page of its own design with on_button takes X, Y, LB and RB, in the game's order, the last for a button winning");
        check(refused.size() == 2 && refused[0] == "prompt 4 (LT): no callback of a page can answer that button", "LT and the d-pad aren't");
        refused.clear();
        got = accept_prompts(own, 3, {true, false, false}, refused, notes);
        check(got.empty() && refused.size() == 3, "without on_button only B");
        refused.clear();
        FH2Prompt odd[] = {prompt(FH2_GLYPH_B, nullptr), prompt(FH2_GLYPH_B, nullptr, 99), prompt(FH2_GLYPH_B, "A label longer than twenty-four characters")};
        odd[2].vk = 0x20;
        got = accept_prompts(odd, 3, {false, false, false}, refused, notes);
        check(refused.size() == 2 && refused[0] == "prompt 0 (B): it has neither a word nor a label" &&
                  refused[1] == "prompt 1 (B): 99 isn't one of the game's words (FH2_WORD_)",
              "a prompt needs a word the game has, or a label");
        check(got.size() == 1 && got[0].label == "A label longer than twe\xE2\x80\xA6", "a label is cut to 24 characters");
        check(notes.size() == 2 && notes[1] == "vk, modifiers, hotkey and hold are ignored on pages, which the controller drives", "vk is ignored, and said");
        refused.clear();
        FH2Prompt reserved = prompt(FH2_GLYPH_B, "Back");
        reserved.reserved[2] = 1;
        check(accept_prompts(&reserved, 1, {false, false, false}, refused, notes).empty() && refused.size() == 1, "reserved members must be zero");
        check(std::string(glyph_name(FH2_GLYPH_RS_LEFTRIGHT)) == "the right stick left/right" && std::string(glyph_name(99)) == "an unknown glyph",
              "glyph names for the log");
    }

    // ---- hint lines ----
    {
        std::vector<std::string> notes;
        const std::string long_hint(100, 'x');
        const char* texts[10] = {"Saved.", long_hint.c_str(), "3", "4", "5", "6", "7", "8", "9", "10"};
        const auto h = read_hints(texts, 10, notes);
        check(h.size() == kMaxHints && h[0] == "Saved." && h[1] == std::string(79, 'x') + "\xE2\x80\xA6", "8 hints of 80 characters at most");
        check(notes.size() == 2 && notes[0] == "10 hints given; a page keeps the first 8" && notes[1] == "hint 1 is cut to 80 characters", "and what was left out is said");
    }

    std::printf(g_failed ? "%d FAILED\n" : "ALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

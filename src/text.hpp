// Text the platform shows, in any language (platform 2.9.0): capitals, lengths counted in characters,
// sentences put together, and messages kept until the game has its language. See text.cpp.
#pragma once

#include "lang.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

namespace fh2platform {

// The Windows locale for one of the game's text language codes ("PL" -> "pl-PL", "CHT" -> "zh-TW"), or ""
// for a code the game doesn't have.
std::wstring locale_of(const char* code);

// UTF-8 `s` in capitals the way `code`'s language writes them (LCMapStringEx with LCMAP_UPPERCASE and
// LCMAP_LINGUISTIC_CASING on UTF-16), so "Osiągnięcia" becomes "OSIĄGNIĘCIA". Null or an unknown code uses
// the invariant locale. Bytes that aren't UTF-8 (text a mod wrote as Windows-1252) only get a-z in capitals,
// as before 2.9.0, so they still look the same.
std::string upper_text(const std::string& s, const char* code);

// How long UTF-8 `s` is in characters: UTF-16 units, as the game's text boxes hold it (a character beyond
// U+FFFF is two). A byte that isn't UTF-8 counts as one.
std::size_t text_chars(const std::string& s);
// `s` cut to at most `chars` characters (as text_chars counts them), never inside a character.
std::string cut_text(const std::string& s, std::size_t chars);

// Two sentences, or groups of them, one after the other: with a space between them, except after a
// Japanese or Chinese full stop (and the like), which has no space after it.
std::string join_sentences(const std::string& a, const std::string& b);

// `tmpl` with {0}, {1}... replaced by `args`, every time each appears, in one pass (so an argument
// holding "{1}" stays as it is). Anything else in braces ("{car:X}") is left alone.
std::string fill(const std::string& tmpl, const std::vector<std::string>& args);

// A message for the player that's put together before the game has its language (a mod that didn't
// start, a car pack's problem), kept as the English template and its arguments and translated when
// it's shown. An argument can be such a message itself (one level deep). Logs use english().
std::string later(const char* english_template, std::initializer_list<std::string> args = {});
// A message from later(): its template and arguments. False for any other text.
bool split_later(const std::string& message, std::string& english_template, std::vector<std::string>& args);
// A message from later() in English; any other text as it is.
std::string english(const std::string& message);

// A message from later() in the game's language now (any other text as it is).
inline std::string shown(const std::string& message) {
    std::string t;
    std::vector<std::string> args;
    if (!split_later(message, t, args)) return message;
    for (std::string& a : args) a = shown(a);
    return fill(tr(t.c_str()), args);
}

// `s` in capitals for the game's language now (invariant before the game has one).
inline std::string upper_game(const std::string& s) {
    char code[8];
    return upper_text(s, game_language(code, sizeof code) ? code : nullptr);
}

} // namespace fh2platform

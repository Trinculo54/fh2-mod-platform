// The platform's own text in the game's language, and the game's language (see lang.cpp).
#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>

namespace fh2platform {

// The game's text language code ("EN", "DE", "CHT"...), 1; or 0 before the game has one. The API's game_language.
int game_language(char* code, size_t size);
// `english` in the game's language now; itself before the game has one, or when there's no translation.
std::string tr(const char* english);
// The same, with {0}, {1}... filled in from `args`.
std::string trf(const char* english, std::initializer_list<std::string> args);
// For text the game asks for as it shows it: a pointer that stays valid.
const wchar_t* tr_w(const wchar_t* english);

} // namespace fh2platform

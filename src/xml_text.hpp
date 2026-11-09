// Text that mods give the platform, put into the XAML pages it writes (the MODS tab, Mods page rows,
// pages, the title screen and the HUD notice).
#pragma once
#include <string>

namespace fh2platform {

// `s` as text for a XAML attribute value: & < > " escaped, and a line break as a space (FZTextBlock
// doesn't break lines, not even at &#10;). Text that starts with { and ends with } gets a no-break
// space (&#160;) on the end, so the game doesn't take it for a markup extension ("{0}" stays text).
//
// Anything beyond ASCII is written as a character reference (&#183;). The game reads the platform's
// page files as Windows-1252, so UTF-8 written as it is came out as two or three wrong letters
// ("Circle · closer" showed as "Circle Â· closer"). fh2mod.h has always called this text UTF-8. A byte
// that doesn't start a valid UTF-8 sequence is read as Windows-1252, which is what the game showed
// for it before, so text a mod wrote that way still looks the same.
std::string xml_escape(const std::string& s);

} // namespace fh2platform

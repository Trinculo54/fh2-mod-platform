// Text that mods give the platform, put into the XAML pages it writes (the MODS tab, Mods page rows,
// pages, the title screen and the HUD notice).
#pragma once
#include <string>

namespace fh2platform {

// `s` as text for a XAML attribute value: & < > " escaped, and a line break or a tab as a space (FZTextBlock
// doesn't break lines). The other control characters but CR are left out (U+0080 to U+009F too), as XML has
// none of them. Text that starts with { and ends with } gets a no-break
// space (&#xa0;) on the end, so the game doesn't take it for a markup extension ("{0}" stays text).
//
// Anything beyond ASCII is written as a hex character reference with a lowercase x (&#xb7;): the only kind the
// game's XAML reader decodes. A decimal one (&#183;) comes out as a space, which is what 2.9.0 rc1 wrote. The
// game reads the platform's page files one byte a character, so UTF-8 written as it is came out as two or three
// wrong letters ("Circle · closer" showed as "Circle Â· closer"). fh2mod.h has always called this text UTF-8. A byte
// that doesn't start a valid UTF-8 sequence is read as Windows-1252, which is what the game showed
// for it before, so text a mod wrote that way still looks the same. A character beyond U+FFFF (an
// emoji, which the game's fonts don't have), U+FFFE, U+FFFF and the five bytes Windows-1252 leaves out come out as "?".
std::string xml_escape(const std::string& s);

} // namespace fh2platform

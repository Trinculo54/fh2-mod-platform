// Pages of a mod's own design (set_page_xaml, API 8), made safe to open (platform 2.9.0): the check each fragment
// passes against the game's own files before the platform takes it, and the crash guard, which leaves out a page
// the game was loading when it last stopped. The rules themselves are in xaml_safety.cpp; this is the platform's
// side: UI.zip and the texture zips read from the game folder (once, read-only), fh2-mods\platform-pages.txt, and
// the log lines.
#pragma once

#include <string>

struct FH2Mod;

namespace fh2platform {

// Checks a fragment for set_page_xaml (any part may be null). True when it may be used, false with `why` (a phrase
// for the log, naming the key or path) when the game would crash on it. UI.zip and the texture zips are read the
// first time, by whichever thread asks; when they can't be read, every fragment is let through (a log line says so
// once), since the page can't be written then either.
bool page_fragment_ok(const char* resources, const char* content, const char* help, std::string& why);

// The crash guard. page_guard_start reads last session's fh2-mods\platform-pages.txt as the menus are written: a
// page that was opening when the game stopped is left out from now on, until its mod's DLL changes.
void page_guard_start();
// Whether `page` of `mod` is left out this session.
bool page_guard_skipped(const FH2Mod* mod, const std::string& page);
// Before the game loads a page of `mod`'s own design: the mark, in the file. page_guard_loaded when the page has
// loaded takes the mark off 10 s later (unless another page has opened since); page_guard_closed, when the player
// has left it (another page loaded, or the pause menu is back), takes it off at once.
void page_guard_opening(const FH2Mod* mod, const std::string& page);
void page_guard_loaded();
void page_guard_closed();

} // namespace fh2platform

// What the player has already been shown (platform 2.9.0): fh2-mods\platform-seen.txt, the one store for every
// "only once" cue. The UI templates keep each mod's tips there (show_tip, tip_seen, set_tip_seen, the "Show tips
// again" row), and the settings restructure its own cues: whether the player has opened YOUR MODS, a mod that's new
// until its page is opened, the first-run welcome's session count. See seen.cpp for the file's format.
//
// Every call is thread-safe and quick: reads come from memory, and writes go to the file on a worker thread of the
// store's own, never on the caller's (the game's UI thread among them). The store only ever errs towards showing a
// cue again: a file that's missing, locked or can't be read reads as nothing seen, and a write that fails (or never
// happens, because Azimuth ends the game first) leaves the file as it was.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fh2platform {

// Who an entry is for: a mod by its DLL file name in fh2-mods ("WreckingBall.dll", compared in any case), never its
// display name (which can have spaces and change), or kSeenPlatform for the platform's own cues.
inline constexpr const char* kSeenPlatform = "*";

// Starts the store on `file` (the platform: <Azimuth>\fh2-mods\platform-seen.txt) and reads it. `log` gets the
// store's lines for platform.log (may be empty). Calling it again starts afresh on that file: what was set before
// and not yet written is dropped (the tests use this to read the file back).
void seen_open(const std::filesystem::path& file, std::function<void(const std::string&)> log = {});

// The value of (who, key), or "" when it isn't set: never seen, cleared, the store not open, or the file unreadable.
std::string seen_get(const std::string& who, const std::string& key);
inline bool seen_has(const std::string& who, const std::string& key) { return !seen_get(who, key).empty(); }

// Sets (who, key) to `value` ("seen", or a count such as "2"), dated today, and has the file written. Returns false,
// with nothing set, for an unusable who, key or value (see seen.cpp: empty, too long, a tab or a line break) or
// when the store isn't open.
bool seen_set(const std::string& who, const std::string& key, const std::string& value = "seen");

// Forgets (who, key). Returns true if it was set.
bool seen_clear(const std::string& who, const std::string& key);
// Forgets every entry `drop(who, key, value)` says to: every mod's tips for "Show tips again", say. Returns how many.
int seen_clear_if(const std::function<bool(const std::string& who, const std::string& key, const std::string& value)>& drop);

// Waits up to `timeout_ms` until every change so far is in the file. True when it is; false when it isn't yet (the
// file was locked, say: the store tries again at the next change). For tests, and before a planned restart.
bool seen_flush(uint32_t timeout_ms);

} // namespace fh2platform

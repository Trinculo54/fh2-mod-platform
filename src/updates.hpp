#pragma once

// Update checks and title-screen notices.
//
// Notices live in fh2-mods\notices\<mod>.txt (line 1: the mod's DLL, so a
// removed mod's notice is ignored; then one line per message). The same
// format was used by mods built before the platform (WheresMyBuddy 1.1-1.2,
// FH2 Universal Radio 1.2), which render the title screen themselves from
// these files; keeping to it means old and new mods never fight over it.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fh2platform {

int compare_versions(std::string_view a, std::string_view b);

// Whether a cached check (latest release `latest`, made at `checked` while
// `cached_for` was installed) still answers for `version` at `now`: under 6
// hours old, made for this same version, and not older than it. A cached
// "latest" older than what's installed means a release came out since.
bool cache_still_good(long long checked, long long now, const std::string& latest, const std::string& cached_for,
                      const std::string& version);

// Latest release tag of "Owner/Repo" on GitHub, cached in
// notices\<name>.check (see cache_still_good). Empty if unknown.
std::string latest_release(const std::filesystem::path& azimuth, const std::wstring& name,
                           const std::string& repo, const std::string& agent_version, bool* from_cache);

void write_notice_file(const std::filesystem::path& azimuth, const std::wstring& name, const std::wstring& dll,
                       const std::vector<std::string>& lines);

// The "TO START" text (IDS_To_Start in Main.str) with every installed mod's
// notice lines under it, or `to_start` unchanged when there are none.
std::wstring title_with_notices(const std::filesystem::path& azimuth, const std::wstring& to_start);
bool any_notices(const std::filesystem::path& azimuth);

constexpr uint16_t kToStartKey = 0x18F0; // IDS_To_Start in Main.str

std::string narrow(std::wstring_view w);
std::wstring widen(std::string_view s);

} // namespace fh2platform

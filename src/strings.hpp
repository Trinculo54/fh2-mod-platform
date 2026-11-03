#pragma once

// Reading FH2's string tables (media\Stripped\StringTables\<lang>.zip) and
// writing edited copies for Forza Horizon 2 Extra Options' mediaoverride
// folder.

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fh2platform {

// One file out of a zip (stored or deflated), or empty if it isn't there.
std::vector<char> unzip_file(const std::filesystem::path& zip, std::string_view name);

// Rebuilds an "LSB2" string table with each string passed through `edit`
// (key, current text) -> new text. Empty result if `table` isn't LSB2.
// `changed` (optional) counts strings that came out different. `add` puts in
// new keys (ones the table has are left alone; `taken` counts them), in key
// order, with a record in each section that has one per string (the IDS_ name
// table, the 16-bit values): empty result if the table's sections aren't laid
// out so.
std::vector<char> edit_lsb2(const std::vector<char>& table,
                            const std::function<std::wstring(uint16_t key, const std::wstring& text)>& edit,
                            int* changed = nullptr, const std::map<uint16_t, std::wstring>& add = {},
                            int* taken = nullptr);

// <game Mount folder>, from Azimuth's data\azimuth.ini ([title-1a96545b]
// folder=...), falling back to FH2\Mount. Empty if not found.
std::filesystem::path find_game_mount(const std::filesystem::path& azimuth_dir,
                                      const std::filesystem::path& command_line_folder = {});

// Writes `file` from every language zip, edited, to
// <mount>\mediaoverride\Stripped\StringTables\<lang>\<file>. With `remove`
// set, deletes those overrides instead. A non-empty `override_root` is used instead of
// <mount>\mediaoverride. `add`: new keys, as edit_lsb2. Returns how many were written or
// removed.
int override_string_table(const std::filesystem::path& mount, std::string_view file,
                          const std::function<std::wstring(uint16_t, const std::wstring&)>& edit,
                          bool remove = false, const std::filesystem::path& override_root = {},
                          const std::map<uint16_t, std::wstring>& add = {});

} // namespace fh2platform

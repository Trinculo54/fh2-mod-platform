#pragma once

// Reading FH2's string tables (media\Stripped\StringTables\<lang>.zip) and
// writing edited copies for Forza Horizon 2 Extra Options' mediaoverride
// folder.

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fh2platform {

// One file out of a zip (stored or deflated), or empty if it isn't there.
std::vector<char> unzip_file(const std::filesystem::path& zip, std::string_view name);

// Rebuilds an "LSB2" string table with each string passed through `edit`
// (key, current text) -> new text. Empty result if `table` isn't LSB2.
// `changed` (optional) counts strings that came out different.
std::vector<char> edit_lsb2(const std::vector<char>& table,
                            const std::function<std::wstring(uint16_t key, const std::wstring& text)>& edit,
                            int* changed = nullptr);

// <game Mount folder>, from Azimuth's data\azimuth.ini ([title-1a96545b]
// folder=...), falling back to FH2\Mount. Empty if not found.
std::filesystem::path find_game_mount(const std::filesystem::path& azimuth_dir,
                                      const std::filesystem::path& command_line_folder = {});

// Writes `file` from every language zip, edited, to
// <mount>\mediaoverride\Stripped\StringTables\<lang>\<file>. With `remove`
// set, deletes those overrides instead. Returns how many were written or
// removed.
int override_string_table(const std::filesystem::path& mount, std::string_view file,
                          const std::function<std::wstring(uint16_t, const std::wstring&)>& edit,
                          bool remove = false);

} // namespace fh2platform

// What's different when the game runs under Wine or Proton, as plain functions the tests check without
// the game (tests/wine_test.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace fh2platform {

// Whether a pointer the exe holds (a vtable slot, say) still points where the file's does once the
// image is mapped at `mapped_base` rather than `file_base`. Under Proton the game isn't mapped at
// 0x140000000, so the loader adds the difference to every absolute pointer in it, and comparing those
// bytes with the file's always fails. `on_disk` has to point into the image on disk (from file_base,
// image_size bytes long): anything else isn't a relocated pointer, so it doesn't match.
bool same_pointer(uint64_t on_disk, uint64_t in_memory, uint64_t file_base, uint64_t mapped_base,
                  uint64_t image_size);

} // namespace fh2platform

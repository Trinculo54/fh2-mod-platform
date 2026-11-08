// What's different when the game runs under Wine or Proton: see wine.hpp.
#include "wine.hpp"

namespace fh2platform {

bool same_pointer(uint64_t on_disk, uint64_t in_memory, uint64_t file_base, uint64_t mapped_base,
                  uint64_t image_size) {
    if (on_disk < file_base || on_disk - file_base >= image_size) return false;
    // Unsigned arithmetic wraps, so this holds for an image mapped below its preferred base too
    // (Proton has put it at 0x14690000, 0x18940000 and 0x19D40000).
    return in_memory == on_disk + (mapped_base - file_base);
}

} // namespace fh2platform

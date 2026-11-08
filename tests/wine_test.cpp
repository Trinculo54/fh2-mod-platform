// What's different under Wine and Proton (src/wine.cpp), with no game. Exits 0 when every check passes.
#include "wine.hpp"

#include <cstdint>
#include <cstdio>

using namespace fh2platform;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

constexpr uint64_t kBase = 0x140000000ull, kSize = 0x3A18000ull;

} // namespace

int main() {
    // The folder volume's vtable (game+0x2EAB918): two pointers into the game's code. On disk they're
    // 0x1411E5B30 and 0x14021DEE0; Proton has mapped the image at 0x18940000, below its preferred base.
    check(same_pointer(0x1411E5B30ull, 0x1411E5B30ull, kBase, kBase, kSize),
          "mapped at its preferred base: the same pointer matches (Windows)");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B31ull, kBase, kBase, kSize),
          "mapped at its preferred base: a different pointer doesn't");
    check(same_pointer(0x1411E5B30ull, 0x19B25B30ull, kBase, 0x18940000ull, kSize),
          "mapped below it (0x18940000): the pointer moved with the image matches");
    check(same_pointer(0x14021DEE0ull, 0x18B5DEE0ull, kBase, 0x18940000ull, kSize),
          "mapped below it: the vtable's second pointer too");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B30ull, kBase, 0x18940000ull, kSize),
          "mapped below it: the pointer as on disk doesn't (the bytes the old check compared)");
    check(same_pointer(0x1411E5B30ull, 0x1C11E5B30ull, kBase, 0x1C0000000ull, kSize),
          "mapped above it (0x1C0000000): the pointer moved with the image matches");
    check(!same_pointer(0x1411E5B30ull, 0x1411E5B30ull + 0x1000, kBase, 0x18940000ull, kSize),
          "mapped elsewhere: a pointer moved by some other amount doesn't");
    check(!same_pointer(0x0ull, 0x0ull - kBase + 0x18940000ull, kBase, 0x18940000ull, kSize),
          "a value that isn't a pointer into the image never matches, even shifted");
    check(!same_pointer(kBase + kSize, kBase + kSize, kBase, kBase, kSize),
          "the first address past the image isn't in it");
    check(same_pointer(kBase + kSize - 1, kBase + kSize - 1, kBase, kBase, kSize),
          "the image's last byte is in it");
    check(!same_pointer(kBase - 1, kBase - 1, kBase, kBase, kSize), "the address before the image isn't in it");

    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

// The game's list of mounted volumes as the platform reads it with Extra Options (src/volume_walk.hpp): made-up
// lists of 56-byte entries, and what classify() says about them. No game: run volume_walk_test.exe.
#include "volume_walk.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace fh2platform;

static int fails = 0;
#define CHECK(x) do { bool ok_ = (x); std::printf("%s %s\n", ok_ ? "ok  " : "FAIL", #x); if (!ok_) ++fails; } while (0)

// Where things are: Extra Options' image, the game's, the platform's; the platform's eo-media volume.
constexpr uint64_t kEoLo = 0x7FF810000000ull, kEoHi = 0x7FF810100000ull;
constexpr uint64_t kEoFolderVt = kEoLo + 0xF1C40;      // its folder volumes' vtable, in its data
constexpr uint64_t kGameZipVt  = 0x142EAA018ull;       // a game volume's
constexpr uint64_t kPlatformVt = 0x7FF820004000ull;    // the platform's folder volumes'
constexpr uint64_t kOverlay    = 0x2000100ull;

struct Made {
    uint32_t priority;
    uint64_t volume, vtable;
    const char* folder;
};

// The bytes the game's vector holds for these entries, then the list as the platform reads it.
static VolumeList made(const std::vector<Made>& in, std::size_t cut = 0) {
    std::vector<unsigned char> bytes(in.size() * kVolumeEntrySize, 0xCD);
    for (std::size_t i = 0; i < in.size(); ++i) {
        unsigned char* e = bytes.data() + i * kVolumeEntrySize;
        std::memcpy(e, &in[i].priority, 4);
        std::memcpy(e + 8, &in[i].volume, 8);
    }
    VolumeList list = parse_volume_list(bytes.data(), bytes.size() - cut);
    for (std::size_t i = 0; i < list.entries.size(); ++i) {
        list.entries[i].vtable      = in[i].vtable;
        list.entries[i].vtable_read = in[i].vtable != 0;
        list.entries[i].folder      = in[i].folder;
        if (!list.entries[i].vtable_read) list.read = false;
    }
    return list;
}

static const VolumeOwners kOwners{kEoLo, kEoHi, kOverlay};

int main() {
    // The game's own volumes only, before anything else has mounted: not late.
    const std::vector<Made> game = {{0x96, 0x2000200, kGameZipVt, ""}, {0x64, 0x2000300, kGameZipVt, ""}, {5, 0x2000400, kGameZipVt, ""}};
    {
        const Classified c = classify(made(game), kOwners);
        CHECK(c.readable && c.eo_volumes == 0 && !c.overlay && !volumes_late(c) && !overlay_in_front(c));
    }
    // Late: one of Extra Options' folders is there before the platform's first lookup.
    {
        std::vector<Made> v = game;
        v.insert(v.begin(), {0x7FFFFFFE, 0x2000500, kEoFolderVt, "game:\\mediaoverride\\"});
        const Classified c = classify(made(v), kOwners);
        CHECK(c.readable && c.eo_volumes == 1 && volumes_late(c));
    }
    // After the first lookup: eo-media alone at the top, then Extra Options' mediaoverride, mediapc, carbin cache, the
    // packs, its mods' folders, the game's.
    const std::vector<Made> good = {{0x7FFFFFFF, kOverlay, kPlatformVt, "T:\\fh2-platform\\eo-media\\"},
                                    {0x7FFFFFFE, 0x2000500, kEoFolderVt, "game:\\mediaoverride\\"},
                                    {0x7FFFFFFD, 0x2000600, kEoFolderVt, "game:\\mediapc\\"},
                                    {0x7FFFFFFC, 0x2000700, kEoFolderVt, "carbin cache"},
                                    {0x7FFFFFF0, 0x2000800, kPlatformVt, "T:\\fh2-platform\\packs\\A\\"},
                                    {0x7FFFFFEF, 0x2000900, kPlatformVt, "T:\\fh2-platform\\packs\\B\\"},
                                    {0x70000001, 0x2000A00, kEoFolderVt, "game:\\__fh2_mods__\\000\\"},
                                    {0x70000002, 0x2000B00, kGameZipVt, ""},
                                    {0x96, 0x2000200, kGameZipVt, ""}};
    {
        const Classified c = classify(made(good), kOwners);
        CHECK(c.readable && c.eo_volumes == 4 && c.overlay && c.overlay_alone_on_top && c.above.empty());
        CHECK(overlay_in_front(c));
        CHECK(volumes_late(c));   // what the same list would mean before the platform mounted
    }
    // An Extra Options volume tying with eo-media at 0x7FFFFFFF: which one the game reads first isn't known.
    {
        std::vector<Made> v = good;
        v.insert(v.begin() + 1, {0x7FFFFFFF, 0x2000C00, kEoFolderVt, "game:\\mediaoverride\\"});
        const Classified c = classify(made(v), kOwners);
        CHECK(c.overlay && !c.overlay_alone_on_top && !overlay_in_front(c));
        CHECK(c.above == "Extra Options' game:\\mediaoverride\\ at 0x7FFFFFFF");
    }
    // One at 0x80000000, which the game's unsigned comparator puts ahead of eo-media; a volume of no one's known there too.
    {
        std::vector<Made> v = good;
        v.insert(v.begin(), {0x80000000u, 0x2000D00, kEoFolderVt, "game:\\mediaoverride\\"});
        v.insert(v.begin(), {0xFFFFFFFFu, 0x2000E00, kGameZipVt, ""});
        const Classified c = classify(made(v), kOwners);
        CHECK(!overlay_in_front(c) && c.above == "a volume at 0xFFFFFFFF, Extra Options' game:\\mediaoverride\\ at 0x80000000");
    }
    // Extra Options mounted nothing (its media overrides off and no mods): eo-media is on top, but nothing of Extra
    // Options' is below it, so what it was built on isn't what the game reads.
    {
        const std::vector<Made> v = {{0x7FFFFFFF, kOverlay, kPlatformVt, "T:\\fh2-platform\\eo-media\\"},
                                     {0x7FFFFFF0, 0x2000800, kPlatformVt, "T:\\fh2-platform\\packs\\A\\"},
                                     {0x96, 0x2000200, kGameZipVt, ""}};
        const Classified c = classify(made(v), kOwners);
        CHECK(c.readable && c.eo_volumes == 0 && c.overlay_alone_on_top && !overlay_in_front(c));
    }
    // eo-media not in the list (it didn't mount): not in front.
    {
        std::vector<Made> v(good.begin() + 1, good.end());
        const Classified c = classify(made(v), kOwners);
        CHECK(!c.overlay && !overlay_in_front(c));
    }
    // A list cut short (its end pointer read part-way through an entry), one longer than any game list, and a volume
    // whose vtable couldn't be read: can't be told, so late, and never in front.
    {
        const VolumeList cut = made(good, 20);
        CHECK(!cut.read && cut.entries.empty() && cut.problem == "its size, 484 bytes, isn't a whole number of entries");
        const Classified c = classify(cut, kOwners);
        CHECK(!c.readable && volumes_late(c) && !overlay_in_front(c));
        std::vector<unsigned char> big((kMaxVolumes + 1) * kVolumeEntrySize);
        const VolumeList huge = parse_volume_list(big.data(), big.size());
        CHECK(!huge.read && huge.problem == "1025 entries is more than any game list has");
        std::vector<Made> v = good;
        v[3].vtable = 0;
        const Classified u = classify(made(v), kOwners);
        CHECK(!u.readable && volumes_late(u) && !overlay_in_front(u));
    }
    // An empty list (nothing mounted yet) is read, and isn't late.
    {
        const VolumeList none = parse_volume_list(nullptr, 0);
        const Classified c    = classify(none, kOwners);
        CHECK(none.read && c.readable && !volumes_late(c));
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails;
}

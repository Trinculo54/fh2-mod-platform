// The game's list of mounted volumes, read without the game's help (platform 2.9.0, car packs with Extra Options).
//
// The file system keeps it as a std::vector of 56-byte entries from fs+0x30 to fs+0x38. The mount (game+0x11F82B0)
// builds an entry at rbp-0x40 and pushes it at game+0x11F83A2 (lea rdx,[rbp-0x40]; lea rcx,[rdi+0x30]; call); the
// sort (game+0x12039D0, the file system's +0x1F8 method; at game+0x1203A10 it sorts fs+0x30..0x38 in steps of 56)
// orders the list by priority, highest first, comparing them as unsigned numbers (game+0x11FB550: mov edx,[rdx];
// cmp [rcx],edx; seta al; ret). An entry:
//   +0x00  u32  its priority          (game+0x11F8357: mov [rbp-0x40],esi)
//   +0x08  ptr  the volume            (game+0x11F836F: mov [rbp-0x38],rbx)
//   +0x10  where it's mounted, an MSVC std::string ("game:\media\" for every override folder)
//   +0x30  u32  that path's hash
// A volume's first 8 bytes are its vtable. A folder volume, made by the game's constructor (game+0x11E05A0, which
// Extra Options' folders and the platform's are made with too), has the folder it reads at +0x28, an MSVC std::string.
//
// Extra Options' folder volumes have a vtable of its own, in its image; its zip volumes and the game's have the
// game's. The platform's have one of its own too. classify() is pure, so tests/volume_walk_test.cpp checks it on
// made-up lists.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fh2platform {

constexpr std::size_t kVolumeEntrySize = 56;
constexpr std::size_t kMaxVolumes      = 1024;   // a longer list is taken for one that isn't what it seems
constexpr uint32_t kOverlayPriority    = 0x7FFFFFFFu;

struct VolumeEntry {
    uint32_t priority = 0;
    uint64_t volume   = 0;
    uint64_t vtable   = 0;   // the volume's first 8 bytes
    bool vtable_read  = false;
    std::string folder;      // a folder volume's folder ("" when it isn't one, or it couldn't be read)
};

struct VolumeList {
    std::vector<VolumeEntry> entries;
    bool read = false;     // the whole list, and every volume's vtable, could be read
    std::string problem;   // why not
};

// The list's bytes (from the vector's begin to its end) as entries, without their vtables and folders.
inline VolumeList parse_volume_list(const unsigned char* bytes, std::size_t n) {
    VolumeList list;
    if (n % kVolumeEntrySize) {
        list.problem = "its size, " + std::to_string(n) + " bytes, isn't a whole number of entries";
        return list;
    }
    if (n / kVolumeEntrySize > kMaxVolumes) {
        list.problem = std::to_string(n / kVolumeEntrySize) + " entries is more than any game list has";
        return list;
    }
    for (std::size_t at = 0; at < n; at += kVolumeEntrySize) {
        VolumeEntry e;
        std::memcpy(&e.priority, bytes + at, 4);
        std::memcpy(&e.volume, bytes + at + 8, 8);
        list.entries.push_back(e);
    }
    list.read = true;
    return list;
}

// What's whose: Extra Options' image, and the platform's eo-media volume.
struct VolumeOwners {
    uint64_t eo_lo = 0, eo_hi = 0;
    uint64_t overlay = 0;
};

struct Classified {
    bool readable = false;
    int eo_volumes = 0;                  // volumes whose vtable is in Extra Options' image
    bool overlay = false;                // eo-media is in the list
    bool overlay_alone_on_top = false;   // and no other volume is at kOverlayPriority or above
    std::string above;                   // those others, for the log
};

inline Classified classify(const VolumeList& list, const VolumeOwners& o) {
    Classified c;
    c.readable = list.read;
    for (const VolumeEntry& e : list.entries) {
        if (!e.vtable_read) c.readable = false;
        const bool eo = e.vtable_read && e.vtable >= o.eo_lo && e.vtable < o.eo_hi;
        c.eo_volumes += eo;
        if (o.overlay && e.volume == o.overlay) {
            c.overlay = true;
            continue;
        }
        if (e.priority >= kOverlayPriority) {   // unsigned, as the game's comparator
            char p[16];
            std::snprintf(p, sizeof p, "0x%08X", e.priority);
            c.above += (c.above.empty() ? "" : ", ") + std::string(eo ? "Extra Options' " : "") +
                       (e.folder.empty() ? "a volume" : e.folder) + " at " + p;
        }
    }
    c.overlay_alone_on_top = c.overlay && c.above.empty();
    return c;
}

// Before the platform mounts: Extra Options' folders are there already (a lookup reached it before the platform's
// chain), or the list can't be read, so it can't be told. Either way the platform mustn't mount.
inline bool volumes_late(const Classified& c) { return !c.readable || c.eo_volumes > 0; }

// Once the first lookup has gone on to Extra Options: it has set up its folders, and eo-media is ahead of all of them.
inline bool overlay_in_front(const Classified& c) {
    return c.readable && c.eo_volumes > 0 && c.overlay && c.overlay_alone_on_top;
}

} // namespace fh2platform

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

// Which DLL Wine loads for a name: the one in the program's folder ("native") or its own ("builtin").
// Unless told otherwise it uses its own for the DLLs it has, winmm.dll among them, so Extra Options'
// winmm.dll only loads with an override that puts native first.
enum class LoadOrder { unsaid, native_first, builtin_first, disabled };

// A load order as Wine reads one: "n,b", "native,builtin", "b", "n" and so on. The first word that
// starts with n or b decides; none at all ("") means the DLL is disabled.
LoadOrder parse_load_order(const std::string& order);

// What WINEDLLOVERRIDES says about the DLL `name`, as Wine looks it up ("winmm" or "*winmm", without
// ".dll"): entries are separated by ';', the names in one by ',', then '=' and the load order
// ("winmm=n,b", "winmm,dxgi=n,b;d3d11=n"). Names are compared without regard to case, and where two
// entries name it, the later one counts, as in Wine. `unsaid` when no entry names it.
LoadOrder env_load_order(const std::string& overrides, const std::string& name);

} // namespace fh2platform

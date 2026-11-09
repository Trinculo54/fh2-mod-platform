// What the platform DLL imports, read from the built file's import and delay-import tables, with no game. Exits 0
// when every check passes. Only KERNEL32.dll, USER32.dll and the C runtime (api-ms-win-crt-*.dll) may be imported
// (CMakeLists.txt): azimuth.exe loads the platform very early, and an app-folder version.dll or winhttp.dll proxy from
// some other mod must not get pulled in by it. That counts DLLs it delay-loads too, since they're looked for in the
// same places. Anything else is loaded from System32 when it's needed (as src/commands.cpp does with advapi32).
//   imports_test.exe [dll...]   with none, the d3dcompiler_47.dll and DWrite.dll next to it (the build's)
// The test's own exe imports ADVAPI32.dll and delay-loads VERSION.dll (CMakeLists.txt), so the check is seen to find
// a DLL outside the list in both tables.
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failed;
}

// One DLL a PE file imports, with the functions it takes from it.
struct Import {
    std::string dll;
    std::vector<std::string> functions; // names, or "#n" for an ordinal
    bool delayed = false;               // from the delay-import table
};

// The delay-import table's entries (IMAGE_DELAYLOAD_DESCRIPTOR in the Windows SDK).
struct DelayDescriptor {
    uint32_t attributes, dll_name, module_handle, address_table, name_table, bound_table, unload_table, time;
};

struct Image {
    std::vector<unsigned char> data;
    uint64_t image_base = 0;
    std::vector<IMAGE_SECTION_HEADER> sections;
    IMAGE_DATA_DIRECTORY dirs[IMAGE_NUMBEROF_DIRECTORY_ENTRIES]{};
};

bool read_file(const std::wstring& path, std::vector<unsigned char>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 256 * 1024 * 1024;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
    }
    CloseHandle(f);
    return ok;
}

template <class T> bool read_at(const Image& im, uint64_t off, T* out) {
    if (off > im.data.size() || im.data.size() - off < sizeof(T)) return false;
    std::memcpy(out, im.data.data() + off, sizeof(T));
    return true;
}

// Where an RVA is in the file, or false when it isn't in any section's bytes on disk.
bool file_offset(const Image& im, uint64_t rva, uint64_t* off) {
    for (const auto& s : im.sections) {
        if (rva < s.VirtualAddress || rva - s.VirtualAddress >= std::max(s.Misc.VirtualSize, s.SizeOfRawData)) continue;
        if (rva - s.VirtualAddress >= s.SizeOfRawData) return false; // the zero-filled rest of the section
        *off = s.PointerToRawData + (rva - s.VirtualAddress);
        return *off < im.data.size();
    }
    return false;
}

// The text at an RVA, or "" when it can't be read.
std::string text_at(const Image& im, uint64_t rva) {
    uint64_t off = 0;
    if (!rva || !file_offset(im, rva, &off)) return {};
    std::string out;
    for (; off < im.data.size() && im.data[off] && out.size() < 512; ++off) out += static_cast<char>(im.data[off]);
    return off < im.data.size() && !im.data[off] ? out : std::string{};
}

// The headers of a 64-bit PE file: its sections and data directories.
bool read_headers(Image* im) {
    IMAGE_DOS_HEADER dos{};
    if (!read_at(*im, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) return false;
    IMAGE_NT_HEADERS64 nt{};
    if (!read_at(*im, static_cast<uint64_t>(dos.e_lfanew), &nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    im->image_base = nt.OptionalHeader.ImageBase;
    const DWORD n = std::min<DWORD>(nt.OptionalHeader.NumberOfRvaAndSizes, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
    for (DWORD i = 0; i < n; ++i) im->dirs[i] = nt.OptionalHeader.DataDirectory[i];
    const uint64_t first = static_cast<uint64_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                           nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER s{};
        if (!read_at(*im, first + i * sizeof s, &s)) return false;
        im->sections.push_back(s);
    }
    return !im->sections.empty();
}

// The functions a lookup table names: 64-bit entries, up to a zero one.
std::vector<std::string> functions(const Image& im, uint64_t rva) {
    std::vector<std::string> out;
    for (int i = 0; rva && i < 10000; ++i, rva += 8) {
        uint64_t off = 0, entry = 0;
        if (!file_offset(im, rva, &off) || !read_at(im, off, &entry) || !entry) break;
        if (entry & IMAGE_ORDINAL_FLAG64) out.push_back("#" + std::to_string(entry & 0xFFFF));
        else out.push_back(text_at(im, (entry & 0x7FFFFFFF) + 2)); // past the hint
    }
    return out;
}

// Every DLL the file imports, from the import table and the delay-import table; false when either can't be read.
bool read_imports(const Image& im, std::vector<Import>* out) {
    if (const auto& d = im.dirs[IMAGE_DIRECTORY_ENTRY_IMPORT]; d.VirtualAddress) {
        for (uint64_t rva = d.VirtualAddress;; rva += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR desc{};
            uint64_t off = 0;
            if (out->size() > 1000 || !file_offset(im, rva, &off) || !read_at(im, off, &desc)) return false;
            if (!desc.Name) break; // the zero entry at the end
            Import i;
            i.dll       = text_at(im, desc.Name);
            i.functions = functions(im, desc.OriginalFirstThunk ? desc.OriginalFirstThunk : desc.FirstThunk);
            if (i.dll.empty()) return false;
            out->push_back(i);
        }
    }
    if (const auto& d = im.dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]; d.VirtualAddress) {
        for (uint64_t rva = d.VirtualAddress;; rva += sizeof(DelayDescriptor)) {
            DelayDescriptor desc{};
            uint64_t off = 0;
            if (out->size() > 1000 || !file_offset(im, rva, &off) || !read_at(im, off, &desc)) return false;
            if (!desc.dll_name) break;
            // Without attribute bit 0 (the old form), the entries hold addresses rather than RVAs.
            const uint32_t less = (desc.attributes & 1) ? 0 : static_cast<uint32_t>(im.image_base);
            Import i;
            i.dll       = text_at(im, desc.dll_name - less);
            i.functions = functions(im, desc.name_table ? desc.name_table - less : 0);
            i.delayed   = true;
            if (i.dll.empty()) return false;
            out->push_back(i);
        }
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool allowed(const std::string& dll) {
    const std::string d = lower(dll);
    return d == "kernel32.dll" || d == "user32.dll" ||
           (d.rfind("api-ms-win-crt-", 0) == 0 && d.size() > 4 && d.compare(d.size() - 4, 4, ".dll") == 0);
}

std::string named(const Import& i) {
    std::string out = i.dll + " (";
    for (size_t n = 0; n < i.functions.size(); ++n) {
        if (n == 8) {
            out += ", and " + std::to_string(i.functions.size() - n) + " more";
            break;
        }
        out += (n ? ", " : "") + i.functions[n];
    }
    return out + ")";
}

std::string narrow(const std::wstring& w) {
    std::string out;
    for (wchar_t c : w) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}

std::wstring file_name(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A file's imports, or false (with a FAIL line) when they can't be read.
bool imports_of(const std::wstring& path, std::vector<Import>* out) {
    Image im;
    const std::string name = narrow(file_name(path));
    const bool ok = read_file(path, &im.data) && read_headers(&im) && read_imports(im, out);
    check(ok, name + ": reads as a 64-bit PE file, with its import and delay-import tables");
    return ok;
}

// The rule, on one built DLL.
void check_platform(const std::wstring& path) {
    const std::string name = narrow(file_name(path));
    std::vector<Import> all;
    if (!imports_of(path, &all)) return;
    bool kernel32 = false;
    int outside = 0, delayed = 0;
    for (const auto& i : all) {
        if (lower(i.dll) == "kernel32.dll" && !i.delayed && !i.functions.empty()) kernel32 = true;
        if (i.delayed) ++delayed;
        if (allowed(i.dll)) continue;
        ++outside;
        check(false, name + (i.delayed ? " delay-loads " : " imports ") + named(i) +
                         (i.delayed ? "" : " at load time") +
                         ": only KERNEL32.dll, USER32.dll and the C runtime (api-ms-win-crt-*.dll) may be imported, "
                         "delay-loaded or not; load it from System32 when it's needed instead (CMakeLists.txt)");
    }
    // A table read as empty would pass the rule above without saying anything.
    check(kernel32, name + " imports functions from KERNEL32.dll, so its import table was read");
    if (!outside)
        check(true, name + " imports only KERNEL32.dll, USER32.dll and the C runtime (" + std::to_string(all.size()) +
                        " DLLs, " + std::to_string(delayed) + " delay-loaded)");
}

// The check itself, on this exe: it imports ADVAPI32.dll and delay-loads VERSION.dll (CMakeLists.txt).
void check_the_check() {
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::vector<Import> all;
    if (!n || n >= MAX_PATH || !imports_of(self, &all)) return;
    bool advapi32 = false, version = false;
    for (const auto& i : all) {
        if (lower(i.dll) == "advapi32.dll" && !i.delayed && !allowed(i.dll)) advapi32 = true;
        if (lower(i.dll) == "version.dll" && i.delayed && !allowed(i.dll) &&
            std::find(i.functions.begin(), i.functions.end(), "GetFileVersionInfoSizeW") != i.functions.end())
            version = true;
    }
    check(advapi32, "the check finds a DLL outside the list in the import table (imports_test.exe's own ADVAPI32.dll)");
    check(version, "and in the delay-import table (imports_test.exe's own VERSION.dll, GetFileVersionInfoSizeW)");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Never called: they put ADVAPI32.dll in this exe's import table and VERSION.dll in its delay-import table.
    void* volatile imported[] = {reinterpret_cast<void*>(&GetUserNameW),
                                 reinterpret_cast<void*>(&GetFileVersionInfoSizeW)};
    (void)imported;
    check_the_check();
    std::vector<std::wstring> dlls;
    for (int i = 1; i < argc; ++i) dlls.push_back(argv[i]);
    if (dlls.empty()) {
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        if (n && n < MAX_PATH) {
            std::wstring dir(self, n);
            dir.resize(dir.find_last_of(L"\\/") + 1); // the folder, with its slash
            dlls = {dir + L"d3dcompiler_47.dll", dir + L"DWrite.dll"};
        }
    }
    check(!dlls.empty(), "there are DLLs to check");
    for (const auto& d : dlls) check_platform(d);
    std::printf(g_failed ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}

// Stands in for Extra Options' winmm.dll in the hook test: the platform
// recognises it by its Fh2TweaksStatus export, and reads FH2_ExtraOptions.log
// next to it, and its version from the text every Extra Options release has in
// its image.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
[[gnu::used]] static const char kVersion[] = "Forza Horizon 2 Extra Options 0.6.0";
extern "C" __declspec(dllexport) const char* Fh2TweaksStatus() { return "OK: hook test stand-in"; }

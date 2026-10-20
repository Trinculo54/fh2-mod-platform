// Stands in for Extra Options' winmm.dll in the hook test: the platform
// recognises it by its Fh2TweaksStatus export, and reads FH2_ExtraOptions.log
// next to it.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
extern "C" __declspec(dllexport) const char* Fh2TweaksStatus() { return "OK: hook test stand-in"; }

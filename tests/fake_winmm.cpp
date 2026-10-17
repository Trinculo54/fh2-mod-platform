// Stands in for Extra Options' winmm.dll in the hook test: the platform
// only looks at where it was loaded from and at FH2_ExtraOptions.log.
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

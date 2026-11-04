// A DLL whose detach never returns, as a GPU driver's can wait for a thread that's been killed. A
// program that has it loaded ends only if its end doesn't run a DLL's detach.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_DETACH) {
        Sleep(INFINITE);
    }

    return TRUE;
}

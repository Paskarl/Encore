// version.dll proxy: UnityPlayer.dll imports VERSION.dll, so a version.dll placed next to
// "Clone Hero.exe" is loaded instead of the system one. Every export forwards to the real DLL.
#include <windows.h>
#include <cstdint>

static HMODULE g_realVersion = nullptr;

static FARPROC RealProc(const char* name)
{
    if (!g_realVersion)
    {
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        lstrcatW(path, L"\\version.dll");
        g_realVersion = LoadLibraryW(path);
    }
    return g_realVersion ? GetProcAddress(g_realVersion, name) : nullptr;
}

// x64: all version.dll exports take <= 8 integer/pointer args, so a generic 8-arg
// trampoline passes registers and stack args through unchanged.
using Generic8 = uintptr_t(WINAPI*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

#define PROXY(name)                                                                              \
    extern "C" uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, \
                                             uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) \
    {                                                                                            \
        static Generic8 real = reinterpret_cast<Generic8>(RealProc(#name));                      \
        return real ? real(a, b, c, d, e, f, g, h) : 0;                                          \
    }

PROXY(GetFileVersionInfoA)
PROXY(GetFileVersionInfoByHandle)
PROXY(GetFileVersionInfoExA)
PROXY(GetFileVersionInfoExW)
PROXY(GetFileVersionInfoSizeA)
PROXY(GetFileVersionInfoSizeExA)
PROXY(GetFileVersionInfoSizeExW)
PROXY(GetFileVersionInfoSizeW)
PROXY(GetFileVersionInfoW)
PROXY(VerFindFileA)
PROXY(VerFindFileW)
PROXY(VerInstallFileA)
PROXY(VerInstallFileW)
PROXY(VerQueryValueA)
PROXY(VerQueryValueW)

// Windows 7: stand-ins for the newer functions that libraries FastChat links
// call and YY-Thunks (linked in too) does not cover. Each calls the real
// function when Windows has it. This file is linked into the exe as an object,
// ahead of the system's import libraries, so these definitions take the place
// of the imports (as YY-Thunks' own do).

#ifdef _WIN32

#include <windows.h>

namespace {

FARPROC FindSystemFunction(const wchar_t* dll, const char* name) {
    HMODULE module = GetModuleHandleW(dll);
    if (!module) module = LoadLibraryW(dll);
    return module ? GetProcAddress(module, name) : nullptr;
}

}  // namespace

// UIAutomationCore, Windows 8 and later (Prism's UIA backend). Declared here
// rather than from the SDK header, which would make it an import.
extern "C" HRESULT WINAPI UiaDisconnectProvider(void* provider) {
    using Fn = HRESULT(WINAPI*)(void*);
    static const Fn real = reinterpret_cast<Fn>(FindSystemFunction(L"uiautomationcore.dll", "UiaDisconnectProvider"));
    return real ? real(provider) : S_OK;  // Windows 7: nothing is connected to disconnect
}
extern "C" const void* __imp_UiaDisconnectProvider = reinterpret_cast<const void*>(&UiaDisconnectProvider);

#endif

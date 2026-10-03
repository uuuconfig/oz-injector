// Test payload. DllMain records its own image base so an injected run can be
// verified from outside.
//
// The base DllMain receives is the key signal:
//   - LoadLibrary mode: the module's real base; the DLL appears in the target's
//     module list.
//   - Manual map mode:   a base the loader never registered, so the DLL is NOT
//     in the module list even though DllMain ran.
#include <windows.h>

static void record(ULONG_PTR base) {
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"oz_injector_probe.txt");

    HANDLE f = CreateFileW(path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, nullptr, FILE_END);

    // Format the hex by hand: wsprintfA would drag in user32.lib.
    static const char kHex[] = "0123456789abcdef";
    char buf[96];
    int n = 0;
    const char* prefix = "attached base=0x";
    while (*prefix) buf[n++] = *prefix++;
    for (int shift = 60; shift >= 0; shift -= 4) {
        buf[n++] = kHex[(base >> shift) & 0xF];
    }
    const char* tail = " pid=";
    while (*tail) buf[n++] = *tail++;
    const ULONG pid = GetCurrentProcessId();
    for (int shift = 28; shift >= 0; shift -= 4) {
        buf[n++] = kHex[(pid >> shift) & 0xF];
    }
    buf[n++] = '\r';
    buf[n++] = '\n';

    DWORD w = 0;
    WriteFile(f, buf, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(f);
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        record(reinterpret_cast<ULONG_PTR>(h));
    }
    return TRUE;
}

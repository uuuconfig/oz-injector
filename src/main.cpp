// oz_injector — entry point.
//
// A standalone, dependency-free DLL injector. Point it at any x64 DLL and any
// running process; it either manual-maps the image (no module-list entry) or
// calls remote LoadLibraryW.
//
// The OpenZen project this was extracted from carries its payload inside an
// RCDATA blob. This build is deliberately separate: the DLL path comes from the
// caller, so any DLL works.
#include <windows.h>

#include <commctrl.h>

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

#include "oz_injector_core.h"
#include "oz_injector_ui.h"

namespace {

// Print usage. This is a GUI subsystem binary, so there is no console
// attached; stderr is still reachable from a shell, stdout is not.
void print_usage() {
    static const wchar_t* kUsage =
        L"oz injector - standalone x64 DLL injector\r\n"
        L"\r\n"
        L"  oz_injector.exe\r\n"
        L"      Open the GUI.\r\n"
        L"\r\n"
        L"  oz_injector.exe --inject <pid> <dll.dll> [--manual|--loadlibrary]\r\n"
        L"      One-shot injection, for scripts. Prints the step log to stderr\r\n"
        L"      and returns 0 on success, 1 on failure.\r\n"
        L"\r\n"
        L"  oz_injector.exe /?\r\n"
        L"      This message.\r\n"
        L"\r\n"
        L"Modes:\r\n"
        L"  --manual       relocates the PE in target memory and calls DllMain\r\n"
        L"                 through a trampoline; nothing enters the module list.\r\n"
        L"  --loadlibrary  classic remote LoadLibraryW; visible in the module\r\n"
        L"                 list. (default: --manual)\r\n"
        L"\r\n"
        L"The target must run at or below this tool's integrity level. Run the\r\n"
        L"tool elevated if the target runs elevated.\r\n";
    ::fwprintf(stderr, L"%s", kUsage);
}

// Split on whitespace.
std::vector<std::wstring> tokenize(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) ++i;
        const size_t start = i;
        while (i < s.size() && !iswspace(s[i])) ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// One-shot injection path: parse args, run oz::inject, print the trace.
// Returns a process exit code so scripts can branch on it.
int run_inject_oneshot(const std::wstring& cmd) {
    const auto args = tokenize(cmd);
    // args[0] == "--inject"
    if (args.size() < 3) {
        fwprintf(stderr, L"error: --inject needs <pid> and <dll path>\n");
        return 1;
    }

    DWORD pid = 0;
    try {
        pid = static_cast<DWORD>(std::stoul(args[1]));
    } catch (...) {
        fwprintf(stderr, L"error: '%s' is not a valid pid\n", args[1].c_str());
        return 1;
    }
    if (pid == 0) {
        fwprintf(stderr, L"error: pid 0 is not a valid target\n");
        return 1;
    }

    const std::wstring dll = args[2];
    oz::InjectMode mode = oz::InjectMode::ManualMap;
    for (size_t i = 3; i < args.size(); ++i) {
        if (args[i] == L"--loadlibrary") mode = oz::InjectMode::LoadLibrary;
        else if (args[i] == L"--manual") mode = oz::InjectMode::ManualMap;
    }

    const auto result = oz::inject(pid, dll, mode);
    for (const auto& line : result.log) {
        fwprintf(stderr, L"  %s\n", line.c_str());
    }
    if (result.status == oz::InjectStatus::Ok) {
        fwprintf(stderr, L"OK: %s\n", result.detail.c_str());
        return 0;
    }
    fwprintf(stderr, L"FAILED: %s%s%s\n", oz::inject_status_text(result.status),
             result.detail.empty() ? L"" : L": ", result.detail.c_str());
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdline, int show) {
    // WinMain semantics: the command line holds arguments only (no argv[0]),
    // as one whitespace-separated string.
    const std::wstring arg = cmdline ? cmdline : L"";

    if (arg == L"/?" || arg == L"-?" || arg == L"--help" ||
        arg.rfind(L"/? ", 0) == 0 || arg.rfind(L"--help ", 0) == 0) {
        print_usage();
        return 0;
    }

    if (arg.rfind(L"--inject", 0) == 0) {
        return run_inject_oneshot(arg);
    }

    // Anything else: open the GUI. The owner-drawn ListView and the RichEdit
    // log both come from comctl32 v6 (the manifest requests it).
    //
    // The UI header is included here rather than at the top so the one-shot
    // path does not drag in the window class.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    oz::ui::AppWindow window;
    if (!window.create(instance)) {
        wchar_t detail[128] = {0};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, window.last_error(), 0, detail,
                       static_cast<DWORD>(std::size(detail) / sizeof(detail[0])),
                       nullptr);
        wchar_t msg[320];
        swprintf_s(msg, L"Failed to create the injector window.\n\n"
                        L"Win32 error %lu: %s\n\n"
                        L"If the injector is already running, close it first.",
                   window.last_error(), detail);
        MessageBoxW(nullptr, msg, L"oz injector", MB_ICONERROR | MB_OK);
        return 1;
    }
    ShowWindow(window.handle_hwnd(), show);
    UpdateWindow(window.handle_hwnd());
    return window.run();
}

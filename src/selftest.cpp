// selftest.cpp — process-internal UI verification.
//
// Creates the window the way the GUI does, then checks that every child control
// really came up. Needed because an external window enumerator cannot see our
// GUI session inside the sandbox, so a failure would otherwise look like a
// silent hang rather than a concrete "this control is null".
#include "oz_injector_ui.h"

#include <windows.h>

#include <commctrl.h>

#include <cstdio>
namespace {

int g_failed = 0;

void check(const wchar_t* what, bool ok) {
    std::fwprintf(stderr, L"  %-28s %s\n", what, ok ? L"ok" : L"FAILED");
    if (!ok) ++g_failed;
}

// Reach into the window's children by class name, so this does not need the
// private members.
HWND find_child(HWND parent, const wchar_t* cls) {
    return FindWindowExW(parent, nullptr, cls, nullptr);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    std::fwprintf(stderr, L"selftest: creating window\n");

    // create() builds and runs WM_CREATE, so every control exists afterwards.
    oz::ui::AppWindow window;
    if (!window.create(instance)) {
        std::fwprintf(stderr, L"selftest: create() FAILED, win32 error %lu\n",
                      window.last_error());
        return 2;
    }
    HWND main = window.handle_hwnd();
    std::fwprintf(stderr, L"selftest: window %p\n", (void*)main);

    check(L"main window", main != nullptr);
    // Visibility is not asserted: the self test never calls ShowWindow, and a
    // window can be fully constructed without being shown.
    check(L"main window valid", IsWindow(main) != 0);

    // The ListView: class name SysListView32.
    HWND list = nullptr;
    {
        // FindWindowEx needs an exact class string.
        HWND child = GetWindow(main, GW_CHILD);
        while (child) {
            wchar_t cls[64] = {0};
            GetClassNameW(child, cls, 64);
            if (wcscmp(cls, L"SysListView32") == 0) {
                list = child;
                break;
            }
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }
    check(L"process ListView", list != nullptr);

    HWND edit = find_child(main, L"Edit");
    check(L"path edit", edit != nullptr);

    int buttons = 0;
    HWND child = GetWindow(main, GW_CHILD);
    while (child) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        if (wcscmp(cls, L"Button") == 0) ++buttons;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    // Browse + two radios + Inject + Refresh = 5 buttons.
    check(L"buttons (expect 5)", buttons == 5);

    HWND logbox = nullptr;
    child = GetWindow(main, GW_CHILD);
    while (child) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        // The process list is a SysListView32; the log is a plain Edit.
        if (wcscmp(cls, L"Edit") == 0 && GetDlgCtrlID(child) == 3104) {
            logbox = child;
            break;
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
    check(L"log edit", logbox != nullptr);
    if (logbox) {
        const int chars0 = GetWindowTextLengthW(logbox);
        std::fwprintf(stderr, L"  log chars: %d\n", chars0);
        check(L"log has content", chars0 > 0);

        // Reproduce the reported bug: pressing Refresh appended a garbled row.
        HWND refresh = GetDlgItem(main, 3103);
        check(L"refresh button", refresh != nullptr);
        if (refresh) {
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(3103, BN_CLICKED),
                         reinterpret_cast<LPARAM>(refresh));
        }

        wchar_t text[2048] = {0};
        GetWindowTextW(logbox, text, 2048);
        std::fwprintf(stderr, L"  log text after refresh:\n[%ls]\n", text);

        const size_t len = wcslen(text);
        check(L"refresh appended text", len > static_cast<size_t>(chars0));

        // Every code point must be a real character. The reported garbage was
        // UTF-8 bytes reinterpreted as UTF-16 (0xC3 0xA9 style pairs), so look
        // for anything outside Latin-1 plus the two symbols we log with.
        bool clean = len > 0;
        for (size_t i = 0; i < len; ++i) {
            const wchar_t ch = text[i];
            const bool ok = (ch >= 0x20 && ch < 0x7F) ||   // ASCII printable
                            (ch >= 0xA0 && ch < 0x2E80) ||  // Latin-1..CJK
                            ch == 0x2500 || ch == 0x2192 ||  // ─ and →
                            ch == L'\r' || ch == L'\n';
            if (!ok) {
                std::fwprintf(stderr, L"  bad code unit at %zu: %04X\n", i,
                              static_cast<unsigned>(
                                  static_cast<unsigned short>(ch)));
                clean = false;
                break;
            }
        }
        check(L"log text clean", clean);

        // And the Refresh line must be exactly what we asked it to log.
        check(L"refresh line exact",
              wcsstr(text, L"Process list refreshed manually.") != nullptr);
    }

    HWND status = nullptr;
    child = GetWindow(main, GW_CHILD);
    while (child) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        if (wcscmp(cls, L"Static") == 0) {
            status = child;
            break;
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
    check(L"status static", status != nullptr);

    // The list must have been populated by WM_CREATE -> refresh_processes().
    if (list) {
        const int count = ListView_GetItemCount(list);
        std::fwprintf(stderr, L"  process rows: %d\n", count);
        check(L"process list populated", count > 0);
    }

    std::fwprintf(stderr, L"selftest: %hs (%d failure(s))\n",
                  g_failed ? "FAILED" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}

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
#include <utility>
namespace {

// LVM_GETITEMW returns (LRESULT)-1 on failure. Spelled out because LV_ERR is
// gated behind a header define that <commctrl.h> does not reliably expose when
// it is included after <windows.h>, and the value has been -1 since comctl32 v4.
constexpr LRESULT kLvErr = static_cast<LRESULT>(-1);

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

    // Reproduce the reported "Inject is a blue block with no caption" bug. The
    // three owner-draw buttons (Manual map / LoadLibrary / Inject) draw their
    // caption in WM_DRAWITEM, which used to read DRAWITEMSTRUCT.itemData — a
    // field that is undefined for BS_OWNERDRAW buttons. Assert every button
    // actually carries a caption, so a regression cannot come back silently.
    {
        int captioned = 0, ownerdraw = 0;
        HWND c = GetWindow(main, GW_CHILD);
        while (c) {
            wchar_t cls[64] = {0};
            GetClassNameW(c, cls, 64);
            if (wcscmp(cls, L"Button") == 0) {
                // BS_OWNERDRAW == 0xB, the low byte of the style.
                const LONG_PTR style =
                    GetWindowLongPtrW(c, GWL_STYLE) & 0xF;
                if (style == 0xB) ++ownerdraw;
                if (GetWindowTextLengthW(c) > 0) ++captioned;
            }
            c = GetWindow(c, GW_HWNDNEXT);
        }
        std::fwprintf(stderr,
                      L"  buttons captioned: %d/%d (owner-draw: %d)\n",
                      captioned, buttons, ownerdraw);
        check(L"all buttons have captions", captioned == buttons);
        // The Inject button specifically — the one that was blank.
        HWND inject = GetDlgItem(main, oz::ui::IDC_INJECT);
        wchar_t cap[64] = {0};
        if (inject) GetWindowTextW(inject, cap, 64);
        std::fwprintf(stderr, L"  inject caption: [%ls]\n", cap);
        check(L"inject button captioned", wcscmp(cap, L"Inject") == 0);
    }

    // Reproduce "the two mode options cannot be selected". A click has to
    // change the mode the injector will actually use.
    //
    // Note what is *not* asserted here: the radio controls' own check state.
    // They are BS_OWNERDRAW, so IsDlgButtonChecked answers 0 on both no matter
    // what happened, and asserting on it would pin the broken behaviour in
    // place. The meaningful assertion is against AppWindow::current_mode().
    {
        HWND manual = GetDlgItem(main, oz::ui::IDC_RADIO_MANUAL);
        HWND loadlib = GetDlgItem(main, oz::ui::IDC_RADIO_LOADLIB);
        check(L"mode radios exist", manual && loadlib);

        if (manual && loadlib) {
            std::fwprintf(stderr, L"  radio style low byte: 0x%X (BS_OWNERDRAW=0xB)\n",
                          static_cast<unsigned>(
                              GetWindowLongPtrW(manual, GWL_STYLE) & 0xF));
            std::fwprintf(stderr, L"  default mode: %ls\n",
                          window.current_mode() == oz::InjectMode::ManualMap
                              ? L"ManualMap" : L"LoadLibrary");

            SendMessageW(loadlib, BM_CLICK, 0, 0);
            std::fwprintf(stderr, L"  after clicking LoadLibrary: %ls\n",
                          window.current_mode() == oz::InjectMode::ManualMap
                              ? L"ManualMap" : L"LoadLibrary");
            check(L"clicking LoadLibrary selects it",
                  window.current_mode() == oz::InjectMode::LoadLibrary);

            SendMessageW(manual, BM_CLICK, 0, 0);
            std::fwprintf(stderr, L"  after clicking Manual: %ls\n",
                          window.current_mode() == oz::InjectMode::ManualMap
                              ? L"ManualMap" : L"LoadLibrary");
            check(L"clicking Manual selects it",
                  window.current_mode() == oz::InjectMode::ManualMap);

            // The controls report nothing, so a test that trusts them is worse
            // than no test. Record the asymmetry so it is not rediscovered.
            check(L"radios have no usable check state (expected)",
                  IsDlgButtonChecked(main, oz::ui::IDC_RADIO_MANUAL) ==
                      BST_UNCHECKED &&
                      IsDlgButtonChecked(main, oz::ui::IDC_RADIO_LOADLIB) ==
                          BST_UNCHECKED);
        }
    }

    HWND logbox = nullptr;
    child = GetWindow(main, GW_CHILD);
    while (child) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        // The process list is a SysListView32; the log is a plain Edit.
        if (wcscmp(cls, L"Edit") == 0 && GetDlgCtrlID(child) == oz::ui::IDC_LOG) {
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
        HWND refresh = GetDlgItem(main, oz::ui::IDC_REFRESH);
        check(L"refresh button", refresh != nullptr);
        if (refresh) {
            SendMessageW(main, WM_COMMAND, MAKEWPARAM(oz::ui::IDC_REFRESH, BN_CLICKED),
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

        // Reproduce the reported "scrolling down snaps back to the top" bug.
        // The cause was the 1 s timer calling refresh_processes() every tick,
        // which rebuilt the whole ListView and reset the top index.
        //
        // Three things this deliberately does NOT do, each of which made an
        // earlier version of this test wrong rather than flaky-to-pass:
        //
        //  - Assert a specific row index. The list is re-sorted on every
        //    enumeration (windowed processes first, then by PID), so row k
        //    holds a different process after a rebuild.
        //  - Anchor on a chosen row. LVM_ENSUREVISIBLE scrolls the *minimum*
        //    amount, so anchoring row 16 leaves the top at row 5, and the UI
        //    then correctly preserves row 5's process — not the one the test
        //    picked. The test was asserting an expectation the implementation
        //    is not asked to meet.
        //  - Use a row near the bottom. Those are the newest, shortest-lived
        //    processes on the machine and routinely exit mid-test.
        //
        // What the UI actually promises: the row that was at the top is still
        // on screen afterwards. So the test scrolls somewhere, records which
        // process that put at the top, and checks that it is still visible.
        if (count > 20) {
            SendMessageW(list, LVM_ENSUREVISIBLE,
                         static_cast<WPARAM>(count / 2), 0);
            const int top_before =
                static_cast<int>(SendMessageW(list, LVM_GETTOPINDEX, 0, 0));
            const DWORD anchor_pid = window.top_row_pid();
            std::fwprintf(stderr,
                          L"  scrolled: top=%d anchor_pid=%lu count=%d\n",
                          top_before, anchor_pid, count);
            check(L"list scrolled away from top", top_before > 0);
            check(L"read the anchor pid", anchor_pid != 0);

            // Force a rebuild through the same path the timer uses. Polling
            // WM_TIMER cannot be relied on to rebuild anything, because
            // refresh_processes() early-outs when nothing changed - and on an
            // idle machine nothing does. Testing "the scroll survived" against
            // a no-op proves nothing.
            window.refresh_for_test();

            const int top_after =
                static_cast<int>(SendMessageW(list, LVM_GETTOPINDEX, 0, 0));
            const int anchor_row = window.row_of_pid(anchor_pid);
            const int rows = window.visible_rows();
            std::fwprintf(stderr,
                          L"  rebuilt: top=%d anchor row=%d visible=[%d,%d) count=%d\n",
                          top_after, anchor_row, top_after, top_after + rows,
                          ListView_GetItemCount(list));

            check(L"anchored process still exists",
                  window.list_contains_pid(anchor_pid));
            const bool visible = anchor_row >= 0 && anchor_row >= top_after &&
                                 anchor_row < top_after + rows;
            check(L"anchored row still visible after rebuild", visible);
        } else {
            std::fwprintf(stderr,
                          L"  (only %d rows, skipping scroll test)\n", count);
        }
    }

    std::fwprintf(stderr, L"selftest: %hs (%d failure(s))\n",
                  g_failed ? "FAILED" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}

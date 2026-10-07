// oz_injector — application window.
//
// A deliberately plain Win32/GDI UI: an owner-drawn process list, a path field
// with a Browse button, two radio-style injection modes, an Inject button and a
// log pane. No third-party dependency, so it builds anywhere MSVC does.
//
// Threading model: the process list refreshes on a 1s timer on the UI thread
// (it only needs Toolhelp + window titles, which is fast). Injection runs on a
// worker std::thread and posts its result back with PostMessage — inject() is
// synchronous and can block for tens of seconds, so it must never run on the
// message loop.
#ifndef OZ_INJECTOR_UI_H
#define OZ_INJECTOR_UI_H

// The build already defines WIN32_LEAN_AND_MEAN / NOMINMAX / UNICODE on the
// command line; guard so either path works.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

#include "oz_injector_core.h"

namespace oz::ui {

// Colours used by both the owner-drawn list and the chrome, in one place so the
// palette stays consistent.
namespace theme {
constexpr COLORREF bg = RGB(0x14, 0x16, 0x1a);
constexpr COLORREF panel = RGB(0x1b, 0x1e, 0x24);
constexpr COLORREF panel_hi = RGB(0x22, 0x26, 0x2e);
constexpr COLORREF border = RGB(0x30, 0x35, 0x40);
constexpr COLORREF text = RGB(0xe4, 0xe8, 0xf0);
constexpr COLORREF text_dim = RGB(0x8b, 0x93, 0xa3);
constexpr COLORREF accent = RGB(0x5b, 0x9d, 0xf5);
constexpr COLORREF ok = RGB(0x3f, 0xc9, 0x7a);
constexpr COLORREF warn = RGB(0xe0, 0xa8, 0x3e);
constexpr COLORREF err = RGB(0xe2, 0x56, 0x56);
}  // namespace theme

// Child control ids. These live in the header rather than being private to the
// .cpp because the self test and the renderer have to name the same controls;
// they used to be bare numbers (3102, 3103) repeated across three files, which
// is exactly the kind of duplication that silently stops matching.
constexpr int IDC_RADIO_MANUAL  = 2001;
constexpr int IDC_RADIO_LOADLIB = 2002;
constexpr int IDC_BROWSE        = 3101;
constexpr int IDC_INJECT        = 3102;
constexpr int IDC_REFRESH       = 3103;
constexpr int IDC_LOG           = 3104;

class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    bool create(HINSTANCE instance);
    int run();

    // Set when create() fails: the Win32 error from RegisterClassExW or
    // CreateWindowExW, so the caller can explain what actually went wrong.
    DWORD last_error() const { return last_error_; }

    // Which injection mode is selected. Public because the self test has to be
    // able to assert that clicking an option actually changes it — the radio
    // controls cannot report their own state, so this is the only place the
    // answer comes from.
    oz::InjectMode current_mode() const { return mode_; }

    // Rebuild the process list from a fresh enumeration, as the 1 s timer does.
    // Exposed so the self test can force the exact code path that used to reset
    // the scroll position.
    //
    // `force` bypasses the change detection. Without it the call is a no-op on
    // an idle machine — refresh_processes() early-outs when nothing changed —
    // and a test asserting "the scroll survived" would be asserting against a
    // function that never ran.
    void refresh_for_test(bool force = true) { refresh_processes(force); }

    // The PID of the row currently at the top of the list, or 0. This is the
    // anchor the UI preserves across a rebuild, read through the same cache the
    // restore path uses. A test that reads the PID out of the control's display
    // text instead is reading a different thing and will disagree whenever the
    // two lists are out of step.
    // Raw LVM_GETTOPINDEX, exposed for diagnostics.
    LRESULT top_index_raw() const {
        // LVM_GETTOPINDEX == LVM_FIRST + 39 (CommCtrl.h). Spelled out because
        // <commctrl.h> is not included here and the macro is gated on defines
        // this header does not set — the same reason LVM_SETFONT is hardcoded
        // in the .cpp. Do not "simplify" this to +27: that is a different,
        // unassigned message and returns -1.
        constexpr UINT kLvmGetTopIndex = 0x1000 + 39;
        return SendMessageW(list_, kLvmGetTopIndex, 0, 0);
    }

    DWORD top_row_pid() const {
        const int top = static_cast<int>(top_index_raw());
        if (top < 0 || top >= static_cast<int>(processes_.size())) return 0;
        return processes_[static_cast<size_t>(top)].pid;
    }

    // Does the list still contain a row for this PID? Lets a test tell "the
    // scroll anchor was lost" apart from "the process the anchor named exited,
    // so there was nothing to restore".
    bool list_contains_pid(DWORD pid) const {
        return row_of_pid(pid) >= 0;
    }

    // Row index of the given PID in the current list, or -1.
    int row_of_pid(DWORD pid) const {
        for (size_t i = 0; i < processes_.size(); ++i) {
            if (processes_[i].pid == pid) return static_cast<int>(i);
        }
        return -1;
    }

    // How many rows fit on screen. Only an estimate from the client height and
    // the row height, which is all a test needs to reason about visibility.
    int visible_rows() const {
        RECT rc{};
        if (!GetClientRect(list_, &rc)) return 1;
        const int h = rc.bottom - rc.top;
        return h > 0 ? h / 20 : 1;  // ~20 px per row with the default font
    }

    // The main window, so the caller can show it after create().
    HWND handle_hwnd() const { return hwnd_; }

private:
    // message handlers
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);

    void layout();
    // `force` skips the change detection and rebuilds unconditionally. Only the
    // self test needs it; the timer and button always pass the default.
    void refresh_processes(bool force = false);
    // `anchor_pid` is the PID of the row that must stay at the top after the
    // rebuild, or 0 for no preference. The caller resolves it from the list
    // that is still in the control — passing an index here would mean indexing
    // one array with another one's coordinates.
    void populate_list(bool keep_selection, DWORD anchor_pid);
    void append_log(const std::wstring& line, COLORREF colour);
    void clear_log();
    void do_inject();
    void set_status(const std::wstring& text);

    // Draw helpers
    void draw_list(NMHDR* header);

    // helpers
    DWORD selected_pid() const;
    void enable_controls();
    std::wstring selected_dll() const;
    static std::wstring basename(const std::wstring& path);

    HINSTANCE instance_ = nullptr;
    DWORD last_error_ = 0;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND path_edit_ = nullptr;
    HWND browse_ = nullptr;
    HWND btn_manual_ = nullptr;
    HWND btn_loadlib_ = nullptr;
    HWND inject_ = nullptr;
    HWND refresh_ = nullptr;
    HWND log_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_ui_ = nullptr;
    HFONT font_bold_ = nullptr;
    HFONT font_mono_ = nullptr;
    HBRUSH background_ = nullptr;  // owned; freed in the dtor
    HBRUSH bg_brush_ = nullptr;    // window bg, reused for WM_CTLCOLOR*

    // Returns true when `next` is row-for-row identical to the cached list, in
    // which case the ListView does not need rebuilding at all.
    bool same_as_list(const std::vector<oz::ProcessInfo>& next) const;

    std::vector<oz::ProcessInfo> processes_;
    DWORD injected_pid_ = 0;   // pid we already injected into (this session)
    int selected_index_ = -1;
    UINT_PTR timer_id_ = 1;
    HANDLE worker_ = nullptr;
    bool injecting_ = false;
    int log_lines_ = 0;

    // Which injection mode the user picked.
    //
    // This cannot be read back from the radio buttons. They are created with
    // BS_OWNERDRAW so the dark chrome can be drawn by hand, and that style
    // occupies the button-type bits: the control is not a radio button at all.
    // CheckRadioButton() on it silently does nothing, IsDlgButtonChecked()
    // always answers 0, and BM_CLICK never flips the state. A selection made
    // through the Win32 radio API is therefore not observable, so the answer
    // lives here and the paint handler reads it from here.
    oz::InjectMode mode_ = oz::InjectMode::LoadLibrary;

    // Log line colours, parallel to the log listbox contents.
    //
    // These CANNOT live in the listbox: for an owner-draw list box
    // (LBS_OWNERDRAWFIXED) LB_SETITEMDATA / LB_GETITEMDATA are unsupported and
    // DRAWITEMSTRUCT.itemData is left undefined, so anything read back from it
    // is whatever happened to be lying around. Keeping the colours here, indexed
    // by LB_GETCURSEL-style item index, avoids all of that.
    std::vector<COLORREF> log_colours_;
};

// The worker thread's payload. Lives until the window posts WM_APP_INJECT_DONE
// and deletes it.
struct InjectJob {
    AppWindow* owner = nullptr;
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring dll;
    oz::InjectMode mode = oz::InjectMode::ManualMap;
    oz::InjectResult result;
};

}  // namespace oz::ui

#endif  // OZ_INJECTOR_UI_H

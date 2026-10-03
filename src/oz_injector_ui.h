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

    // The main window, so the caller can show it after create().
    HWND handle_hwnd() const { return hwnd_; }

private:
    // message handlers
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);

    void layout();
    void refresh_processes();
    void populate_list(bool keep_selection);
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
    oz::InjectMode current_mode() const;
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

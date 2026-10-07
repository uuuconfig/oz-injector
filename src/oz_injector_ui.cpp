// oz_injector — application window implementation. See oz_injector_ui.h.
#include "oz_injector_ui.h"

#include <commctrl.h>
#include <commdlg.h>
// EM_SETCHARFORMAT / CHARFORMATW come from here.
#include <richedit.h>
// SetWindowTheme, for the dark-mode list header.
#include <uxtheme.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <thread>

#include "injector_resources.h"

namespace oz::ui {

namespace {

constexpr UINT WM_APP_INJECT_DONE = WM_APP + 1;
constexpr int LIST_ID = 1001;
constexpr int kMaxLogLines = 400;   // bound the log so long runs stay fast

// Column order in the process list.
enum Col { COL_PID = 0, COL_NAME, COL_TITLE, COL_STATE, COL_COUNT };

// Non-client colours so the title bar matches the dark chrome on Win10/11.
constexpr UINT_PTR kDarkTitle = 1;

void make_font(HFONT* out, int px_height, int weight, bool mono) {
    LOGFONTW lf{};
    lf.lfHeight = -px_height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    wcscpy_s(lf.lfFaceName, mono ? L"Consolas" : L"Segoe UI");
    *out = CreateFontIndirectW(&lf);
}

// Flat, borderless radio look: the radio dot is drawn by the paint handler, so
// the control itself is a plain STATIC that we draw over. The ids themselves
// live in the header so the self test and renderer name the same controls.

}  // namespace

// ---------------------------------------------------------------------------

AppWindow::AppWindow() = default;

AppWindow::~AppWindow() {
    if (timer_id_) KillTimer(hwnd_, timer_id_);
    if (worker_) {
        WaitForSingleObject(worker_, INFINITE);
        CloseHandle(worker_);
    }
    if (font_ui_) DeleteObject(font_ui_);
    if (font_bold_) DeleteObject(font_bold_);
    if (font_mono_) DeleteObject(font_mono_);
    if (background_) DeleteObject(background_);
}

std::wstring AppWindow::basename(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

// ---------------------------------------------------------------------------
// creation
// ---------------------------------------------------------------------------

bool AppWindow::create(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AppWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // A real brush handle, not a COLORREF cast into HBRUSH — that would hand
    // the window an invalid brush and CreateWindowExW would fail.
    wc.hbrBackground = bg_brush_ = background_ = CreateSolidBrush(theme::bg);
    wc.lpszClassName = L"oz_injector_window";
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    // Registering an already-registered class is not a failure: it happens
    // when a second instance starts in the same session. Treat it as success
    // and let CreateWindowExW decide.
    if (!RegisterClassExW(&wc)) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            last_error_ = err;
            return false;
        }
        // Already registered by an earlier instance in this session — fine,
        // CreateWindowExW decides whether we can still make a window.
    }

    hwnd_ = CreateWindowExW(
        0, wc.lpszClassName, L"oz injector",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
        nullptr, nullptr, instance, this);
    if (!hwnd_) {
        // Leave the real reason in the thread's last-error for the caller.
        last_error_ = GetLastError();
        return false;
    }
    return true;
}

int AppWindow::run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK AppWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    AppWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<AppWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // hwnd_ is not assigned yet — CreateWindowExW has not returned — so
        // publish it now. Everything in handle() goes through this member, and
        // WM_CREATE fires before CreateWindowExW returns.
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(msg, w, l);
    return DefWindowProcW(hwnd, msg, w, l);
}

LRESULT AppWindow::handle(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_NCCREATE: {
            // MUST be handled and MUST return TRUE: falling through to
            // DefWindowProcW returns FALSE, which aborts creation with
            // ERROR_INVALID_WINDOW_HANDLE (1400).
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(this));
            return TRUE;
        }

        case WM_CREATE: {
            make_font(&font_ui_, 15, FW_NORMAL, false);
            make_font(&font_bold_, 15, FW_SEMIBOLD, false);
            make_font(&font_mono_, 14, FW_NORMAL, true);

            // Process list.
            //
            // LVS_REPORT + NM_CUSTOMDRAW, deliberately NOT LVS_OWNERDRAWFIXED:
            // an owner-draw list view receives WM_DRAWITEM and never emits the
            // per-item CDDS_ITEMPREPAINT notifications that draw_list() handles,
            // so every row would stay blank.
            list_ = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(LIST_ID)), instance_, nullptr);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT |
                                                       LVS_EX_DOUBLEBUFFER |
                                                       LVS_EX_HEADERDRAGDROP);
            // The comctl32 convenience macros (ListView_*, SetRedraw) are not
            // used: this file does not rely on COMCTL_VERSION being defined
            // before <commctrl.h> is parsed, so the raw messages go out
            // instead. LVM_SETFONT == LVM_FIRST + 67.
            constexpr UINT kLvmSetFont = 0x1000 + 67;
            SendMessageW(list_, kLvmSetFont, 0,
                         reinterpret_cast<LPARAM>(font_ui_));

            // Ask comctl32 for dark-mode chrome so the built-in header control
            // matches the rest of the UI. Available on Windows 10 1809+; a
            // failure just leaves the system theme, which is harmless.
            SetWindowTheme(list_, L"DarkMode_Explorer", nullptr);
            SetWindowTheme(GetWindow(list_, GW_CHILD), L"DarkMode_Explorer",
                           nullptr);

            const struct {
                const wchar_t* title;
                int width;
            } cols[] = {
                {L"PID", 70},
                {L"Process", 130},
                {L"Window title", 380},
                {L"State", 110},
            };
            for (int i = 0; i < COL_COUNT; ++i) {
                LVCOLUMNW c{};
                c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                c.pszText = const_cast<LPWSTR>(cols[i].title);
                c.cx = cols[i].width;
                c.iSubItem = i;
                ListView_InsertColumn(list_, i, &c);
            }

            path_edit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(3100)), instance_, nullptr);
            SendMessageW(path_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);

            browse_ = CreateWindowExW(
                0, L"BUTTON", L"Browse…",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BROWSE)), instance_, nullptr);
            SendMessageW(browse_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);

            btn_manual_ = CreateWindowExW(
                0, L"BUTTON", L"Manual map (stealth)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RADIO_MANUAL)), instance_, nullptr);
            btn_loadlib_ = CreateWindowExW(
                0, L"BUTTON", L"LoadLibrary (classic)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RADIO_LOADLIB)), instance_, nullptr);
            // The default mode deliberately lives in AppWindow::mode_ rather
            // than in a CheckRadioButton call: the controls are BS_OWNERDRAW,
            // which consumes the button-type bits, so the radio API is a no-op
            // on them and any state set there is unobservable.

            inject_ = CreateWindowExW(
                0, L"BUTTON", L"Inject",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INJECT)), instance_, nullptr);
            SendMessageW(inject_, WM_SETFONT, reinterpret_cast<WPARAM>(font_bold_), TRUE);

            refresh_ = CreateWindowExW(
                0, L"BUTTON", L"Refresh",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)), instance_, nullptr);
            SendMessageW(refresh_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);

            // Log pane: a read-only multiline EDIT.
            //
            // This started out as a RichEdit, then as an LBS_OWNERDRAWFIXED
            // listbox. Both were wrong:
            //   - RichEdit ignores EM_SETBKGNDCOLOR and WM_CTLCOLOREDIT for its
            //     background, so it stayed white and could not be themed.
            //   - An owner-draw listbox does not support LB_SETITEMDATA, leaves
            //     DRAWITEMSTRUCT.itemData undefined, and on a CJK system page
            //     LB_ADDSTRING round-trips through ANSI - which mangled every
            //     non-ASCII character into mojibake.
            // A plain EDIT handles wide text correctly and takes its colours
            // from EM_SETCHARFORMAT, which is exactly what we need.
            log_ = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL | ES_NOHIDESEL,
                0, 0, 0, 0, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), instance_,
                nullptr);
            SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(font_mono_), TRUE);
            // Keep the dark background: an EDIT honours WM_CTLCOLOREDIT only
            // while it is not ES_READONLY on some builds, so set it explicitly.
            SendMessageW(log_, EM_SETBKGNDCOLOR,
                         static_cast<WPARAM>(theme::panel), 0);

            status_ = CreateWindowExW(
                0, L"STATIC", L"Ready.",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
                0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(3105)), instance_, nullptr);
            SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_ui_), TRUE);

            layout();
            refresh_processes();
            timer_id_ = SetTimer(hwnd_, timer_id_, 1000, nullptr);
            append_log(L"oz injector ready. Select a process, pick a DLL, press Inject.",
                       theme::text_dim);
            return 0;
        }

        case WM_SIZE:
            layout();
            return 0;

        case WM_TIMER:
            if (w == timer_id_) refresh_processes();
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->idFrom == LIST_ID) {
                if (hdr->code == NM_CUSTOMDRAW) {
                    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                    draw_list(hdr);
                    return (LRESULT)cd->nmcd.uItemState & CDRF_NEWFONT;
                }
                if (hdr->code == LVN_ITEMCHANGED) {
                    auto* item = reinterpret_cast<NMLISTVIEW*>(l);
                    if (item->iItem >= 0) {
                        selected_index_ = item->iItem;
                        enable_controls();
                    }
                }
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* di = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (di->CtlType != ODT_BUTTON) return 0;
            const bool is_radio = (di->CtlID == IDC_RADIO_MANUAL || di->CtlID == IDC_RADIO_LOADLIB);
            // "Checked" is our own state, not the control's: these are
            // BS_OWNERDRAW controls, so IsDlgButtonChecked would always say 0
            // and the dot would never light up.
            const bool checked =
                is_radio && ((di->CtlID == IDC_RADIO_MANUAL)
                                 ? mode_ == oz::InjectMode::ManualMap
                                 : mode_ == oz::InjectMode::LoadLibrary);
            const bool hot = (di->itemState & ODS_SELECTED) != 0;
            const bool on = (di->itemState & ODS_DISABLED) == 0;

            RECT rc = di->rcItem;
            COLORREF bg = theme::panel_hi, fg = theme::text, edge = theme::border;
            if (di->CtlID == IDC_INJECT) {  // Inject — accent when actionable.
                if (hot && on) { bg = RGB(0x2a, 0x53, 0x8f); fg = RGB(0xff, 0xff, 0xff); }
                else if (!on) { bg = RGB(0x25, 0x28, 0x2f); fg = theme::text_dim; }
                else { bg = theme::accent; fg = RGB(0x0b, 0x14, 0x22); }
                edge = RGB(0x33, 0x66, 0xa8);
            } else if (is_radio) {
                bg = theme::panel;
                if (checked) { fg = theme::accent; edge = theme::accent; }
                if (!on) fg = theme::text_dim;
            } else if (!on) {
                fg = theme::text_dim;
            }

            HBRUSH b = CreateSolidBrush(bg);
            FillRect(di->hDC, &rc, b);
            DeleteObject(b);
            FrameRect(di->hDC, &rc, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

            HFONT font = (di->CtlID == IDC_INJECT) ? font_bold_ : font_ui_;
            SelectObject(di->hDC, font);
            SetBkMode(di->hDC, TRANSPARENT);
            SetTextColor(di->hDC, fg);

            if (is_radio) {
                // Draw the radio dot ourselves so it matches the dark chrome.
                const int d = 12;
                const int cy = (rc.top + rc.bottom) / 2;
                RECT dot = {rc.left + 10, cy - d / 2, rc.left + 10 + d, cy + d / 2};
                HBRUSH db = CreateSolidBrush(theme::bg);
                FillRect(di->hDC, &dot, db);
                DeleteObject(db);
                HBRUSH db2 = CreateSolidBrush(checked ? theme::accent : theme::border);
                FrameRect(di->hDC, &dot, db2);
                DeleteObject(db2);
                if (checked) {
                    RECT inner = {dot.left + 4, dot.top + 4, dot.right - 4,
                                  dot.bottom - 4};
                    HBRUSH ib = CreateSolidBrush(theme::accent);
                    FillRect(di->hDC, &inner, ib);
                    DeleteObject(ib);
                }
                rc.left += 10 + d + 8;
            }

            // The caption must come from the control itself. DRAWITEMSTRUCT's
            // itemData is the *ListBox / ComboBox / ListView item data* — for a
            // BS_OWNERDRAW button it carries whatever was passed as lpParam to
            // CreateWindowEx (nullptr here), so drawing it produced an empty
            // coloured block with no text. GetWindowText is the documented way
            // to reach a button's caption during WM_DRAWITEM.
            wchar_t caption[128] = {0};
            GetWindowTextW(di->hwndItem, caption,
                           static_cast<int>(std::size(caption)));

            DrawTextW(di->hDC, caption, -1, &rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            // WM_CTLCOLORLISTVIEW shares WM_CTLCOLORLISTBOX's value, so it is
            // already covered above — there is no separate constant for it.
            auto* dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, theme::text);
            SetBkColor(dc, theme::bg);
            if (bg_brush_) return reinterpret_cast<LRESULT>(bg_brush_);
            return reinterpret_cast<LRESULT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        case WM_COMMAND: {
            const int id = LOWORD(w);
            const int code = HIWORD(w);
            if (code == BN_CLICKED) {
                switch (id) {
                    case IDC_BROWSE: {  // Browse
                        OPENFILENAMEW ofn{};
                        wchar_t file[MAX_PATH] = {0};
                        ofn.lStructSize = sizeof ofn;
                        ofn.hwndOwner = hwnd_;
                        ofn.lpstrFilter = L"DLL files\0*.dll\0All files\0*.*\0\0";
                        ofn.lpstrFile = file;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.lpstrTitle = L"Select a DLL to inject";
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
                        if (GetOpenFileNameW(&ofn)) {
                            SetWindowTextW(path_edit_, file);
                            set_status(L"Selected " + basename(file));
                            enable_controls();
                        }
                        return 0;
                    }
                    case IDC_INJECT:  // Inject
                        do_inject();
                        return 0;
                    case IDC_REFRESH:  // Refresh
                        refresh_processes();
                        append_log(L"Process list refreshed manually.", theme::text_dim);
                        return 0;
                    default:
                        break;
                }
            }
            if (id == IDC_RADIO_MANUAL || id == IDC_RADIO_LOADLIB) {
                if (code == BN_CLICKED) {
                    // Record the choice ourselves. The controls are
                    // BS_OWNERDRAW, not radio buttons (see the comment on
                    // AppWindow::mode_), so BM_SETCHECK / IsDlgButtonChecked
                    // cannot carry it.
                    mode_ = (id == IDC_RADIO_MANUAL) ? oz::InjectMode::ManualMap
                                                     : oz::InjectMode::LoadLibrary;
                    enable_controls();
                    InvalidateRect(btn_manual_, nullptr, TRUE);
                    InvalidateRect(btn_loadlib_, nullptr, TRUE);
                }
                return 0;
            }
            if (id == 3100 && code == EN_CHANGE) {
                enable_controls();
                return 0;
            }
            return 0;
        }

        case WM_APP_INJECT_DONE: {
            auto* job = reinterpret_cast<InjectJob*>(l);
            if (!job) return 0;

            injecting_ = false;
            if (worker_) {
                WaitForSingleObject(worker_, 0);
                CloseHandle(worker_);
                worker_ = nullptr;
            }

            const bool ok = job->result.status == oz::InjectStatus::Ok;
            const COLORREF tint =
                ok ? theme::ok : (job->result.status == oz::InjectStatus::AlreadyInjected
                                     ? theme::warn
                                     : theme::err);

            append_log(L"──────── injection finished ────────", theme::border);
            for (const auto& line : job->result.log) {
                append_log(L"  " + line, theme::text_dim);
            }
            append_log(L"  → " + std::wstring(oz::inject_status_text(job->result.status)) +
                           (job->result.detail.empty() ? std::wstring()
                                                       : L": " + job->result.detail),
                       tint);
            if (ok) {
                injected_pid_ = job->pid;
                append_log(L"  Note: a manually mapped image stays resident; there is "
                           L"no unload path.",
                           theme::warn);
            }
            set_status(ok ? L"Injected into PID " + std::to_wstring(job->pid) +
                                L" — " + std::wstring(oz::inject_mode_name(job->mode))
                          : std::wstring(oz::inject_status_text(job->result.status)) +
                                L": " + job->result.detail);

            delete job;
            enable_controls();
            refresh_processes();
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (timer_id_) {
                KillTimer(hwnd_, timer_id_);
                timer_id_ = 0;
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

// ---------------------------------------------------------------------------
// layout + paint
// ---------------------------------------------------------------------------

void AppWindow::layout() {
    if (!hwnd_) return;
    RECT c{};
    GetClientRect(hwnd_, &c);
    const int pad = 16;
    const int gap = 10;
    const int width = c.right;

    int y = pad;
    // The list takes half the client height; the rest is shared by the two
    // control rows, the log and the status line.
    const int list_h = (c.bottom - pad * 2) * 48 / 100;
    MoveWindow(list_, pad, y, width - pad * 2, list_h, TRUE);
    y += list_h + gap;

    // Row 1: DLL path + Browse.
    const int browse_w = 90;
    MoveWindow(path_edit_, pad, y, width - pad * 2 - browse_w - gap, 26, TRUE);
    MoveWindow(browse_, width - pad - browse_w, y, browse_w, 26, TRUE);
    y += 26 + gap;

    // Row 2: mode radios on the left, Refresh + Inject on the right.
    const int inject_w = 120;
    const int refresh_w = 90;
    MoveWindow(btn_manual_, pad, y, 190, 26, TRUE);
    MoveWindow(btn_loadlib_, pad + 200, y, 190, 26, TRUE);
    MoveWindow(refresh_, width - pad - inject_w - refresh_w - gap, y, refresh_w, 26,
               TRUE);
    MoveWindow(inject_, width - pad - inject_w, y, inject_w, 26, TRUE);
    y += 26 + gap;

    // Remaining vertical space goes to the log.
    const int status_h = 22;
    MoveWindow(log_, pad, y, width - pad * 2, c.bottom - y - status_h - pad, TRUE);
    MoveWindow(status_, pad, c.bottom - status_h - pad / 2, width - pad * 2, status_h,
               TRUE);

    // Columns are fixed except the title, which absorbs whatever width is left.
    // Without this the list leaves a wide empty gutter on the right at larger
    // window sizes, and the title column truncates needlessly.
    const int col_pid = 70;
    const int col_name = 130;
    const int col_state = 110;
    const int content_w = width - pad * 2;
    const int title_w = content_w - col_pid - col_name - col_state;
    if (title_w > 80) {
        ListView_SetColumnWidth(list_, COL_PID, col_pid);
        ListView_SetColumnWidth(list_, COL_NAME, col_name);
        ListView_SetColumnWidth(list_, COL_TITLE, title_w);
        ListView_SetColumnWidth(list_, COL_STATE, col_state);
    }
}

void AppWindow::draw_list(NMHDR* header) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    const int stage = cd->nmcd.dwDrawStage;
    auto* hdc = cd->nmcd.hdc;

    // Rows only. The header strip is left to the ListView's own header control
    // — painting it at CDDS_PREPAINT as well draws the same pixels twice and
    // leaves ghosting behind the real header.
    if (stage != CDDS_ITEMPREPAINT) return;

    const int index = static_cast<int>(cd->nmcd.dwItemSpec);
    if (index < 0 || index >= static_cast<int>(processes_.size())) return;
    const auto& pi = processes_[static_cast<size_t>(index)];

    RECT rc = cd->nmcd.rc;
    const bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;

    // Row background: selection tint, else alternating stripes.
    COLORREF row_bg = (index & 1) ? theme::panel : theme::bg;
    if (selected) row_bg = RGB(0x1c, 0x2c, 0x44);
    HBRUSH brush = CreateSolidBrush(row_bg);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme::text);

    // Read the live column widths rather than hardcoding them: layout() makes
    // the title column elastic, so a fixed width here would drift out of sync
    // and draw each cell in the wrong place.
    int col_w[COL_COUNT] = {0};
    for (int i = 0; i < COL_COUNT; ++i) {
        col_w[i] = ListView_GetColumnWidth(list_, i);
        if (col_w[i] <= 0) col_w[i] = 100;  // before layout has run
    }

    struct Cell { const wchar_t* text; int width; };
    const std::wstring pid_s = std::to_wstring(pi.pid);
    const std::wstring state_s =
        pi.injected ? L"injected"
                    : (pi.elevated ? L"elevated"
                                   : (pi.window_title.empty() ? L"no window" : L"ready"));
    const Cell cells[COL_COUNT] = {
        {pid_s.c_str(), col_w[COL_PID]},
        {pi.exe_name.c_str(), col_w[COL_NAME]},
        {pi.window_title.empty() ? pi.exe_path.c_str() : pi.window_title.c_str(),
         col_w[COL_TITLE]},
        {state_s.c_str(), col_w[COL_STATE]},
    };

    int x = rc.left + 6;
    for (int i = 0; i < COL_COUNT; ++i) {
        RECT cell = rc;
        cell.left = x;
        cell.right = x + cells[i].width - 8;
        SelectObject(hdc, (i == COL_PID || i == COL_STATE) ? font_mono_ : font_ui_);
        if (i == COL_STATE) {
            const bool ok = pi.injected || (!pi.elevated && !pi.window_title.empty());
            SetTextColor(hdc, pi.injected
                                  ? theme::ok
                                  : (ok ? theme::text_dim : theme::warn));
        } else if (i == COL_PID) {
            SetTextColor(hdc, theme::text_dim);
        }
        DrawTextW(hdc, cells[i].text, -1, &cell,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        x += cells[i].width;
    }
}

// ---------------------------------------------------------------------------
// process list
// ---------------------------------------------------------------------------

void AppWindow::refresh_processes(bool force) {
    const DWORD prev_pid =
        (selected_index_ >= 0 && selected_index_ < static_cast<int>(processes_.size()))
            ? processes_[static_cast<size_t>(selected_index_)].pid
            : 0;

    std::vector<oz::ProcessInfo> next = oz::enumerate_processes(/*java_only=*/false);
    if (injected_pid_) {
        for (auto& p : next) {
            if (p.pid == injected_pid_) p.injected = true;
        }
    }

    // A timer ticks once a second, and rebuilding the list means DeleteAllItems
    // plus a re-insert of every row. That resets the scroll position to the top
    // and makes the list jump under the cursor, so only touch the control when
    // something actually changed. The set of PIDs is what the user sees move;
    // a changed window title is worth a refresh too, but nothing else is.
    if (!force && same_as_list(next)) return;

    // Resolve the scroll anchor *before* overwriting processes_. The anchor is
    // looked up by row index, and the row index refers to the list currently in
    // the control — which is still the old one. Reading it after the assignment
    // below would index the new array with the old array's index: both lists
    // have different contents, so the resulting pid would be arbitrary and its
    // restore loop would silently find nothing, leaving the view at the top.
    DWORD anchor_pid = top_row_pid();
    processes_ = std::move(next);
    populate_list(prev_pid != 0, anchor_pid);
}

bool AppWindow::same_as_list(const std::vector<oz::ProcessInfo>& next) const {
    if (next.size() != processes_.size()) return false;
    for (size_t i = 0; i < next.size(); ++i) {
        const auto& a = next[i];
        const auto& b = processes_[i];
        if (a.pid != b.pid || a.window_title != b.window_title ||
            a.injected != b.injected || a.elevated != b.elevated) {
            return false;
        }
    }
    return true;
}

void AppWindow::populate_list(bool keep_selection, DWORD anchor_pid) {
    // `anchor_pid` is the row that was at the top, resolved by the caller while
    // the old list was still in place. DeleteAllItems resets the list's top
    // index to 0, so without this any rebuild yanks the view back to the first
    // row. The anchor is a PID rather than a row index because rows shift when
    // processes come and go.
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);

    int restore = -1;
    for (size_t i = 0; i < processes_.size(); ++i) {
        const auto& pi = processes_[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.lParam = static_cast<LPARAM>(pi.pid);
        const std::wstring pid_s = std::to_wstring(pi.pid);
        item.pszText = const_cast<LPWSTR>(pid_s.c_str());
        const int idx = ListView_InsertItem(list_, &item);

        const std::wstring name = pi.exe_name;
        ListView_SetItemText(list_, idx, COL_NAME,
                             const_cast<LPWSTR>(name.c_str()));
        const std::wstring title =
            pi.window_title.empty() ? pi.exe_path : pi.window_title;
        ListView_SetItemText(list_, idx, COL_TITLE,
                             const_cast<LPWSTR>(title.c_str()));
        const std::wstring state = pi.injected ? L"injected"
                                               : (pi.elevated ? L"elevated" : L"ready");
        ListView_SetItemText(list_, idx, COL_STATE,
                             const_cast<LPWSTR>(state.c_str()));

        if (keep_selection && pi.pid == selected_pid()) restore = idx;
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);

    // Re-establish the scroll anchor before the selection, so the view lands
    // where the user left it.
    //
    // LVM_ENSUREVISIBLE takes the item index in wParam and requires lParam to be
    // zero - the ListView_EnsureVisible macro passes it that way. Passing an
    // LVITEM in lParam (the LVM_SETITEMSTATE shape) is accepted silently and
    // acts on item 0, which is why an earlier version of this restore appeared
    // to work while actually pinning the view to the first row.
    for (size_t i = 0; i < processes_.size(); ++i) {
        if (processes_[i].pid != anchor_pid) continue;
        SendMessageW(list_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(i), 0);
        break;
    }

    if (restore >= 0) {
        ListView_SetItemState(list_, restore, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        selected_index_ = restore;
    } else {
        selected_index_ = -1;
    }
    enable_controls();
    set_status(std::to_wstring(processes_.size()) + L" process(es) — " +
               (selected_index_ >= 0 ? L"select a process and a DLL"
                                     : L"select a process"));
}

DWORD AppWindow::selected_pid() const {
    if (selected_index_ < 0 ||
        selected_index_ >= static_cast<int>(processes_.size())) {
        return 0;
    }
    return processes_[static_cast<size_t>(selected_index_)].pid;
}

std::wstring AppWindow::selected_dll() const {
    wchar_t buf[MAX_PATH] = {0};
    GetWindowTextW(path_edit_, buf, MAX_PATH);
    return buf;
}

// ---------------------------------------------------------------------------
// injection
// ---------------------------------------------------------------------------

void AppWindow::do_inject() {
    const DWORD pid = selected_pid();
    if (pid == 0) {
        set_status(L"Select a process first.");
        return;
    }
    if (injecting_) return;

    const std::wstring dll = selected_dll();
    if (dll.empty()) {
        set_status(L"Choose a DLL first.");
        return;
    }

    const oz::InjectMode mode = current_mode();
    if (pid == injected_pid_) {
        append_log(L"Refusing to inject into PID " + std::to_wstring(pid) +
                       L" twice in one session — a manual-mapped image cannot be "
                       L"unloaded.",
                   theme::warn);
        set_status(L"Already injected into this process.");
        return;
    }

    const std::wstring exe =
        processes_[static_cast<size_t>(selected_index_)].exe_name;
    append_log(L"──────── injecting " + basename(dll) + L" into " + exe + L" (PID " +
                   std::to_wstring(pid) + L") via " + oz::inject_mode_name(mode) +
                   L" ────────",
               theme::accent);
    set_status(L"Injecting…");

    auto* job = new InjectJob{};
    job->owner = this;
    job->hwnd = hwnd_;
    job->pid = pid;
    job->dll = dll;
    job->mode = mode;

    injecting_ = true;
    enable_controls();

    // inject() is synchronous and can block for tens of seconds, so it runs on
    // a worker thread and posts the result back.
    worker_ = CreateThread(nullptr, 0,
                           [](LPVOID param) -> DWORD {
                               auto* j = static_cast<InjectJob*>(param);
                               j->result = oz::inject(j->pid, j->dll, j->mode);
                               PostMessageW(j->hwnd, WM_APP_INJECT_DONE, 0,
                                            reinterpret_cast<LPARAM>(j));
                               return 0;
                           },
                           job, 0, nullptr);
    if (!worker_) {
        delete job;
        injecting_ = false;
        set_status(L"Could not start the injection thread.");
        enable_controls();
    }
}

void AppWindow::enable_controls() {
    const bool have_pid = selected_pid() != 0;
    const bool have_dll = !selected_dll().empty();
    const bool can = have_pid && have_dll && !injecting_;

    EnableWindow(inject_, can);
    EnableWindow(btn_manual_, !injecting_);
    EnableWindow(btn_loadlib_, !injecting_);
    EnableWindow(browse_, !injecting_);
    EnableWindow(path_edit_, !injecting_);
    InvalidateRect(btn_manual_, nullptr, TRUE);
    InvalidateRect(btn_loadlib_, nullptr, TRUE);
    InvalidateRect(inject_, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// log + status
// ---------------------------------------------------------------------------

void AppWindow::append_log(const std::wstring& line, COLORREF colour) {
    if (!log_) return;

    // Bound the buffer so a long session does not slow every append down.
    // Keep our own colour array in lockstep, since it is what the draw code
    // consults (the EDIT's own char formats get rewritten on every append).
    if (++log_lines_ > kMaxLogLines) {
        const int drop = 120;
        const int n = std::min(drop, static_cast<int>(log_colours_.size()));
        if (n > 0) {
            log_colours_.erase(log_colours_.begin(), log_colours_.begin() + n);
        }
        log_lines_ = static_cast<int>(log_colours_.size());

        // Drop the same number of lines from the top of the control.
        const int chars = GetWindowTextLengthW(log_);
        if (chars > 0) {
            SendMessageW(log_, EM_SETSEL, 0, chars);
            SendMessageW(log_, EM_REPLACESEL, FALSE,
                         reinterpret_cast<LPARAM>(L""));
        }
        log_lines_ = static_cast<int>(log_colours_.size());
    }

    // EM_REPLACESEL takes a wide string, so no ANSI round trip and no mojibake
    // on a CJK system code page - the reason the previous listbox approach was
    // abandoned.
    CHARFORMATW cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = colour;

    const int len = GetWindowTextLengthW(log_);
    SendMessageW(log_, EM_SETSEL, len, len);
    SendMessageW(log_, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&cf));
    SendMessageW(log_, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L"\r\n"));
    SendMessageW(log_, EM_SCROLLCARET, 0, 0);

    log_colours_.push_back(colour);
}

void AppWindow::clear_log() {
    if (log_) SetWindowTextW(log_, L"");
    log_colours_.clear();
    log_lines_ = 0;
}

void AppWindow::set_status(const std::wstring& text) {
    if (status_) SetWindowTextW(status_, text.c_str());
}

}  // namespace oz::ui

// render.cpp — process-internal UI render, for visual verification.
//
// Creates the real window, shows it, and dumps it to a PNG from inside the
// process. An external window enumerator cannot see this sandbox's GUI session,
// so this is how the UI gets visually verified: the same window code runs, we
// just read the pixels ourselves instead of from another process.
#include "oz_injector_ui.h"

#include <windows.h>

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Minimal PNG writer (RGB8, zlib "stored" blocks so we need no zlib).
std::vector<unsigned char> zlib_store(const std::vector<unsigned char>& raw) {
    // zlib stream: 0x78 0x01, then stored deflate blocks, then adler32.
    std::vector<unsigned char> out = {0x78, 0x01};
    const size_t kBlock = 65535;
    size_t pos = 0;
    do {
        const size_t n = std::min(kBlock, raw.size() - pos);
        const bool last = (pos + n) >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<unsigned char>(n & 0xFF));
        out.push_back(static_cast<unsigned char>(n >> 8));
        out.push_back(static_cast<unsigned char>(~n & 0xFF));
        out.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<long>(pos),
                   raw.begin() + static_cast<long>(pos + n));
        pos += n;
    } while (pos < raw.size());

    unsigned long a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const unsigned long adler = (b << 16) | a;
    out.push_back(static_cast<unsigned char>(adler >> 24));
    out.push_back(static_cast<unsigned char>(adler >> 16));
    out.push_back(static_cast<unsigned char>(adler >> 8));
    out.push_back(static_cast<unsigned char>(adler));
    return out;
}

void put_u32(std::vector<unsigned char>& v, unsigned x) {
    v.push_back(static_cast<unsigned char>(x >> 24));
    v.push_back(static_cast<unsigned char>(x >> 16));
    v.push_back(static_cast<unsigned char>(x >> 8));
    v.push_back(static_cast<unsigned char>(x));
}

void png_chunk(std::vector<unsigned char>& out, const char* type,
               const std::vector<unsigned char>& data) {
    put_u32(out, static_cast<unsigned>(data.size()));

    std::vector<unsigned char> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());

    // Standard CRC-32 (IEEE): init 0xFFFFFFFF, reflect per byte, final xor.
    unsigned long c = 0xffffffffUL;
    for (unsigned char b : body) {
        c ^= b;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
    }
    c ^= 0xffffffffUL;
    put_u32(out, static_cast<unsigned>(c));
}

bool write_png(const std::wstring& path, int w, int h,
               const std::vector<unsigned char>& rgb) {
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(h) * (1 + static_cast<size_t>(w) * 3));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  // filter: none
        const size_t off = static_cast<size_t>(y) * w * 3;
        raw.insert(raw.end(), rgb.begin() + static_cast<long>(off),
                   rgb.begin() + static_cast<long>(off + w * 3));
    }
    std::vector<unsigned char> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    put_u32(ihdr, static_cast<unsigned>(w));
    put_u32(ihdr, static_cast<unsigned>(h));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // truecolor
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(out, "IHDR", ihdr);
    png_chunk(out, "IDAT", zlib_store(raw));
    png_chunk(out, "IEND", {});

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
    CloseHandle(f);
    return wrote == out.size();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    oz::ui::AppWindow window;
    if (!window.create(instance)) {
        std::fwprintf(stderr, L"create() failed, error %lu\n", window.last_error());
        return 2;
    }

    HWND hwnd = window.handle_hwnd();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // Force a definite client size so WM_SIZE -> layout() runs with real
    // numbers rather than whatever the create-time default was.
    RECT want{0, 0, 900, 640};
    AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
                       FALSE, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER);

    // Let the 1s refresh timer and the first paint settle.
    MSG msg{};
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < 2500) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(16);
    }

    // Select the first row so the screenshot shows the selected state.
    HWND list = GetWindow(hwnd, GW_CHILD);
    while (list) {
        wchar_t cls[64] = {0};
        GetClassNameW(list, cls, 64);
        if (wcscmp(cls, L"SysListView32") == 0) break;
        list = GetWindow(list, GW_HWNDNEXT);
    }
    if (list) {
        LVITEMW item{};
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list, LVM_SETITEMSTATE, 0,
                     reinterpret_cast<LPARAM>(&item));
    }

    // Press Refresh so the screenshot shows the log with more than one line,
    // and so the line that used to render as mojibake is on display.
    HWND refresh = GetDlgItem(hwnd, oz::ui::IDC_REFRESH);
    if (refresh) {
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(oz::ui::IDC_REFRESH, BN_CLICKED),
                     reinterpret_cast<LPARAM>(refresh));
    }
    // Give the owner-drawn rows a chance to paint with the selection.
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
    Sleep(400);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    std::fwprintf(stderr, L"render: window %dx%d\n", w, h);

    HDC screen = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    // Two passes. PrintWindow with PW_RENDERFULLCONTENT is the documented way
    // to capture a window's own pixels including its client area, but some
    // child controls (ListView / ListBox with themed chrome) are not composited
    // into it. So capture twice and pick the one that actually carries content:
    // print first, and if it comes out uniformly one colour, fall back to a
    // straight BitBlt from the screen.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    auto blit_screen = [&] {
        BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);
    };
    auto read_pixels = [&] {
        std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
        GetDIBits(mem, bmp, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
        return bgra;
    };

    PrintWindow(hwnd, mem, 2);
    std::vector<unsigned char> bgra = read_pixels();

    // Count distinct colours: a blank capture has almost none.
    auto colour_count = [](const std::vector<unsigned char>& px) {
        std::vector<unsigned long> seen;
        const size_t step = 4 * 37;  // sparse sample
        for (size_t i = 0; i + 3 < px.size(); i += step) {
            const unsigned long key = (static_cast<unsigned long>(px[i]) << 16) |
                                      (static_cast<unsigned long>(px[i + 1]) << 8) |
                                      px[i + 2];
            if (seen.size() < 4096) seen.push_back(key);
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        return seen.size();
    };

    std::fwprintf(stderr, L"render: PrintWindow colours=%zu\n", colour_count(bgra));
    if (colour_count(bgra) < 8) {
        std::fwprintf(stderr, L"render: print capture looks blank, using BitBlt\n");
        blit_screen();
        bgra = read_pixels();
        std::fwprintf(stderr, L"render: BitBlt colours=%zu\n", colour_count(bgra));
    }

    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        rgb[i * 3 + 0] = bgra[i * 4 + 2];
        rgb[i * 3 + 1] = bgra[i * 4 + 1];
        rgb[i * 3 + 2] = bgra[i * 4 + 0];
    }

    // Pixel check on the owner-drawn buttons.
    //
    // Asserting GetWindowText on a BS_OWNERDRAW button proves nothing: the
    // caption was always set, the bug was that WM_DRAWITEM drew the wrong
    // pointer. Only the rendered pixels distinguish the two. Each of these
    // buttons is a solid fill, so a correct render has a scattering of pixels
    // far from that fill — the glyphs. A blank block has none.
    {
        int blank = 0;
        const int ids[] = {oz::ui::IDC_RADIO_MANUAL, oz::ui::IDC_RADIO_LOADLIB, oz::ui::IDC_INJECT};
        for (const int id : ids) {
            HWND btn = GetDlgItem(hwnd, id);
            if (!btn) continue;
            RECT br{};
            GetWindowRect(btn, &br);
            // The capture is the whole window, so offset by the window origin.
            const int bx = br.left - r.left;
            const int by = br.top - r.top;
            const int bw = br.right - br.left;
            const int bh = br.bottom - br.top;

            // Most common colour in the button = its background fill.
            std::vector<unsigned long> hist;
            for (int y = 2; y < bh - 2; ++y) {
                for (int x = 2; x < bw - 2; ++x) {
                    const int ix = bx + x;
                    const int iy = by + y;
                    if (ix < 0 || iy < 0 || ix >= w || iy >= h) continue;
                    const size_t o =
                        (static_cast<size_t>(iy) * w + static_cast<size_t>(ix)) * 3;
                    hist.push_back((static_cast<unsigned long>(rgb[o]) << 16) |
                                   (static_cast<unsigned long>(rgb[o + 1]) << 8) |
                                   rgb[o + 2]);
                }
            }
            if (hist.empty()) continue;
            std::sort(hist.begin(), hist.end());
            const unsigned long bg_key = hist[hist.size() / 2];

            // Count pixels far from the fill: that is the text.
            int contrast = 0;
            for (const unsigned long k : hist) {
                const int dr = static_cast<int>((k >> 16) & 0xFF) -
                               static_cast<int>((bg_key >> 16) & 0xFF);
                const int dg = static_cast<int>((k >> 8) & 0xFF) -
                               static_cast<int>((bg_key >> 8) & 0xFF);
                const int db = static_cast<int>(k & 0xFF) -
                               static_cast<int>(bg_key & 0xFF);
                if (dr * dr + dg * dg + db * db > 60 * 60) ++contrast;
            }
            const int pct = contrast * 100 / static_cast<int>(hist.size());            std::fwprintf(stderr,
                          L"render: button %d text pixels %d/%zu (%d%%)\n",
                          id, contrast, hist.size(), pct);
            // A real caption covers a few percent of the button. The broken
            // version renders 0.
            if (contrast * 100 < static_cast<int>(hist.size()) * 2) ++blank;
        }
        if (blank) {
            std::fwprintf(stderr,
                          L"render: FAILED - %d owner-draw button(s) have no text\n",
                          blank);
            return 1;
        }
        std::fwprintf(stderr, L"render: owner-draw button text ok\n");
    }

    wchar_t out[MAX_PATH];
    GetTempPathW(MAX_PATH, out);
    lstrcatW(out, L"oz_injector_ui.png");
    const bool wrote = write_png(out, w, h, rgb);
    std::fwprintf(stderr, L"render: wrote %s (%d)\n", wrote ? L"ok" : L"FAILED", wrote);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    return wrote ? 0 : 1;
}

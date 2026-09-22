// InputWindow.cpp — GDI painting and placement for the floating input box.
//
// See InputWindow.h for what this is for. The notes worth having in front of you
// while reading:
//
//   * WS_EX_NOACTIVATE is not optional. Without it, showing the box steals focus
//     from the host, which ends the composition and breaks typing outright.
//   * HTTRANSPARENT from WM_NCHITTEST makes the box a hole in the hit-test map,
//     so a click that lands on it reaches the document underneath.
//   * DPI is re-read from the host on every layout rather than cached and
//     refreshed on WM_DPICHANGED: we are a DLL inside someone else's process and
//     inherit its DPI awareness, which we cannot change without breaking it. The
//     rects ITfContextView::GetTextExt hands us are already in screen pixels, so
//     they are directly usable in SetWindowPos.

#include "InputWindow.h"
#include "Globals.h"

#include <algorithm>
#include <cwchar>
#include <new>

namespace {

const wchar_t kBoxClass[] = L"DSInputBoxWnd";

// Metrics in DIPs; scaled by the host's DPI at layout time.
const int kPadX = 10;
const int kPadY = 6;
const int kRadius = 6;
const int kBadgeGap = 2;
const int kMaxWidthDip = 640;
const int kAnchorGap = 2;  // between the caret and the box

const COLORREF kBg = RGB(255, 255, 255);
const COLORREF kBorder = RGB(184, 184, 184);
const COLORREF kText = RGB(28, 28, 28);
const COLORREF kBadge = RGB(120, 120, 120);
const COLORREF kBadgeBad = RGB(200, 40, 40);

// "待转换 " — spelled as escapes because the MSVC build here does not pass
// /utf-8, so a literal would depend on the source file's encoding.
const wchar_t kPendingPrefix[] = L"待转换 ";             // 待转换
const wchar_t kFailedBadge[] = L"已复制到剪贴板";  // 已复制到剪贴板

// user32!GetDpiForWindow (Windows 10 1607+), resolved lazily so the DLL still
// loads on older systems — where 96 is the right answer anyway.
int DpiOf(HWND hwnd) {
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = reinterpret_cast<GetDpiForWindowFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    UINT dpi = (fn && hwnd) ? fn(hwnd) : 96;
    return dpi ? static_cast<int>(dpi) : 96;
}

int Scaled(int dip, int dpi) { return ::MulDiv(dip, dpi, 96); }

}  // namespace

// ---- lifecycle -------------------------------------------------------------

DSInputBoxWnd* DSInputBoxWnd::Create(HINSTANCE hInst) {
    if (!_RegisterClass(hInst)) return nullptr;

    DSInputBoxWnd* self = new (std::nothrow) DSInputBoxWnd();
    if (self == nullptr) return nullptr;
    self->_hInst = hInst;

    // WS_POPUP + an owner: a popup with a parent is *owned* by it, so the box
    // minimises with the host and is never left floating over an unrelated app.
    //
    // WS_DISABLED alongside WS_EX_NOACTIVATE is belt and braces, and it is what
    // the Rime/Weasel candidate window uses: a disabled window cannot be
    // activated at all, so even a stray SetFocus cannot take the caret out of
    // the document we are tracking. WS_EX_TOOLWINDOW keeps it out of the taskbar
    // and the alt-tab list.
    self->_hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kBoxClass, L"",
        WS_POPUP | WS_DISABLED, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (self->_hwnd == nullptr) {
        delete self;
        return nullptr;
    }

    ::SetWindowLongPtrW(self->_hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(self));
    return self;
}

DSInputBoxWnd::~DSInputBoxWnd() {
    if (_font) ::DeleteObject(_font);
    if (_smallFont) ::DeleteObject(_smallFont);
}

void DSInputBoxWnd::Destroy() {
    if (_hwnd) {
        ::SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    delete this;
}

BOOL DSInputBoxWnd::_RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW existing = {};
    if (::GetClassInfoExW(hInst, kBoxClass, &existing)) return TRUE;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &DSInputBoxWnd::_WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBoxClass;
    if (::RegisterClassExW(&wc)) return TRUE;
    // A concurrent activation may have registered it first; that is success.
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// ---- content and placement -------------------------------------------------

void DSInputBoxWnd::SetHost(HWND host) {
    if (_host == host) return;
    _host = host;
    if (_hwnd && host) {
        ::SetWindowLongPtrW(_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(host));
    }
}

void DSInputBoxWnd::SetAnchor(const RECT& caret) {
    // A degenerate rect from GetTextExt means "no position available" (the
    // window is minimised, the text has no layout yet, …). Keep the last good
    // one; the box stays where it was rather than snapping to a screen corner.
    if (IsDegenerate(caret)) return;
    // Nothing moved. Worth testing: the layout timer re-probes every 150 ms, and
    // without this an idle caret would drag a SetWindowPos + full repaint along
    // with it six times a second for as long as the pending count is on screen.
    if (::EqualRect(&_caret, &caret)) return;
    _caret = caret;
    if (_visible) _Relayout();
}

void DSInputBoxWnd::SetContent(const std::wstring& pinyin, unsigned pending,
                               bool failed) {
    if (_pinyin == pinyin && _pending == pending && _failed == failed) return;
    _pinyin = pinyin;
    _pending = pending;
    _failed = failed;

    // Nothing being typed and nothing outstanding: there is no box to show.
    //
    // `failed` is an exception and must be tested here. "No pinyin, nothing
    // pending, something went wrong" is exactly the shape of the clipboard
    // fallback — the one state where the badge is the entire message. Hiding on
    // this test would swallow the only signal the user gets that a sentence did
    // not reach the document.
    if (_pinyin.empty() && _pending == 0 && !_failed) {
        Hide();
        return;
    }
    _Relayout();
}

void DSInputBoxWnd::Hide() {
    if (!_hwnd) return;
    if (_visible) {
        ::ShowWindow(_hwnd, SW_HIDE);
        _visible = false;
    }
}

// ---- painting --------------------------------------------------------------

void DSInputBoxWnd::_EnsureFonts(int dpi) {
    if (_font && _dpi == dpi) return;
    if (_font) ::DeleteObject(_font);
    if (_smallFont) ::DeleteObject(_smallFont);

    // Microsoft YaHei UI is the Windows shell UI face and covers CJK; the pinyin
    // line also needs a face that renders Latin decently, which it does.
    _font = ::CreateFontW(-Scaled(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    _smallFont = ::CreateFontW(-Scaled(11, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    _dpi = dpi;
}

void DSInputBoxWnd::_LineHeights(HDC dc, int* outPinyinH, int* outBadgeH) const {
    *outPinyinH = 0;
    *outBadgeH = 0;

    if (!_pinyin.empty()) {
        HGDIOBJ old = ::SelectObject(dc, _font);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);
        *outPinyinH = static_cast<int>(tm.tmHeight);
    }
    if (_failed || _pending > 0) {
        HGDIOBJ old = ::SelectObject(dc, _smallFont);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);
        *outBadgeH = static_cast<int>(tm.tmHeight);
    }
}

void DSInputBoxWnd::_Repaint() {
    PAINTSTRUCT ps;
    HDC dc = ::BeginPaint(_hwnd, &ps);
    if (dc == nullptr) return;

    RECT rc;
    ::GetClientRect(_hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    if (w <= 0 || h <= 0) {
        ::EndPaint(_hwnd, &ps);
        return;
    }

    // Double-buffered: the box is repainted on every keystroke, and painting
    // straight to the screen flickers visibly at that rate.
    HDC mem = ::CreateCompatibleDC(dc);
    HBITMAP bmp = ::CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    HBRUSH bg = ::CreateSolidBrush(kBg);
    HPEN border = ::CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldBrush = ::SelectObject(mem, bg);
    HGDIOBJ oldPen = ::SelectObject(mem, border);

    const int radius = Scaled(kRadius, _dpi);
    ::RoundRect(mem, 0, 0, w, h, radius, radius);

    ::SelectObject(mem, oldBrush);
    ::SelectObject(mem, oldPen);
    ::DeleteObject(bg);
    ::DeleteObject(border);

    ::SetBkMode(mem, TRANSPARENT);

    const int padX = Scaled(kPadX, _dpi);
    const int padY = Scaled(kPadY, _dpi);

    // Each line's rect stops at that line's own measured height, which is the
    // same number _MeasureContent reserved for it. Letting it run to the bottom
    // of the box instead is the tempting version — DT_VCENTER centres within
    // whatever it is handed, so it looks fine with one line in the box — but it
    // centres the pinyin against the *whole box*, i.e. below the room the layout
    // set aside for it, and once the pending badge is there too it centres
    // straight on top of it.
    int pinyinH = 0;
    int badgeH = 0;
    _LineHeights(mem, &pinyinH, &badgeH);

    int y = padY;
    if (!_pinyin.empty()) {
        HGDIOBJ oldFont = ::SelectObject(mem, _font);
        ::SetTextColor(mem, kText);
        RECT line = {padX, y, w - padX, y + pinyinH};
        // DT_NOPREFIX matters: '&' is a legal character in the buffer and would
        // otherwise be eaten as an accelerator marker.
        ::DrawTextW(mem, _pinyin.c_str(), static_cast<int>(_pinyin.size()), &line,
                    DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
        ::SelectObject(mem, oldFont);
        y += pinyinH + Scaled(kBadgeGap, _dpi);
    }

    if (_failed || _pending > 0) {
        std::wstring badge;
        if (_failed) {
            badge = kFailedBadge;
        } else {
            wchar_t num[16] = {};
            ::wsprintfW(num, L"%u", _pending);
            badge = std::wstring(kPendingPrefix) + num;
        }
        HGDIOBJ oldFont = ::SelectObject(mem, _smallFont);
        ::SetTextColor(mem, _failed ? kBadgeBad : kBadge);
        RECT line = {padX, y, w - padX, y + badgeH};
        ::DrawTextW(mem, badge.c_str(), static_cast<int>(badge.size()), &line,
                    DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
        ::SelectObject(mem, oldFont);
    }

    ::BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(_hwnd, &ps);
}

// ---- layout ----------------------------------------------------------------

void DSInputBoxWnd::_MeasureContent(int dpi, int* outW, int* outH) {
    HDC dc = ::GetDC(_hwnd ? _hwnd : nullptr);
    const int padX = Scaled(kPadX, dpi);
    const int padY = Scaled(kPadY, dpi);

    int w = 0;
    int h = padY * 2;

    // Heights come from the same place _Repaint gets them, so the window is
    // exactly as tall as what gets drawn into it.
    int pinyinH = 0;
    int badgeH = 0;
    _LineHeights(dc, &pinyinH, &badgeH);

    if (!_pinyin.empty()) {
        HGDIOBJ old = ::SelectObject(dc, _font);
        SIZE sz = {};
        ::GetTextExtentPoint32W(dc, _pinyin.c_str(),
                                static_cast<int>(_pinyin.size()), &sz);
        ::SelectObject(dc, old);
        w = std::max(w, static_cast<int>(sz.cx));
        h += pinyinH;
    }

    if (_failed || _pending > 0) {
        // Measured generously: the real badge text is built in _Repaint. Its
        // widest form is bounded by the failure message, so measure that.
        HGDIOBJ old = ::SelectObject(dc, _smallFont);
        SIZE sz = {};
        ::GetTextExtentPoint32W(dc, kFailedBadge,
                                static_cast<int>(wcslen(kFailedBadge)), &sz);
        ::SelectObject(dc, old);
        w = std::max(w, static_cast<int>(sz.cx) + Scaled(24, dpi));  // room for digits
        h += Scaled(kBadgeGap, dpi) + badgeH;
    }

    ::ReleaseDC(_hwnd ? _hwnd : nullptr, dc);

    *outW = w + padX * 2;
    *outH = h;
}

void DSInputBoxWnd::_Relayout() {
    if (!_hwnd) return;

    const int dpi = DpiOf(_host ? _host : _hwnd);
    _EnsureFonts(dpi);

    int w = 0;
    int h = 0;
    _MeasureContent(dpi, &w, &h);

    const int maxW = Scaled(kMaxWidthDip, dpi);
    if (w > maxW) w = maxW;

    // Place under the caret, flipping above it when the monitor's work area has
    // no room below — the conventional IME behaviour near the bottom of a screen.
    const RECT caret = _caret;
    int x = caret.left;
    int y = caret.bottom + Scaled(kAnchorGap, dpi);

    HMONITOR mon = ::MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(mon, &mi)) {
        if (y + h > mi.rcWork.bottom) {
            y = caret.top - Scaled(kAnchorGap, dpi) - h;
        }
        if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    }

    // SWP_NOACTIVATE is the load-bearing flag here; without it the box takes
    // focus on show and the composition dies.
    ::SetWindowPos(_hwnd, HWND_TOPMOST, x, y, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    _visible = true;
    ::InvalidateRect(_hwnd, nullptr, FALSE);
    ::UpdateWindow(_hwnd);
}

// ---- window proc -----------------------------------------------------------

LRESULT CALLBACK DSInputBoxWnd::_WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    DSInputBoxWnd* self =
        reinterpret_cast<DSInputBoxWnd*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    return self->_Handle(msg, wParam, lParam);
}

LRESULT DSInputBoxWnd::_Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            _Repaint();
            return 0;
        case WM_ERASEBKGND:
            return 1;  // _Repaint covers every pixel
        case WM_NCHITTEST:
            // Be a hole in the hit-test map: a click aimed at the box reaches the
            // document under it, exactly as the user expects of an opaque
            // overlay that is not interactive.
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DPICHANGED:
            // Deliberately not handled: we inherit the host's DPI awareness and
            // cannot change it non-destructively. The next SetAnchor/_Relayout
            // re-reads the DPI anyway.
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(_hwnd, msg, wParam, lParam);
}

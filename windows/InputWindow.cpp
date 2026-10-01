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

const wchar_t kBoxClass[] = L"DSPinyinIMEBoxWnd";

// Metrics in DIPs; scaled by the host's DPI at layout time.
const int kPadX = 10;
const int kPadY = 6;
const int kRadius = 6;
const int kBadgeGap = 2;
const int kMaxWidthDip = 640;
const int kAnchorGap = 2;  // between the caret and the box

// Gap between two candidates on the candidate row.
const wchar_t kCandSep[] = L"  ";

const COLORREF kBg = RGB(255, 255, 255);
const COLORREF kBorder = RGB(184, 184, 184);
const COLORREF kText = RGB(28, 28, 28);
const COLORREF kBadge = RGB(120, 120, 120);
const COLORREF kBadgeBad = RGB(200, 40, 40);

// Spelled as \uXXXX escapes on purpose. The build passes no /utf-8 and the
// sources carry no BOM, so a literal here would be decoded with whatever ANSI
// code page the *build machine* happens to use — correct today only because this
// one is set to UTF-8 (65001), and mojibake on any other. Escapes do not care.
const wchar_t kPendingPrefix[] = L"\u5F85\u8F6C\u6362 ";  // 待转换
const wchar_t kFailedBadge[] = L"\u5DF2\u590D\u5236\u5230\u526A\u8D34\u677F";  // 已复制到剪贴板
const wchar_t kChineseBadge[] = L"\u4E2D\u6587";  // 中文
const wchar_t kEnglishBadge[] = L"\u82F1\u6587";  // 英文
const wchar_t kBadgeSep[] = L" \u00B7 ";  // " · "

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

DSPinyinIMEBoxWnd* DSPinyinIMEBoxWnd::Create(HINSTANCE hInst) {
    if (!_RegisterClass(hInst)) return nullptr;

    DSPinyinIMEBoxWnd* self = new (std::nothrow) DSPinyinIMEBoxWnd();
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

DSPinyinIMEBoxWnd::~DSPinyinIMEBoxWnd() {
    if (_font) ::DeleteObject(_font);
    if (_smallFont) ::DeleteObject(_smallFont);
}

void DSPinyinIMEBoxWnd::Destroy() {
    if (_hwnd) {
        ::SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    delete this;
}

BOOL DSPinyinIMEBoxWnd::_RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW existing = {};
    if (::GetClassInfoExW(hInst, kBoxClass, &existing)) return TRUE;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &DSPinyinIMEBoxWnd::_WndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBoxClass;
    if (::RegisterClassExW(&wc)) return TRUE;
    // A concurrent activation may have registered it first; that is success.
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// ---- content and placement -------------------------------------------------

void DSPinyinIMEBoxWnd::SetHost(HWND host) {
    if (_host == host) return;
    _host = host;
    if (_hwnd && host) {
        ::SetWindowLongPtrW(_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(host));
    }
}

void DSPinyinIMEBoxWnd::SetAnchor(const RECT& caret) {
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

void DSPinyinIMEBoxWnd::SetContent(const std::wstring& pinyin, unsigned pending, bool failed,
                                   const std::vector<std::wstring>& candidates) {
    if (_pinyin == pinyin && _pending == pending && _failed == failed &&
        _cands == candidates) {
        return;
    }
    _pinyin = pinyin;
    _pending = pending;
    _failed = failed;
    _cands = candidates;
    _ShowOrHide();
}

void DSPinyinIMEBoxWnd::SetMode(bool english, bool flash) {
    if (_english == english && _flash == flash) return;
    _english = english;
    _flash = flash;
    _ShowOrHide();
}

void DSPinyinIMEBoxWnd::_ShowOrHide() {
    // Nothing being typed, nothing outstanding, nothing to announce: there is no
    // box to show.
    //
    // Two states are exceptions, and both have to be tested here because each is
    // the one case where the status line is the entire message:
    //   * `_failed` — the clipboard fallback. Hiding would swallow the only
    //     signal the user gets that a sentence did not reach the document.
    //   * `_flash`  — Ctrl+Space's mode notice, and Ctrl+Space is most often
    //     pressed with nothing on screen at all, which is the whole reason it
    //     announces itself. The caller's timer takes it back down.
    //
    // The candidate row needs no case of its own: candidates only exist while
    // `_pinyin` is non-empty, so the test above already covers the situation
    // where there is something to draw.
    if (_pinyin.empty() && _pending == 0 && !_failed && !_flash) {
        Hide();
        return;
    }
    _Relayout();
}

// The status line under the pinyin.
//
// `failed` stands alone and wins. It is the only signal that text was lost, and
// it is sticky until the next successful write, so neither the mode marker nor a
// count may crowd it out.
std::wstring DSPinyinIMEBoxWnd::_StatusText() const {
    if (_failed) return std::wstring(kFailedBadge);

    // The mode is always there. Which language the next key produces is the one
    // thing about this box that has no other cue on screen.
    std::wstring text = _english ? kEnglishBadge : kChineseBadge;
    if (_pending > 0) {
        wchar_t num[16] = {};
        ::wsprintfW(num, L"%u", _pending);
        text += kBadgeSep;
        text += kPendingPrefix;
        text += num;
    }
    return text;
}

// The candidate row: "2你好 3你们 4首要".
//
// Built in one place because the same string decides the window's width and gets
// painted into it, exactly as _StatusText does for the line below.
//
// The label is the index plus two, never a stored string: that is the only way the
// number and the word cannot drift apart, and it is why the first candidate shows
// as "2" and there is no "1".
std::wstring DSPinyinIMEBoxWnd::_CandidateText() const {
    if (_cands.empty()) return std::wstring();
    std::wstring out;
    wchar_t label[8] = {};
    for (size_t i = 0; i < _cands.size(); ++i) {
        if (i > 0) out += kCandSep;
        ::wsprintfW(label, L"%u", static_cast<unsigned>(i) + 2);
        out += label;
        out += _cands[i];
    }
    return out;
}

void DSPinyinIMEBoxWnd::Hide() {
    if (!_hwnd) return;
    if (_visible) {
        ::ShowWindow(_hwnd, SW_HIDE);
        _visible = false;
    }
}

// ---- painting --------------------------------------------------------------

void DSPinyinIMEBoxWnd::_EnsureFonts(int dpi) {
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

void DSPinyinIMEBoxWnd::_LineHeights(HDC dc, int* outPinyinH, int* outCandH,
                                    int* outBadgeH) const {
    *outPinyinH = 0;
    *outCandH = 0;
    *outBadgeH = 0;

    if (!_pinyin.empty()) {
        HGDIOBJ old = ::SelectObject(dc, _font);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);
        *outPinyinH = static_cast<int>(tm.tmHeight);
    }
    // The candidate row gets the SAME face as the pre-edit line, and this is a
    // deliberate change of heart. It used to share the status line's small font,
    // on the grounds that a candidate is an annotation rather than the answer —
    // which is true of the *document* (nothing is written until Space) and false
    // of the *user* (these are the words they are about to press a digit for, and
    // they were too small to read). Two lines at one size also stops the box
    // reading as a main line with a footnote. Only the mode/pending line is small.
    if (!_cands.empty()) {
        HGDIOBJ old = ::SelectObject(dc, _font);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);
        *outCandH = static_cast<int>(tm.tmHeight);
    }
    // Unconditional, because the status line always carries the mode marker:
    // whenever the box is up, this line is on it. _MeasureContent and _Repaint
    // both take the height from here, which is what keeps the two from disagreeing
    // about how tall the box is.
    {
        HGDIOBJ old = ::SelectObject(dc, _smallFont);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc, &tm);
        ::SelectObject(dc, old);
        *outBadgeH = static_cast<int>(tm.tmHeight);
    }
}

void DSPinyinIMEBoxWnd::_Repaint() {
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
    int candH = 0;
    int badgeH = 0;
    _LineHeights(mem, &pinyinH, &candH, &badgeH);

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

    // The candidate row, in the same ink as the pre-edit above it: these are the
    // words a digit will commit, and the mode/pending line below is what is
    // secondary.
    if (!_cands.empty()) {
        const std::wstring cands = _CandidateText();
        HGDIOBJ oldFont = ::SelectObject(mem, _font);
        ::SetTextColor(mem, kText);
        RECT line = {padX, y, w - padX, y + candH};
        ::DrawTextW(mem, cands.c_str(), static_cast<int>(cands.size()), &line,
                    DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS);
        ::SelectObject(mem, oldFont);
        y += candH + Scaled(kBadgeGap, _dpi);
    }

    // Always drawn, and always at the full height _LineHeights reserved for it.
    // The mode marker lives here, so there is never a box without this line; the
    // red text is the only thing that varies.
    {
        const std::wstring badge = _StatusText();
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

void DSPinyinIMEBoxWnd::_MeasureContent(int dpi, int* outW, int* outH) {
    HDC dc = ::GetDC(_hwnd ? _hwnd : nullptr);
    const int padX = Scaled(kPadX, dpi);
    const int padY = Scaled(kPadY, dpi);

    int w = 0;
    int h = padY * 2;

    // Heights come from the same place _Repaint gets them, so the window is
    // exactly as tall as what gets drawn into it.
    int pinyinH = 0;
    int candH = 0;
    int badgeH = 0;
    _LineHeights(dc, &pinyinH, &candH, &badgeH);

    if (!_pinyin.empty()) {
        HGDIOBJ old = ::SelectObject(dc, _font);
        SIZE sz = {};
        ::GetTextExtentPoint32W(dc, _pinyin.c_str(),
                                static_cast<int>(_pinyin.size()), &sz);
        ::SelectObject(dc, old);
        w = std::max(w, static_cast<int>(sz.cx));
        h += pinyinH;
    }

    if (!_cands.empty()) {
        // Measured from the very string that gets painted, and capped at the box's
        // maximum width by the layout step — a long candidate row is ellipsised
        // rather than allowed to make the box span the screen.
        const std::wstring cands = _CandidateText();
        // The same face _Repaint paints this line with. Measuring in the small font
        // and drawing in the main one is how a wider line ends up ellipsised by the
        // box that was sized for the narrower reading of it.
        HGDIOBJ old = ::SelectObject(dc, _font);
        SIZE sz = {};
        ::GetTextExtentPoint32W(dc, cands.c_str(), static_cast<int>(cands.size()), &sz);
        ::SelectObject(dc, old);
        w = std::max(w, static_cast<int>(sz.cx));
        h += Scaled(kBadgeGap, _dpi) + candH;
    }

    {
        // Always present, since the status line carries the mode marker.
        //
        // Measured generously rather than from _StatusText(): the width would then
        // change with the pending count and the box would twitch as a digit went
        // from 9 to 10. The failure message is the widest thing this line ever
        // holds — the mode marker plus a count is shorter — so measuring that
        // bounds it, and DT_END_ELLIPSIS covers the rest.
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

void DSPinyinIMEBoxWnd::_Relayout() {
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

LRESULT CALLBACK DSPinyinIMEBoxWnd::_WndProc(HWND hWnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    DSPinyinIMEBoxWnd* self =
        reinterpret_cast<DSPinyinIMEBoxWnd*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    return self->_Handle(msg, wParam, lParam);
}

LRESULT DSPinyinIMEBoxWnd::_Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
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

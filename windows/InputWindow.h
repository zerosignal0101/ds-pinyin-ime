// InputWindow.h — the standalone input box the pinyin is drawn into.
//
// This is the 搜狗-style floating box: while you type, the pinyin lives HERE
// rather than in the document. That is the whole point of it. Writing a
// zero-width ITfComposition keeps TSF (and therefore the host) informed that a
// text edit is in progress, but nothing is ever inserted until a conversion
// lands — so a search box in Chrome does not run a query on every keystroke.
//
// Presentation only. It owns no state that the text service does not also hold:
// the caller pushes content in with SetContent and a caret rect with SetAnchor,
// and the window decides its own size, placement and visibility from those.
//
// Threading: created on and used only from the TSF STA thread.

#pragma once

#include <windows.h>
#include <string>

class DSInputBoxWnd {
public:
    // Create on the STA thread. Returns nullptr on failure (the IME then works
    // without a visible box rather than not at all).
    static DSInputBoxWnd* Create(HINSTANCE hInst);

    void Destroy();

    // The window the box belongs to (the host of the focused document). Drives
    // both ownership — so the box minimises with it and never floats over an
    // unrelated app — and DPI lookup. Safe to call repeatedly.
    void SetHost(HWND host);

    // Show what is being typed. `pinyin` is the raw ASCII buffer; `pending` is
    // how many conversions are still queued; `failed` marks a queued sentence
    // that could not be written to the document (its text is on the clipboard).
    // An empty `pinyin` with nothing pending hides the box.
    void SetContent(const std::wstring& pinyin, unsigned pending, bool failed);

    // Where the caret is, in SCREEN coordinates, as returned by
    // ITfContextView::GetTextExt. A degenerate rect means "position unknown":
    // the box then keeps its last position rather than jumping to a corner.
    void SetAnchor(const RECT& caret);

    void Hide();

private:
    DSInputBoxWnd() = default;
    ~DSInputBoxWnd();

    DSInputBoxWnd(const DSInputBoxWnd&) = delete;
    DSInputBoxWnd& operator=(const DSInputBoxWnd&) = delete;

    static BOOL _RegisterClass(HINSTANCE hInst);
    static LRESULT CALLBACK _WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT _Handle(UINT msg, WPARAM wParam, LPARAM lParam);

    void _EnsureFonts(int dpi);
    // Height of each line the box may show, in the fonts the painter uses.
    // Measured in one place because _MeasureContent sizes the window and
    // _Repaint draws into it, and any disagreement between them shows up as one
    // line sitting on top of another.
    void _LineHeights(HDC dc, int* outPinyinH, int* outBadgeH) const;
    void _Repaint();
    // Recompute size from the current content, place relative to the anchor, and
    // show/hide. The single place that touches SetWindowPos.
    void _Relayout();
    void _MeasureContent(int dpi, int* outW, int* outH);

    HWND _hwnd = nullptr;
    HWND _host = nullptr;
    HINSTANCE _hInst = nullptr;

    std::wstring _pinyin;
    unsigned _pending = 0;
    bool _failed = false;

    // Caret position in screen coordinates; empty (all zero) until the caller
    // supplies a plausible one.
    RECT _caret = {0, 0, 0, 0};

    bool _visible = false;
    int _dpi = 96;
    HFONT _font = nullptr;       // pinyin line
    HFONT _smallFont = nullptr;  // pending / error badge
};

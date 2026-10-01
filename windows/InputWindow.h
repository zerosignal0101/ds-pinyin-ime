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
#include <vector>

class DSPinyinIMEBoxWnd {
public:
    // Create on the STA thread. Returns nullptr on failure (the IME then works
    // without a visible box rather than not at all).
    static DSPinyinIMEBoxWnd* Create(HINSTANCE hInst);

    void Destroy();

    // The window the box belongs to (the host of the focused document). Drives
    // both ownership — so the box minimises with it and never floats over an
    // unrelated app — and DPI lookup. Safe to call repeatedly.
    void SetHost(HWND host);

    // Show what is being typed. `pinyin` is the raw ASCII buffer; `pending` is
    // how many conversions are still queued; `failed` marks a queued sentence
    // that could not be written to the document (its text is on the clipboard).
    // An empty `pinyin` with nothing pending hides the box.
    // `pinyin` is the whole pre-edit: chosen words (already Chinese) followed by
    // the pinyin still to be converted. `candidates` are the words offered for
    // the segment the user can act on; empty means there is no dictionary, no
    // match, or nothing left to choose, and the box is two lines as before.
    //
    // Candidate labels start at 2, so `candidates[0]` is drawn as "2". There is no
    // label 1 and the digit 1 does not select.
    void SetContent(const std::wstring& pinyin, unsigned pending, bool failed,
                    const std::vector<std::wstring>& candidates);

    // Which mode the text service is in, drawn as a 中/英 marker on the status
    // line. `flash` additionally holds the box on screen even with an empty
    // buffer and an empty queue — that is Ctrl+Space's only feedback when
    // nothing else is going on, and the caller's timer clears it. The marker is
    // drawn whenever the box is up; only `flash` keeps it there, which is what
    // stops English mode from parking a panel over the document indefinitely.
    void SetMode(bool english, bool flash);

    // Where the caret is, in SCREEN coordinates, as returned by
    // ITfContextView::GetTextExt. A degenerate rect means "position unknown":
    // the box then keeps its last position rather than jumping to a corner.
    void SetAnchor(const RECT& caret);

    void Hide();

private:
    DSPinyinIMEBoxWnd() = default;
    ~DSPinyinIMEBoxWnd();

    DSPinyinIMEBoxWnd(const DSPinyinIMEBoxWnd&) = delete;
    DSPinyinIMEBoxWnd& operator=(const DSPinyinIMEBoxWnd&) = delete;

    static BOOL _RegisterClass(HINSTANCE hInst);
    static LRESULT CALLBACK _WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT _Handle(UINT msg, WPARAM wParam, LPARAM lParam);

    void _EnsureFonts(int dpi);
    // Apply the show/hide rule to whatever the current content and mode are.
    // Shared by SetContent and SetMode so the decision genuinely exists once:
    // it is a four-term condition, and a second copy of it would be the place a
    // new state gets forgotten.
    void _ShowOrHide();
    // Height of each line the box may show, in the fonts the painter uses.
    // Measured in one place because _MeasureContent sizes the window and
    // _Repaint draws into it, and any disagreement between them shows up as one
    // line sitting on top of another.
    //
    // Three lines now, because the candidate row is between the pre-edit and the
    // status line — which is where a candidate window belongs, and where the eye
    // already is. `outCandH` is 0 when there is no candidate row, so the two-line
    // case is literally the same layout it always was.
    void _LineHeights(HDC dc, int* outPinyinH, int* outCandH, int* outBadgeH) const;
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

    // Candidate words for the segment the user can act on, in order. Empty means
    // no row. Held as UTF-16 because this whole class is wide-only — the UTF-8
    // -> UTF-16 conversion happens once, at SetContent, and nowhere below.
    //
    // Stored as words only (not "2你好") because the label is generated from the
    // index, so the two can never disagree about which number goes with which word.
    std::vector<std::wstring> _cands;

    // The mode marker, and whether it is currently holding the box open.
    bool _english = false;
    bool _flash = false;

    // The status line's text: the mode, plus the pending count when there is one,
    // or the failure notice instead of both. Built in one place because the same
    // string decides the window's height and gets painted into it.
    std::wstring _StatusText() const;
    // The candidate row's text, or empty when there is no row. Same reason as
    // _StatusText: the same string sizes the window and is painted into it.
    std::wstring _CandidateText() const;

    // Caret position in screen coordinates; empty (all zero) until the caller
    // supplies a plausible one.
    RECT _caret = {0, 0, 0, 0};

    bool _visible = false;
    int _dpi = 96;
    HFONT _font = nullptr;       // pinyin line
    HFONT _smallFont = nullptr;  // pending / error badge
};

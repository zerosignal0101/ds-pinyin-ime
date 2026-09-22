// EditSessions.cpp — ITfEditSession implementations.
//
// TSF forbids mutating a document directly. Instead you ask for an edit cookie
// by submitting an ITfEditSession to ITfContext::RequestEditSession; TSF calls
// your DoEditSession(ec) back with a cookie that authorizes reads/writes for the
// duration of that call.
//
// Four concrete sessions:
//   CStartCompositionEditSession — opens a ZERO-WIDTH ITfComposition at the
//                                  caret. It is never given text; the pre-edit
//                                  lives in our own floating box.
//   CEndCompositionEditSession   — writes optional final text and ends the
//                                  composition (the commit / cancel path).
//   CInsertTextEditSession       — writes one converted sentence at a captured
//                                  anchor (the queue's delivery step).
//   CCaretProbeEditSession       — read-only; where is the caret on screen?
//
// Every submitter checks BOTH the RequestEditSession return value and hrSession.
// They are different failures: the first means "the request could not be
// submitted at all", the second "the session ran and the document said no"
// (TS_E_READONLY, TF_E_DISCONNECTED, TF_E_LOCKED, …). Only the second tells you
// anything about the document, and the callers act on it — the insert path
// retries on TF_E_LOCKED and falls back to the clipboard otherwise.

#include "TextService.h"
#include "Globals.h"
#include "Trace.h"

#include <new>

// Helper: set the selection to a range (used to position the caret after edits).
static HRESULT SelectRange(TfEditCookie ec, ITfContext* pic, ITfRange* pRange) {
    TF_SELECTION sel;
    sel.range = pRange;
    sel.style.ase = TF_AE_NONE;
    sel.style.fInterimChar = FALSE;
    return pic->SetSelection(ec, 1, &sel);
}

// A range at the point where new text would go, or null.
//
// Two ways to ask, and the order matters. InsertTextAtSelection with
// TF_IAS_QUERYONLY asks the *host* where an insertion would land — that is the
// insertion point the host itself maintains, and it is what Weasel uses. The
// fallback (clone the selection, collapse it) lands in the same place whenever
// it works, but it is the second-hand answer and it was previously the only one
// this code had.
static ITfRange* CaretRange(ITfContext* pic, TfEditCookie ec) {
    ITfRange* pOut = nullptr;

    ITfInsertAtSelection* pInsertAt = nullptr;
    if (SUCCEEDED(pic->QueryInterface(IID_ITfInsertAtSelection,
                                      reinterpret_cast<void**>(&pInsertAt))) &&
        pInsertAt != nullptr) {
        pInsertAt->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &pOut);
        pInsertAt->Release();
    }
    if (pOut != nullptr) return pOut;

    TF_SELECTION sel;
    ULONG fetched = 0;
    if (SUCCEEDED(pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) &&
        fetched == 1) {
        sel.range->Clone(&pOut);
        sel.range->Release();
    }
    return pOut;
}

// Read a window of document text around `range` back out, and trace it.
//
// This is the one fact that separates the two explanations for "I typed and
// nothing appeared": either the edit never landed in the document, or it landed
// and the host did not repaint. Every edit session here returns S_OK in both
// cases, so the return codes cannot tell them apart — only reading the text
// back can.
static void TraceReadback(TfEditCookie ec, ITfRange* range, const wchar_t* what) {
    // Test first: the reads below are real work, and DsimeTrace can only skip
    // the write, not the work that produced its arguments.
    if (!DsimeTraceEnabled()) return;

    ITfRange* pRead = nullptr;
    if (FAILED(range->Clone(&pRead)) || !pRead) return;
    pRead->Collapse(ec, TF_ANCHOR_START);
    LONG shifted = 0;
    pRead->ShiftStart(ec, -12, &shifted, nullptr);  // a little context before
    pRead->ShiftEnd(ec, 48, &shifted, nullptr);     // and the insertion itself
    wchar_t buf[160] = {};
    ULONG got = 0;
    const HRESULT hr = pRead->GetText(ec, 0, buf, 159, &got);
    pRead->Release();
    DsimeTrace(L"  readback(%s): hr=%08X got=%lu text='%s'", what,
               static_cast<unsigned>(hr), got, buf);
}

// Submit `pES` and collapse the two failure modes into one HRESULT. On success
// returns whatever the session reported (S_OK, or TF_S_ASYNC if TSF deferred it
// anyway).
//
// Every edit session in this file goes through here, so this one line of trace
// answers the question that matters when text does not appear: did the session
// run, was it deferred, or did the document refuse it? `ret` is
// RequestEditSession's own return value and `hr` is hrSession — they disagree in
// exactly the cases that are hard to see, notably TF_E_SYNCHRONOUS.
static HRESULT SubmitSync(ITfContext* pic, TfClientId tid, ITfEditSession* pES,
                          DWORD flags, const wchar_t* what) {
    HRESULT hrSession = S_OK;
    HRESULT hr = pic->RequestEditSession(tid, pES, flags, &hrSession);
    pES->Release();
    DsimeTrace(L"session %-16s flags=%08X ret=%08X hr=%08X%s",
               what, static_cast<unsigned>(flags), static_cast<unsigned>(hr),
               static_cast<unsigned>(hrSession),
               (hrSession == TF_S_ASYNC) ? L"  DEFERRED" : L"");
    if (FAILED(hr)) return hr;
    return hrSession;
}

// ---------------------------------------------------------------------------
// CStartCompositionEditSession — open a zero-width composition at the caret
// ---------------------------------------------------------------------------

class CStartCompositionEditSession final : public ITfEditSession {
public:
    CStartCompositionEditSession(CTextService* pSvc, ITfContext* pic,
                                 ITfComposition** ppCompositionOut)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _ppCompositionOut(ppCompositionOut) {
        _pSvc->AddRef();
        _pic->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfContextComposition* pCtxComp = nullptr;
        if (FAILED(_pic->QueryInterface(IID_ITfContextComposition,
                                        reinterpret_cast<void**>(&pCtxComp))))
            return E_FAIL;

        HRESULT hr = E_FAIL;
        ITfRange* pRangeStart = CaretRange(_pic, ec);
        if (pRangeStart != nullptr) {
            // Collapse to the END with forward gravity. The composition is
            // zero-width and stays that way, so the only thing gravity decides
            // is which side of text inserted at this exact point the composition
            // ends up on — and we want it to follow the caret, not to be
            // stranded in front of what was just written.
            pRangeStart->Collapse(ec, TF_ANCHOR_END);
            pRangeStart->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_FORWARD);

            ITfComposition* pComposition = nullptr;
            hr = pCtxComp->StartComposition(
                ec, pRangeStart,
                static_cast<ITfCompositionSink*>(_pSvc),
                &pComposition);
            DsimeTrace(L"  startComposition: hr=%08X comp=%p",
                       static_cast<unsigned>(hr), static_cast<void*>(pComposition));
            if (SUCCEEDED(hr) && pComposition) {
                *_ppCompositionOut = pComposition;  // ownership to caller

                // Weasel puts the selection at the composition's end as part of
                // starting it; hosts that key their layout off the selection
                // rather than off the composition need that to agree.
                ITfRange* pSel = nullptr;
                if (SUCCEEDED(pRangeStart->Clone(&pSel))) {
                    pSel->Collapse(ec, TF_ANCHOR_END);
                    SelectRange(ec, _pic, pSel);
                    pSel->Release();
                }
            }
            pRangeStart->Release();
        }
        pCtxComp->Release();
        return hr;
    }

private:
    ~CStartCompositionEditSession() { _pic->Release(); _pSvc->Release(); }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition** _ppCompositionOut;
};

// ---------------------------------------------------------------------------
// CEndCompositionEditSession — write final text (optional) + end composition
// ---------------------------------------------------------------------------

class CEndCompositionEditSession final : public ITfEditSession {
public:
    CEndCompositionEditSession(CTextService* pSvc, ITfContext* pic,
                               ITfComposition* pComposition,
                               std::wstring finalText, BOOL hasFinal)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _pComposition(pComposition),
          _finalText(std::move(finalText)), _hasFinal(hasFinal) {
        _pSvc->AddRef();
        _pic->AddRef();
        _pComposition->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pRange = nullptr;
        if (FAILED(_pComposition->GetRange(&pRange)) || !pRange) {
            // Range gone; just end the composition.
            _pComposition->EndComposition(ec);
            return S_OK;
        }

        // The inserted text, when there is any; used below to park the caret.
        ITfRange* pInserted = nullptr;

        if (_hasFinal && !_finalText.empty()) {
            // InsertTextAtSelection, for the same reason as
            // CWriteCompositionEditSession — see the long note there. Writing the
            // committed text with ITfRange::SetText routes it through the host's
            // ITextStoreACP::SetText, which the contract says must *not* notify,
            // so a conforming host lays nothing out: the punctuation is in the
            // document and apparently swallowed, until the next click or key
            // re-lays-out around it. This is the path Enter and idle punctuation
            // take, which is why they kept the symptom after the queue was fixed.
            ITfInsertAtSelection* pInsert = nullptr;
            HRESULT hrIns = _pic->QueryInterface(IID_ITfInsertAtSelection,
                                                 reinterpret_cast<void**>(&pInsert));
            if (SUCCEEDED(hrIns) && pInsert != nullptr) {
                hrIns = pInsert->InsertTextAtSelection(
                    ec, 0, _finalText.c_str(),
                    static_cast<ULONG>(_finalText.length()), &pInserted);
                pInsert->Release();
            }
            if (FAILED(hrIns) || pInserted == nullptr) {
                if (pInserted != nullptr) {
                    pInserted->Release();
                    pInserted = nullptr;
                }
                DsimeTrace(L"  endComposition: InsertTextAtSelection failed "
                           L"(hr=%08X); using SetText", static_cast<unsigned>(hrIns));
                hrIns = pRange->SetText(ec, 0, _finalText.c_str(),
                                        static_cast<LONG>(_finalText.length()));
                pInserted = pRange;  // borrowed; see the release below
            }
            DsimeTrace(L"  endComposition: hr=%08X cch=%d text='%s'",
                       static_cast<unsigned>(hrIns),
                       static_cast<int>(_finalText.length()), _finalText.c_str());
            TraceReadback(ec, pInserted, L"endComposition");
        } else if (_hasFinal) {
            // Discard path. Clear the range rather than inserting nothing:
            // hasFinal must be TRUE even for empty text, since passing FALSE would
            // leave whatever is currently in the range sitting in the document.
            // The write is empty, so the non-notifying API costs nothing here.
            pRange->SetText(ec, 0, L"", 0);
        }

        // Move the caret past the committed text, then end the composition.
        ITfRange* pCaret = nullptr;
        ITfRange* pFrom = (pInserted != nullptr) ? pInserted : pRange;
        if (SUCCEEDED(pFrom->Clone(&pCaret))) {
            pCaret->Collapse(ec, TF_ANCHOR_END);
            SelectRange(ec, _pic, pCaret);
            pCaret->Release();
        }
        // pInserted is our own reference when InsertTextAtSelection succeeded, and
        // pRange borrowed when it fell back, so it is not released unconditionally.
        if (pInserted != nullptr && pInserted != pRange) pInserted->Release();
        pRange->Release();

        _pComposition->EndComposition(ec);
        return S_OK;
    }

private:
    ~CEndCompositionEditSession() {
        _pComposition->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition* _pComposition;
    std::wstring _finalText;
    BOOL _hasFinal;
};

// ---------------------------------------------------------------------------
// CInsertTextEditSession — deliver one converted sentence at its anchor
// ---------------------------------------------------------------------------

class CInsertTextEditSession final : public ITfEditSession {
public:
    CInsertTextEditSession(CTextService* pSvc, ITfContext* pic, ITfRange* anchor,
                           std::wstring text, BOOL moveCaret)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _anchor(anchor),
          _text(std::move(text)), _moveCaret(moveCaret) {
        _pSvc->AddRef();
        _pic->AddRef();
        _anchor->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pRange = nullptr;
        if (FAILED(_anchor->Clone(&pRange)) || !pRange) return E_FAIL;
        // The anchor is already a collapsed point with forward gravity; the
        // collapse is repeated here because TSF may have shifted the anchors
        // while the document was edited underneath us.
        pRange->Collapse(ec, TF_ANCHOR_END);
        pRange->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_FORWARD);

        // FALLBACK, used only when Dsime_RequestInsertText could not open a
        // composition. The normal path is the three-session one there; this
        // class does the same work in a single session, which a host is entitled
        // not to repaint (see below) and Notepad3 does not.
        //
        // The write still happens inside a composition of our own making, because
        // ITfRange::SetText on a range that is not a composition range is served
        // by the host's ITextStoreACP::SetText, and that method is documented to
        // *not* notify: "An application should not call the
        // ITextStoreACPSink::OnTextChange method in response to this method",
        // and "this method should be used only within an existing composition" —
        // with TSF wrapping a throwaway composition around the call when there
        // is none. The text lands in the store and the host is never told to
        // repaint it, so it appears only when something else forces a layout:
        // clicking, or losing focus. Chrome notifies anyway, which is exactly
        // why this looked like a host quirk with one application.
        //
        // A normal IME never hits this — it always commits into a composition.
        // So does Weasel, for every commit. Do the same: open a composition at
        // the anchor, write into that, end it.
        ITfContextComposition* pCtxComp = nullptr;
        ITfComposition* pComposition = nullptr;
        HRESULT hr = _pic->QueryInterface(IID_ITfContextComposition,
                                          reinterpret_cast<void**>(&pCtxComp));
        if (SUCCEEDED(hr) && pCtxComp != nullptr) {
            // The sink is REQUIRED — IContextComposition::StartComposition
            // documents pCompositionSink as "required and cannot be NULL", and
            // passing NULL fails with E_INVALIDARG. That failure is quiet in the
            // worst way: the caller falls back to the bare SetText, which is the
            // exact non-notifying path this composition exists to avoid, so the
            // text still lands and still never repaints. The service already
            // implements ITfCompositionSink, and it only acts on terminations of
            // the composition it deliberately tracks (_pComposition), so this
            // transient one cannot disturb a sentence being typed.
            hr = pCtxComp->StartComposition(
                ec, pRange, static_cast<ITfCompositionSink*>(_pSvc), &pComposition);
            pCtxComp->Release();
        }
        if (FAILED(hr) || pComposition == nullptr) {
            // Could not open one. Fall back to the bare write: on a host that
            // honours the SetText contract this will not repaint, but nothing
            // is lost and the trace says which path was taken.
            const unsigned hrStart = static_cast<unsigned>(hr);
            hr = pRange->SetText(ec, 0, _text.c_str(),
                                 static_cast<LONG>(_text.length()));
            DsimeTrace(L"  insert: NO COMPOSITION (start=%08X), bare setText hr=%08X",
                       hrStart, static_cast<unsigned>(hr));
            TraceReadback(ec, pRange, L"insert-bare");
            pRange->Release();
            return hr;
        }

        ITfRange* pCompRange = nullptr;
        hr = pComposition->GetRange(&pCompRange);
        if (SUCCEEDED(hr) && pCompRange != nullptr) {
            hr = pCompRange->SetText(ec, 0, _text.c_str(),
                                     static_cast<LONG>(_text.length()));
            DsimeTrace(L"  insert: setText hr=%08X cch=%d moveCaret=%d text='%s'",
                       static_cast<unsigned>(hr), static_cast<int>(_text.length()),
                       static_cast<int>(_moveCaret), _text.c_str());
            TraceReadback(ec, pCompRange, L"insert");
            if (SUCCEEDED(hr) && _moveCaret) {
                // Park the caret after the sentence we just wrote, so a user who
                // is still typing sees the result appear behind their cursor
                // rather than in front of it. Only the caller knows whether the
                // user is actually still typing — hence the flag.
                ITfRange* pCaret = nullptr;
                if (SUCCEEDED(pCompRange->Clone(&pCaret))) {
                    pCaret->Collapse(ec, TF_ANCHOR_END);
                    SelectRange(ec, _pic, pCaret);
                    pCaret->Release();
                }
            }
            pCompRange->Release();
        }
        pComposition->EndComposition(ec);
        pComposition->Release();
        pRange->Release();
        return hr;
    }

private:
    ~CInsertTextEditSession() {
        _anchor->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfRange* _anchor;
    std::wstring _text;
    BOOL _moveCaret;
};

// ---------------------------------------------------------------------------
// CCaretProbeEditSession — where is the caret, in screen coordinates?
// ---------------------------------------------------------------------------

// Where is the caret, in screen coordinates? `ec` is the read authority of the
// edit session we opened for the question.
static bool ProbeCaretRect(ITfContext* pic, TfEditCookie ec, RECT* pOut) {
    // CaretRange — the host's own insertion point — and NOT GetSelection.
    //
    // They are different questions, and a host is allowed to answer only the first
    // one correctly. A mouse click updates the insertion point that Scintilla
    // hands back to ITfInsertAtSelection, while the TSF *selection* stays wherever
    // the last programmatic edit left it; nothing tells us it changed. Probing the
    // selection therefore pinned the box to our own previous insertion until the
    // next keystroke started a composition, which re-anchors the selection — which
    // is why the box looked like it followed the caret only while typing, and
    // jumped correctly the instant a key was pressed.
    ITfRange* pProbe = CaretRange(pic, ec);
    if (pProbe == nullptr) return false;

    ITfContextView* pView = nullptr;
    if (FAILED(pic->GetActiveView(&pView)) || !pView) {
        pProbe->Release();
        return false;
    }
    pProbe->Collapse(ec, TF_ANCHOR_START);

    // A collapsed range usually reports a degenerate rect — TSF has no width and
    // no line box for a zero-length span. Widening by one character to the right
    // yields the real line height and baseline, which is what the box needs in
    // order to sit under the caret. At end-of-document there is nothing to widen
    // into, so the collapsed rect (likely thin) is taken as the fallback rather
    // than reporting no position at all.
    bool got = false;
    ITfRange* pWide = nullptr;
    if (SUCCEEDED(pProbe->Clone(&pWide))) {
        LONG shifted = 0;
        if (SUCCEEDED(pWide->ShiftEnd(ec, 1, &shifted, nullptr)) && shifted > 0) {
            RECT rc = {};
            BOOL clipped = FALSE;
            if (SUCCEEDED(pView->GetTextExt(ec, pWide, &rc, &clipped)) &&
                !IsDegenerate(rc)) {
                *pOut = rc;
                got = true;
            }
        }
        pWide->Release();
    }
    if (!got) {
        RECT rc = {};
        BOOL clipped = FALSE;
        // Clipping only means the caret is scrolled out of view; the rect is
        // still clamped into the view, which is a usable place to put the box. A
        // *degenerate* rect is the real "no position" signal.
        if (SUCCEEDED(pView->GetTextExt(ec, pProbe, &rc, &clipped)) &&
            !IsDegenerate(rc)) {
            *pOut = rc;
            got = true;
        }
    }
    DsimeTrace(L"  caretProbe: got=%d rect=%ld,%ld,%ld,%ld", static_cast<int>(got),
               pOut->left, pOut->top, pOut->right, pOut->bottom);
    pProbe->Release();
    pView->Release();
    return got;
}

class CCaretProbeEditSession final : public ITfEditSession {
public:
    CCaretProbeEditSession(ITfContext* pic, RECT* pOut)
        : _cRef(1), _pic(pic), _pOut(pOut) {
        _pic->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        return ProbeCaretRect(_pic, ec, _pOut) ? S_OK : E_FAIL;
    }

private:
    ~CCaretProbeEditSession() { _pic->Release(); }
    LONG _cRef;
    ITfContext* _pic;
    RECT* _pOut;  // borrowed; the caller waits for the synchronous session
};

// ---------------------------------------------------------------------------
// CCaptureAnchorEditSession — clone the caret as a durable insertion point
// ---------------------------------------------------------------------------

class CCaptureAnchorEditSession final : public ITfEditSession {
public:
    CCaptureAnchorEditSession(ITfContext* pic, ITfRange** ppOut)
        : _cRef(1), _pic(pic), _ppOut(ppOut) {
        _pic->AddRef();
        *_ppOut = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pAnchor = CaretRange(_pic, ec);
        if (pAnchor == nullptr) return E_FAIL;

        pAnchor->Collapse(ec, TF_ANCHOR_END);
        // Forward gravity is what makes queued sentences come out in the right
        // order. Typing never touches the document, so two Spaces in a row — the
        // user writing the next sentence while the first is still converting —
        // capture the *identical* position. With forward gravity an earlier job's
        // insertion pushes this anchor to the end of that text; with backward
        // gravity it would be stranded in front of it and the sentences would
        // come out reversed.
        pAnchor->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_FORWARD);

        *_ppOut = pAnchor;  // ownership to caller
        return S_OK;
    }

private:
    ~CCaptureAnchorEditSession() { _pic->Release(); }
    LONG _cRef;
    ITfContext* _pic;
    ITfRange** _ppOut;
};

// ---------------------------------------------------------------------------
// Submitters
// ---------------------------------------------------------------------------

HRESULT Dsime_RequestStartComposition(CTextService* pSvc, ITfContext* pic,
                                      TfClientId tid, ITfComposition** ppComp) {
    *ppComp = nullptr;
    CStartCompositionEditSession* pES =
        new (std::nothrow) CStartCompositionEditSession(pSvc, pic, ppComp);
    if (!pES) return E_OUTOFMEMORY;
    // Synchronous read/write session: edits happen inline before this returns.
    return SubmitSync(pic, tid, pES, TF_ES_SYNC | TF_ES_READWRITE, L"startComposition");
}

HRESULT Dsime_RequestEndComposition(CTextService* pSvc, ITfContext* pic,
                                    TfClientId tid, ITfComposition* pComp,
                                    const std::wstring& finalText, BOOL hasFinal) {
    if (!pComp) return S_OK;
    CEndCompositionEditSession* pES = new (std::nothrow)
        CEndCompositionEditSession(pSvc, pic, pComp, finalText, hasFinal);
    if (!pES) return E_OUTOFMEMORY;
    return SubmitSync(pic, tid, pES, TF_ES_SYNC | TF_ES_READWRITE, L"endComposition");
}

// ---- the queue's delivery, as three separate edit sessions -----------------
//
// Open a composition at the anchor, write into it, end it — each step its own
// RequestEditSession, which is how a normal IME does it and what Weasel does.
//
// Doing all three inside one session is what this used to do, and it is why a
// committed sentence did not appear in Notepad3 until the user clicked: the
// host is given no lock cycle between "the text changed" and "the composition
// ended" at which to re-lay-out and repaint. Chrome re-lays-out either way,
// which is what made this look host-specific rather than ours. The trace shows
// it plainly — OnLayoutChange follows the insert within milliseconds in Chrome
// and not at all in Notepad3.

class CComposeAtRangeEditSession final : public ITfEditSession {
public:
    CComposeAtRangeEditSession(CTextService* pSvc, ITfContext* pic, ITfRange* anchor,
                               ITfComposition** ppOut)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _anchor(anchor), _ppOut(ppOut) {
        _pSvc->AddRef();
        _pic->AddRef();
        _anchor->AddRef();
        *_ppOut = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfContextComposition* pCtxComp = nullptr;
        if (FAILED(_pic->QueryInterface(IID_ITfContextComposition,
                                        reinterpret_cast<void**>(&pCtxComp))) ||
            pCtxComp == nullptr) {
            return E_FAIL;
        }

        ITfRange* pRange = nullptr;
        HRESULT hr = _anchor->Clone(&pRange);
        if (SUCCEEDED(hr) && pRange != nullptr) {
            pRange->Collapse(ec, TF_ANCHOR_END);
            pRange->SetGravity(ec, TF_GRAVITY_FORWARD, TF_GRAVITY_FORWARD);
            ITfComposition* pComp = nullptr;
            // The sink is required and cannot be NULL; see CInsertTextEditSession.
            hr = pCtxComp->StartComposition(
                ec, pRange, static_cast<ITfCompositionSink*>(_pSvc), &pComp);
            if (SUCCEEDED(hr) && pComp != nullptr) *_ppOut = pComp;
            pRange->Release();
        }
        pCtxComp->Release();
        return hr;
    }

private:
    ~CComposeAtRangeEditSession() {
        _anchor->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfRange* _anchor;
    ITfComposition** _ppOut;
};

class CWriteCompositionEditSession final : public ITfEditSession {
public:
    CWriteCompositionEditSession(CTextService* pSvc, ITfContext* pic,
                                 ITfComposition* pComp, std::wstring text,
                                 BOOL moveCaret)
        : _cRef(1), _pSvc(pSvc), _pic(pic), _pComp(pComp),
          _text(std::move(text)), _moveCaret(moveCaret) {
        _pSvc->AddRef();
        _pic->AddRef();
        _pComp->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        ITfRange* pRange = nullptr;
        if (FAILED(_pComp->GetRange(&pRange)) || pRange == nullptr) return E_FAIL;

        // INSERT VIA InsertTextAtSelection, NOT SetText.
        //
        // This is the difference between the host being told and not being told.
        // MSDN's ITextStoreACP::SetText contract says an application "should not
        // call the ITextStoreACPSink::OnTextChange method in response to this
        // method", and that it "should call SetSelection, then
        // InsertTextAtSelection, to perform the actual change". So every
        // insertion this service has ever made has gone through the one API that
        // is specified *not* to notify.
        //
        // The symptoms of that are all one thing: the text is in the store and
        // the host does not re-lay-out or re-style it. The caret stays where it
        // was, so the next literal space lands in front of the sentence; the new
        // characters keep the old font, so Simplified Chinese renders with
        // Japanese glyph variants; and even spaces the *host itself* inserted sat
        // invisible until the next key. Every one of them snapped right the
        // moment the user pressed a key or clicked — because that is the host
        // processing input of its own, not because anything we did took effect.
        ITfInsertAtSelection* pInsert = nullptr;
        HRESULT hr = _pic->QueryInterface(IID_ITfInsertAtSelection,
                                          reinterpret_cast<void**>(&pInsert));
        ITfRange* pOut = nullptr;
        if (SUCCEEDED(hr) && pInsert != nullptr) {
            hr = pInsert->InsertTextAtSelection(
                ec, 0, _text.c_str(), static_cast<ULONG>(_text.length()), &pOut);
            pInsert->Release();
        }
        if (FAILED(hr) || pOut == nullptr) {
            // Host would not take it that way. Fall back to writing into the
            // composition range: worse (see above) but the text is not lost.
            DsimeTrace(L"  insert: InsertTextAtSelection hr=%08X; falling back to SetText",
                       static_cast<unsigned>(hr));
            if (pOut != nullptr) { pOut->Release(); pOut = nullptr; }
            hr = pRange->SetText(ec, 0, _text.c_str(),
                                 static_cast<LONG>(_text.length()));
            pOut = pRange;
            pRange->AddRef();
        }
        DsimeTrace(L"  insert: hr=%08X cch=%d moveCaret=%d text='%s'",
                   static_cast<unsigned>(hr), static_cast<int>(_text.length()),
                   static_cast<int>(_moveCaret), _text.c_str());
        TraceReadback(ec, pOut, L"insert");

        if (SUCCEEDED(hr) && _moveCaret) {
            ITfRange* pCaret = nullptr;
            if (SUCCEEDED(pOut->Clone(&pCaret))) {
                pCaret->Collapse(ec, TF_ANCHOR_END);
                SelectRange(ec, _pic, pCaret);
                pCaret->Release();
            }
        }
        pOut->Release();
        pRange->Release();
        return hr;
    }

private:
    ~CWriteCompositionEditSession() {
        _pComp->Release();
        _pic->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfContext* _pic;
    ITfComposition* _pComp;
    std::wstring _text;
    BOOL _moveCaret;
};

class CEndInsertCompositionEditSession final : public ITfEditSession {
public:
    CEndInsertCompositionEditSession(CTextService* pSvc, ITfComposition* pComp)
        : _cRef(1), _pSvc(pSvc), _pComp(pComp) {
        _pSvc->AddRef();
        _pComp->AddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&_cRef); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = ::InterlockedDecrement(&_cRef);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override {
        _pComp->EndComposition(ec);
        return S_OK;
    }

private:
    ~CEndInsertCompositionEditSession() {
        _pComp->Release();
        _pSvc->Release();
    }
    LONG _cRef;
    CTextService* _pSvc;
    ITfComposition* _pComp;
};

HRESULT Dsime_RequestInsertText(CTextService* pSvc, ITfContext* pic, TfClientId tid,
                                ITfRange* anchor, const std::wstring& text,
                                BOOL moveCaret) {
    if (!pic || !anchor) return E_INVALIDARG;

    // 1. Open a composition at the anchor. Without one the write below would be
    //    served by the host's ITextStoreACP::SetText, which is documented not to
    //    notify — see CInsertTextEditSession, which is the fallback.
    ITfComposition* pComp = nullptr;
    {
        CComposeAtRangeEditSession* pES = new (std::nothrow)
            CComposeAtRangeEditSession(pSvc, pic, anchor, &pComp);
        if (!pES) return E_OUTOFMEMORY;
        const HRESULT hr = SubmitSync(pic, tid, pES,
                                      TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                                      L"insert.compose");
        if (FAILED(hr) || pComp == nullptr) {
            if (pComp != nullptr) {
                pComp->Release();
                pComp = nullptr;
            }
            DsimeTrace(L"  insert: could not open a composition (hr=%08X); bare write",
                       static_cast<unsigned>(hr));
            CInsertTextEditSession* pBare = new (std::nothrow)
                CInsertTextEditSession(pSvc, pic, anchor, text, moveCaret);
            if (!pBare) return E_OUTOFMEMORY;
            return SubmitSync(pic, tid, pBare,
                              TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, L"insert.bare");
        }
    }

    // 2. Write the sentence into it.
    HRESULT hr = E_FAIL;
    {
        CWriteCompositionEditSession* pES = new (std::nothrow)
            CWriteCompositionEditSession(pSvc, pic, pComp, text, moveCaret);
        if (pES) {
            hr = SubmitSync(pic, tid, pES,
                            TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, L"insert.write");
        }
    }

    // 3. Close it.
    {
        CEndInsertCompositionEditSession* pES =
            new (std::nothrow) CEndInsertCompositionEditSession(pSvc, pComp);
        if (pES) {
            SubmitSync(pic, tid, pES,
                       TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, L"insert.end");
        }
    }
    pComp->Release();
    return hr;
}

bool Dsime_CaptureInsertAnchor(CTextService* pSvc, ITfContext* pic,
                               ITfRange** ppAnchor) {
    *ppAnchor = nullptr;
    if (!pic) return false;
    CCaptureAnchorEditSession* pES =
        new (std::nothrow) CCaptureAnchorEditSession(pic, ppAnchor);
    if (!pES) return false;
    HRESULT hr = SubmitSync(pic, pSvc->ClientId(), pES, TF_ES_SYNC | TF_ES_READ,
                            L"captureAnchor");
    if (FAILED(hr) || *ppAnchor == nullptr) {
        if (*ppAnchor) { (*ppAnchor)->Release(); *ppAnchor = nullptr; }
        return false;
    }
    return true;
}

bool Dsime_GetCaretPos(CTextService* pSvc, ITfContext* pic, HWND* pHost, RECT* pCaret) {
    RECT none = {0, 0, 0, 0};
    *pHost = nullptr;
    *pCaret = none;
    if (!pic) return false;

    // Rung 1: ask TSF. The only rung that knows about wrapping and scrolling.
    // Read-only, as the layout-sink contract requires of anything we run from
    // OnLayoutChange.
    CCaretProbeEditSession* pES = new (std::nothrow) CCaretProbeEditSession(pic, pCaret);
    if (pES) {
        HRESULT hr = SubmitSync(pic, pSvc->ClientId(), pES, TF_ES_SYNC | TF_ES_READ,
                                L"caretProbe");
        if (FAILED(hr)) *pCaret = none;
    }

    // The view's host window, whichever rung ends up supplying the position: it
    // owns the floating box (so the box minimises with the document) and is where
    // the DPI comes from.
    ITfContextView* pView = nullptr;
    if (SUCCEEDED(pic->GetActiveView(&pView)) && pView) {
        pView->GetWnd(pHost);
        if (!IsDegenerate(*pCaret)) {
            pView->Release();
            return true;
        }
    }

    // Rung 2: the classic caret. Scintilla, Edit and RichEdit controls publish
    // theirs here; TSF hosts generally do not.
    //
    // rcCaret is in the CLIENT coordinates of hwndCaret, and every other rung —
    // and everything downstream of this function, which places a window — works
    // in screen coordinates. Handing it back unconverted put the box wherever the
    // caret happened to be relative to the host's client area, i.e. near the top
    // left of the screen.
    GUITHREADINFO gti = {};
    gti.cbSize = sizeof(gti);
    if (::GetGUIThreadInfo(::GetCurrentThreadId(), &gti) &&
        (gti.flags & GUI_CARETBLINKING) != 0 && !IsDegenerate(gti.rcCaret)) {
        POINT pt = {gti.rcCaret.left, gti.rcCaret.top};
        if (gti.hwndCaret != nullptr && ::ClientToScreen(gti.hwndCaret, &pt)) {
            const int w = gti.rcCaret.right - gti.rcCaret.left;
            const int h = gti.rcCaret.bottom - gti.rcCaret.top;
            pCaret->left = pt.x;
            pCaret->top = pt.y;
            pCaret->right = pt.x + w;
            pCaret->bottom = pt.y + h;
            if (pView) pView->Release();
            return true;
        }
    }

    // Rung 3: the document's visible rectangle. Not the caret, but the box has to
    // appear somewhere, and the bottom-left of the view beats a screen corner.
    if (pView) {
        RECT ext = {};
        if (SUCCEEDED(pView->GetScreenExt(&ext)) && !IsDegenerate(ext)) {
            pCaret->left = ext.left + 8;
            pCaret->right = ext.left + 8;
            pCaret->top = ext.bottom - 24;
            pCaret->bottom = ext.bottom - 4;
            pView->Release();
            return true;
        }
        pView->Release();
    }
    return false;
}

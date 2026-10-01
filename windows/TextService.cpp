// TextService.cpp — lifecycle, IUnknown, sink wiring, and the marshaling window.
//
// See TextService.h for the threading and input design. This file owns:
//   * IUnknown reference counting and QueryInterface across all the interfaces
//     CTextService implements.
//   * Activate/ActivateEx/Deactivate: create the core engine+session, advise all
//     sinks, build the language bar, stand up the floating input box, and create
//     the hidden message-only window used for cross-thread marshaling.
//   * The focus sinks, which decide how much survives a window switch: the
//     typing session does not, the queue does.
//   * The hidden window proc that receives the terminal conversion results (and
//     the two timers).

#include "TextService.h"
#include "Globals.h"
#include "Guids.h"
#include "InputWindow.h"
#include "Trace.h"

#include <new>

// Class name for the hidden message-only window. Registered lazily.
static const wchar_t kMsgWndClass[] = L"DSPinyinIMEMsgWnd";

CTextService::CTextService() {
    DllAddRef();  // the module stays loaded while any object is alive
}

CTextService::~CTextService() {
    // By the time we're destroyed Deactivate should already have run, but be
    // defensive: never leave the core or windows dangling.
    _DestroyMessageWindow();
    DllRelease();
}

// ---- IUnknown --------------------------------------------------------------

STDMETHODIMP CTextService::QueryInterface(REFIID riid, void** ppvObj) {
    if (ppvObj == nullptr) return E_INVALIDARG;
    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor) ||
        IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
        *ppvObj = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
        *ppvObj = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfThreadFocusSink)) {
        *ppvObj = static_cast<ITfThreadFocusSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
        *ppvObj = static_cast<ITfKeyEventSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
        *ppvObj = static_cast<ITfCompositionSink*>(this);
    } else if (IsEqualIID(riid, IID_ITfTextLayoutSink)) {
        *ppvObj = static_cast<ITfTextLayoutSink*>(this);
    }

    if (*ppvObj) {
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CTextService::AddRef() {
    return ::InterlockedIncrement(&_cRef);
}

STDMETHODIMP_(ULONG) CTextService::Release() {
    LONG cr = ::InterlockedDecrement(&_cRef);
    if (cr == 0) {
        delete this;
    }
    return cr;
}

// ---- Activation ------------------------------------------------------------

STDMETHODIMP CTextService::Activate(ITfThreadMgr* ptim, TfClientId tid) {
    return ActivateEx(ptim, tid, 0);
}

STDMETHODIMP CTextService::ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD /*dwFlags*/) {
    _pThreadMgr = ptim;
    _pThreadMgr->AddRef();
    _tid = tid;

    // Which host are we inside? Every TSF behaviour in the trace below is the
    // host's, not ours — an edit that returns S_OK and shows nothing is a
    // statement about this process's text store.
    {
        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        DsimeTrace(L"=== activate: host=%s tid=%u", exe, static_cast<unsigned>(tid));
    }

    // 1) Stand up the core engine (per-user config) and one session. If the
    //    engine fails to init we still activate so the IME doesn't vanish from
    //    the language list; conversion simply yields errors and the raw pinyin
    //    is written instead. (Settings lets the user fix the config.)
    _engine.Create(nullptr);
    if (_engine.valid()) {
        _session.Create(_engine);
        _maxPending = _engine.QueueMaxPending();
    }

    // 1b) The dictionary for segmentation and candidate selection. Independent
    //     of the engine: a missing dsime.lex costs candidates, not conversion, so
    //     this must not be inside the `if (_engine.valid())` above. It only reads
    //     the filesystem, and is a no-op after the first activation in the process.
    _InitLexicon();

    // 2) Hidden message-only window must exist before any conversion can post
    //    back to us. Created on THIS (the STA) thread, so its window proc runs
    //    here.
    if (!_CreateMessageWindow()) {
        goto fail;
    }

    // 3) The floating input box. Non-fatal: without it the IME still converts,
    //    the user just cannot see what they are typing.
    _inputBox = DSPinyinIMEBoxWnd::Create(g_hInst);

    // 4) Follow the caret in hosts that tell us about layout changes. The timer
    //    is the fallback for the ones that do not (and for caret moves that
    //    change no text, which no layout sink reports).
    if (_msgWnd) {
        ::SetTimer(_msgWnd, DSIME_LAYOUT_TIMER_ID, DSIME_LAYOUT_TIMER_MS, nullptr);
    }

    // 5) Advise the sinks. Order doesn't matter much, but key-event last so we
    //    never receive a key before the rest of our state exists.
    if (!_InitThreadMgrEventSink()) goto fail;
    if (!_InitThreadFocusSink())    { /* non-fatal */ }
    if (!_InitKeyEventSink())       goto fail;

    // 6) Language-bar button that opens Settings. Non-fatal if it fails.
    _InitLanguageBar();

    return S_OK;

fail:
    Deactivate();
    return E_FAIL;
}

STDMETHODIMP CTextService::Deactivate() {
    // Logged unconditionally, because this firing is itself a diagnosis: the text
    // service is being switched out, and one of the things that can do that is
    // Windows' own "输入法/非输入法切换" hotkey — bound to Ctrl+Space by default.
    // A "deactivate" in the trace with no "test: space" line ahead of it is the
    // signature of Ctrl+Space never reaching us at all.
    DsimeTrace(L"=== deactivate (english=%d jobs=%u)", static_cast<int>(_englishMode),
               static_cast<unsigned>(_jobs.size()));

    // Tear down in reverse order of Activate. Each helper is idempotent.
    _UninitLanguageBar();
    _UninitKeyEventSink();
    _UninitThreadFocusSink();
    _UninitThreadMgrEventSink();
    _UnadviseLayoutSink();

    if (_msgWnd) {
        ::KillTimer(_msgWnd, DSIME_LAYOUT_TIMER_ID);
        ::KillTimer(_msgWnd, DSIME_PUMP_TIMER_ID);
        ::KillTimer(_msgWnd, DSIME_FLASH_TIMER_ID);
    }

    // Drop the queue by hand rather than relying on callbacks. The core
    // guarantees a terminal callback for every request it accepted, but a job
    // that was never dispatched has none coming — and every job holds COM
    // references that must not outlive us.
    _jobs.clear();
    _activeJobId = 0;

    if (_inputBox) {
        _inputBox->Destroy();
        _inputBox = nullptr;
    }

    // Drop any live composition reference without trying to mutate the doc (the
    // host may be tearing down). TSF will clean the range up.
    if (_pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }

    // Session first, then engine: freeing the engine cancels outstanding work
    // and waits (bounded) for its terminal callbacks to land, which is what lets
    // the drain in _DestroyMessageWindow actually find them.
    _session.reset();
    _engine.reset();
    _DestroyMessageWindow();

    _pinyin.clear();
    _composing = false;
    _threadFocused = true;
    _writeFailed = false;
    _maxPending = 0;
    // The mode belongs to the activation, so a re-activation starts in Chinese
    // rather than in whatever state the last one was left in.
    _englishMode = false;
    _modeFlash = false;
    _flushBufferAsLiteralDue = false;
    _lastToggleTick = 0;

    if (_pThreadMgr) {
        _pThreadMgr->Release();
        _pThreadMgr = nullptr;
    }
    _tid = TF_CLIENTID_NULL;
    return S_OK;
}

// ---- ITfThreadMgrEventSink -------------------------------------------------
// Focus changes pit the two halves of the state against each other: the typing
// session is nailed to a caret in a particular document and dies with it, while
// the queue is a promise to a document the user may have left.

STDMETHODIMP CTextService::OnInitDocumentMgr(ITfDocumentMgr*)   { return S_OK; }

STDMETHODIMP CTextService::OnUninitDocumentMgr(ITfDocumentMgr* pdim) {
    if (pdim == nullptr) return S_OK;

    // If the document going away is the one we were typing into, the context
    // holding our composition is gone with it.
    if (_pCompositionContext) {
        ITfDocumentMgr* owner = nullptr;
        if (SUCCEEDED(_pCompositionContext->GetDocumentMgr(&owner)) && owner) {
            const bool same = (owner == pdim);
            owner->Release();
            if (same) _AbandonTyping();
        }
    }
    _ReleaseJobsForDocument(pdim, nullptr);
    return S_OK;
}

STDMETHODIMP CTextService::OnSetFocus(ITfDocumentMgr* pdimFocus,
                                ITfDocumentMgr* /*pdimPrevFocus*/) {
    // The typing session does not survive a document switch: the caret, the
    // buffer and the zero-width composition all belong to the document being
    // left. The QUEUE does survive — a sentence already committed with Space is
    // still owed to the document it was typed into, which is the whole point of
    // capturing an insertion anchor per job.
    _AbandonTyping();

    // Track the caret in the new document. There is no thread-wide layout sink,
    // so this is per-context and has to be moved by hand.
    if (pdimFocus) {
        ITfContext* pic = nullptr;
        if (SUCCEEDED(pdimFocus->GetTop(&pic)) && pic) {
            _AdviseLayoutSink(pic);
            pic->Release();
        }
    } else {
        _UnadviseLayoutSink();
    }
    _UpdateInputBox(false);  // TSF callback: no synchronous session from here
    return S_OK;
}

STDMETHODIMP CTextService::OnPushContext(ITfContext*) { return S_OK; }

STDMETHODIMP CTextService::OnPopContext(ITfContext* pic) {
    // A context is going away. Queued sentences aimed at it can never be
    // delivered, and holding their ITfContext references past the teardown is
    // exactly how a dangling pointer gets made.
    _ReleaseJobsForDocument(nullptr, pic);
    if (pic != nullptr && pic == _pLayoutSinkContext) _UnadviseLayoutSink();
    return S_OK;
}

// ---- ITfThreadFocusSink ----------------------------------------------------

STDMETHODIMP CTextService::OnSetThreadFocus() {
    _threadFocused = true;
    // Coming back to our thread is the first moment a change made in Settings
    // can be noticed (the IME's engine holds its own copy of the config).
    if (_engine.valid()) {
        _engine.ReloadConfig();
        _maxPending = _engine.QueueMaxPending();
    }
    _UpdateInputBox(false);  // TSF callback: no synchronous session from here
    return S_OK;
}

STDMETHODIMP CTextService::OnKillThreadFocus() {
    // The whole thread lost focus (app switch). Drop what is being typed — the
    // caret belongs to an app the user has left, so the box would be stranded
    // over whatever they moved to — but leave the QUEUE alone. Cancelling it
    // here, as this used to, would mean every alt-tab silently kills the
    // sentences already committed with Space.
    _AbandonTyping();
    _threadFocused = false;
    _UpdateInputBox(false);  // hides; TSF callback either way
    return S_OK;
}

// ---- ITfCompositionSink ----------------------------------------------------

STDMETHODIMP CTextService::OnCompositionTerminated(TfEditCookie /*ecWrite*/,
                                             ITfComposition* pComposition) {
    DsimeTrace(L"onCompositionTerminated: comp=%p ours=%p suppress=%d",
               static_cast<void*>(pComposition), static_cast<void*>(_pComposition),
               static_cast<int>(_suppressTermination));
    if (_pComposition != pComposition) return S_OK;
    if (_suppressTermination) {
        // We are the ones ending it (_ReanchorIfComposing); the caller releases
        // the reference right after EndComposition returns. Releasing here too
        // would double-free it.
        return S_OK;
    }

    // TSF (or the app) ended our composition out from under us. Release our
    // reference and stop typing: without the composition there is nothing
    // anchoring the keystroke stream. The buffer goes too — the caret it
    // belonged to is gone, so there is nowhere to put a conversion of it.
    // _StartComposition simply runs again on the next key.
    _pComposition->Release();
    _pComposition = nullptr;
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    _pinyin.clear();
    _composing = false;
    _UpdateInputBox(false);  // TSF callback, mid-teardown: no session from here
    return S_OK;
}

// ---- ITfTextLayoutSink -----------------------------------------------------

STDMETHODIMP CTextService::OnLayoutChange(ITfContext* pic, TfLayoutCode lcode,
                                    ITfContextView* /*pView*/) {
    // Does the host notice our edits at all? A layout change fired right after a
    // SetText means the host saw the text change and chose not to show it; no
    // layout change at all means it never saw one.
    DsimeTrace(L"onLayoutChange: pic=%p code=%d ours=%d",
               static_cast<void*>(pic), static_cast<int>(lcode),
               static_cast<int>(pic == _pLayoutSinkContext));
    // The document reflowed under us: scrolled, rewrapped, resized.
    //
    // This runs with the document locked, and a synchronous edit session is not
    // safe from in here — the caret probe would at best fail and at worst
    // deadlock. So record that a reposition is due and hand it to our own
    // message loop, which runs the moment this callback returns and the lock is
    // released. Coalesced, because a scroll produces a burst of these.
    if (pic != nullptr && pic == _pLayoutSinkContext && _msgWnd != nullptr &&
        !_relocatePending) {
        _relocatePending = true;
        if (!::PostMessageW(_msgWnd, WM_DSIME_RELOCATE, 0, 0)) {
            _relocatePending = false;
        }
    }
    return S_OK;
}

// ---- sink advise/unadvise --------------------------------------------------

BOOL CTextService::_InitThreadMgrEventSink() {
    ITfSource* pSource = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))))
        return FALSE;
    HRESULT hr = pSource->AdviseSink(IID_ITfThreadMgrEventSink,
                                     static_cast<ITfThreadMgrEventSink*>(this),
                                     &_dwThreadMgrEventSinkCookie);
    pSource->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitThreadMgrEventSink() {
    if (_dwThreadMgrEventSinkCookie == TF_INVALID_COOKIE) return;
    ITfSource* pSource = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
        pSource->UnadviseSink(_dwThreadMgrEventSinkCookie);
        pSource->Release();
    }
    _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
}

BOOL CTextService::_InitThreadFocusSink() {
    ITfSource* pSource = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))))
        return FALSE;
    HRESULT hr = pSource->AdviseSink(IID_ITfThreadFocusSink,
                                     static_cast<ITfThreadFocusSink*>(this),
                                     &_dwThreadFocusSinkCookie);
    pSource->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitThreadFocusSink() {
    if (_dwThreadFocusSinkCookie == TF_INVALID_COOKIE) return;
    ITfSource* pSource = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
        pSource->UnadviseSink(_dwThreadFocusSinkCookie);
        pSource->Release();
    }
    _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;
}

void CTextService::_AdviseLayoutSink(ITfContext* pic) {
    _UnadviseLayoutSink();
    if (pic == nullptr) return;

    ITfSource* pSource = nullptr;
    if (FAILED(pic->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&pSource))))
        return;
    HRESULT hr = pSource->AdviseSink(IID_ITfTextLayoutSink,
                                     static_cast<ITfTextLayoutSink*>(this),
                                     &_dwLayoutSinkCookie);
    pSource->Release();
    if (SUCCEEDED(hr)) {
        _pLayoutSinkContext = pic;
        _pLayoutSinkContext->AddRef();
    }
}

void CTextService::_UnadviseLayoutSink() {
    if (_pLayoutSinkContext != nullptr && _dwLayoutSinkCookie != TF_INVALID_COOKIE) {
        ITfSource* pSource = nullptr;
        if (SUCCEEDED(_pLayoutSinkContext->QueryInterface(
                IID_ITfSource, reinterpret_cast<void**>(&pSource)))) {
            pSource->UnadviseSink(_dwLayoutSinkCookie);
            pSource->Release();
        }
    }
    _dwLayoutSinkCookie = TF_INVALID_COOKIE;
    if (_pLayoutSinkContext) {
        _pLayoutSinkContext->Release();
        _pLayoutSinkContext = nullptr;
    }
}

BOOL CTextService::_InitKeyEventSink() {
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (FAILED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                           reinterpret_cast<void**>(&pKeyMgr))))
        return FALSE;
    HRESULT hr = pKeyMgr->AdviseKeyEventSink(_tid,
                                             static_cast<ITfKeyEventSink*>(this),
                                             TRUE /*fForeground*/);
    pKeyMgr->Release();
    return SUCCEEDED(hr);
}

void CTextService::_UninitKeyEventSink() {
    ITfKeystrokeMgr* pKeyMgr = nullptr;
    if (SUCCEEDED(_pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                              reinterpret_cast<void**>(&pKeyMgr)))) {
        pKeyMgr->UnadviseKeyEventSink(_tid);
        pKeyMgr->Release();
    }
}

// ---- hidden marshaling window ----------------------------------------------

BOOL CTextService::_CreateMessageWindow() {
    // Register the window class once per process. GetClassInfo tells us if it's
    // already there (a second activation on the same thread, or a prior one).
    WNDCLASSEXW wc = {};
    if (!::GetClassInfoExW(g_hInst, kMsgWndClass, &wc)) {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CTextService::_MsgWndProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = kMsgWndClass;
        if (!::RegisterClassExW(&wc)) {
            // class may have been registered concurrently; tolerate that.
            if (::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return FALSE;
        }
    }

    // HWND_MESSAGE => a message-only window: never visible, no z-order, just a
    // thread-affine message sink. Perfect for marshaling.
    _msgWnd = ::CreateWindowExW(0, kMsgWndClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, g_hInst, nullptr);
    if (_msgWnd == nullptr) return FALSE;

    // Stash `this` so the static window proc can reach the instance.
    ::SetWindowLongPtrW(_msgWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return TRUE;
}

void CTextService::_DestroyMessageWindow() {
    if (_msgWnd == nullptr) return;

    // Drain whatever the worker thread posted but we never got to process.
    // Every such message owns a reference taken when its request was issued;
    // dropping the window with them still queued would leak CTextService, which
    // in turn keeps the DLL loaded for the life of the host process.
    MSG msg;
    while (::PeekMessageW(&msg, _msgWnd, WM_DSIME_CONVERT_RESULT,
                          WM_DSIME_CONVERT_RESULT, PM_REMOVE)) {
        ConvertResult* r = reinterpret_cast<ConvertResult*>(msg.lParam);
        if (r) {
            if (r->pThis) r->pThis->Release();
            delete r;
        }
    }

    ::SetWindowLongPtrW(_msgWnd, GWLP_USERDATA, 0);
    ::DestroyWindow(_msgWnd);
    _msgWnd = nullptr;
}

LRESULT CALLBACK CTextService::_MsgWndProc(HWND hWnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam) {
    CTextService* self =
        reinterpret_cast<CTextService*>(::GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    if (msg == WM_DSIME_CONVERT_RESULT) {
        // lParam owns a heap ConvertResult* posted from the core worker thread.
        // We are now on the STA thread, so it is safe to touch the composition.
        ConvertResult* r = reinterpret_cast<ConvertResult*>(lParam);
        if (r) {
            if (self) {
                self->_OnConvertResultOnStaThread(r->request_id, r->status, r->text);
            }
            // We hold a ref that was taken when the request was issued; release
            // it now that the round trip is complete.
            if (r->pThis) r->pThis->Release();
            delete r;
        }
        return 0;
    }

    if (msg == WM_DSIME_RELOCATE) {
        if (self == nullptr) return 0;
        self->_relocatePending = false;
        self->_UpdateInputBox();
        return 0;
    }

    if (msg == WM_DSIME_PUMP) {
        // A literal insertion deferred out of a key event — see WM_DSIME_PUMP.
        if (self == nullptr) return 0;
        self->_PumpQueue();
        return 0;
    }

    if (msg == WM_TIMER) {
        if (self == nullptr) return 0;
        if (wParam == DSIME_LAYOUT_TIMER_ID) {
            // Cheap when idle: _UpdateInputBox returns before the caret probe
            // unless there is something to draw.
            self->_UpdateInputBox();
            return 0;
        }
        if (wParam == DSIME_PUMP_TIMER_ID) {
            self->_PumpQueue();
            return 0;
        }
        if (wParam == DSIME_FLASH_TIMER_ID) {
            // The Ctrl+Space notice has had its moment. Clear the flag and let
            // the normal rule decide whether the box stays up — it will, if there
            // is pinyin or a pending count behind the notice.
            self->_modeFlash = false;
            ::KillTimer(hWnd, DSIME_FLASH_TIMER_ID);
            self->_UpdateInputBox();
            return 0;
        }
        return 0;
    }

    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

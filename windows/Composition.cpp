// Composition.cpp — composition orchestration and conversion plumbing.
//
// This file ties the STA-thread composition state in CTextService to the edit
// sessions (EditSessions.cpp) and to the dsime core's async conversion.
//
// Flow recap (all on the STA thread except where noted):
//   _StartComposition       -> opens the ITfComposition (first pinyin key).
//   _UpdateCompositionText   -> rewrites the pre-edit (raw buffer or converted).
//   _CommitComposition       -> writes final text and ends composition.
//   _EndComposition          -> clears the pre-edit and ends composition, so
//       nothing at all is written (the Esc path).
//   _FireConversion          -> STA thread: Space pressed. Snapshot the buffer,
//       AddRef self, and ds_session_convert_stream. This is the ONLY thing that
//       ever calls the provider — there is no keystroke timer.
//   _StreamCallbackThunk     -> CORE WORKER THREAD: package each update and
//       PostMessage WM_DSIME_CONVERT_PARTIAL (cumulative preview) or
//       WM_DSIME_CONVERT_RESULT (terminal) back to the STA window.
//   _OnConvertResultOnStaThread -> STA thread: drop stale ids, preview partials,
//       and on the terminal result write the sentence if Space asked for it.

#include "TextService.h"
#include "Globals.h"

// Prototypes for the edit-session submitters defined in EditSessions.cpp.
HRESULT Dsime_RequestStartComposition(CTextService* pSvc, ITfContext* pic,
                                      TfClientId tid, ITfComposition** ppComp);
HRESULT Dsime_RequestSetText(CTextService* pSvc, ITfContext* pic, TfClientId tid,
                             ITfComposition* pComp, TfGuidAtom gaAttr,
                             const std::wstring& text, BOOL underline);
HRESULT Dsime_RequestEndComposition(CTextService* pSvc, ITfContext* pic,
                                    TfClientId tid, ITfComposition* pComp,
                                    const std::wstring& finalText, BOOL hasFinal);

// CTextService needs the display-attribute atom from a private member; expose a
// tiny accessor via friend-free indirection by reading it through a getter we
// add inline here. To keep the header lean we instead pass it through the
// methods below using a helper that reads the member. Since these methods are
// CTextService members, they can read _gaDisplayAttribute directly.

// ---- composition orchestration --------------------------------------------

HRESULT CTextService::_StartComposition(ITfContext* pic) {
    if (_HasComposition()) return S_OK;  // already composing

    ITfComposition* pComp = nullptr;
    HRESULT hr = Dsime_RequestStartComposition(this, pic, _tid, &pComp);
    if (FAILED(hr) || !pComp) return FAILED(hr) ? hr : E_FAIL;

    // Keep the composition and its owning context alive for the whole session.
    _pComposition = pComp;            // ownership transferred from edit session
    _pCompositionContext = pic;
    _pCompositionContext->AddRef();
    return S_OK;
}

HRESULT CTextService::_UpdateCompositionText(ITfContext* pic,
                                             const std::wstring& text,
                                             BOOL underline) {
    if (!_HasComposition()) return E_UNEXPECTED;
    return Dsime_RequestSetText(this, pic, _tid, _pComposition,
                                _gaDisplayAttribute, text, underline);
}

HRESULT CTextService::_CommitComposition(ITfContext* pic,
                                         const std::wstring& text) {
    if (!_HasComposition()) return S_OK;
    HRESULT hr = Dsime_RequestEndComposition(this, pic, _tid, _pComposition,
                                             text, TRUE /*hasFinal*/);
    // Release our hold on the composition/context; it is finished.
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    return hr;
}

HRESULT CTextService::_EndComposition(ITfContext* pic) {
    if (!_HasComposition()) return S_OK;
    // Discard: write empty replacement text so the pre-edit disappears from the
    // document and nothing is inserted in its place (the Esc path). hasFinal
    // must be TRUE for that empty text to be applied — FALSE would leave
    // whatever is currently in the range sitting there.
    HRESULT hr = Dsime_RequestEndComposition(this, pic, _tid, _pComposition,
                                             std::wstring(), TRUE /*hasFinal*/);
    if (_pComposition) { _pComposition->Release(); _pComposition = nullptr; }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    return hr;
}

void CTextService::_ResetBuffer() {
    _pinyin.clear();
    _displayText.clear();
    _showingConverted = false;
    _lastRequestId = 0;
    _commitOnResult = false;
    _session.Reset();  // clears core buffer + cancels in-flight
}

// ---- conversion: issue request (STA thread) --------------------------------

bool CTextService::_FireConversion() {
    // STA thread. Only convert if we still have a non-empty buffer and an active
    // composition (the user may have committed/cancelled in the meantime).
    if (!_HasComposition() || _pinyin.empty() || !_session.valid()) return false;

    // The core's set_input was already called on each keystroke; ensure the
    // latest buffer is what gets converted.
    _session.SetInput(_pinyin);

    // We hand a borrowed `this` to the worker thread via the callback's
    // user_data. To keep `this` alive until the TERMINAL result is delivered
    // (and its posted message processed), take one reference now; the terminal
    // (is_final=1) stream callback releases it. Partial updates do not.
    AddRef();

    // Stream the conversion so the pre-edit fills in as the answer arrives.
    // Honors the `stream` config flag: when false the core fires only the single
    // terminal call. Reuses the engine's pooled, keep-alive connection and the
    // provider's cached system-prompt prefix across sentences.
    uint64_t reqId = _session.ConvertStream(&CTextService::_StreamCallbackThunk, this);
    if (reqId == 0) {
        // Empty buffer per the core (shouldn't happen given the check above):
        // no callback will fire, so release the ref we just took.
        Release();
        return false;
    }
    _lastRequestId = reqId;
    return true;
}

void CTextService::_AbandonPendingConversion() {
    // The user is typing again, so a conversion Space set running must not land
    // on top of the new input. Cancelling still delivers a terminal
    // DS_ERR_CANCELLED, which the result handler treats as "keep the buffer".
    if (!_commitOnResult) return;
    _commitOnResult = false;
    _session.Cancel();
}

// ---- regeneration: ask for a DIFFERENT candidate (STA thread) --------------

void CTextService::_FireRegenerate() {
    // STA thread. Like _FireConversion, but asks the core for an alternative
    // conversion that excludes the candidates already shown. The result streams
    // back through the same thunk/handler and replaces the pre-edit.
    if (!_HasComposition() || _pinyin.empty() || !_session.valid()) return;

    _session.SetInput(_pinyin);
    AddRef();  // terminal (is_final=1) stream callback releases it
    uint64_t reqId = _session.Regenerate(&CTextService::_StreamCallbackThunk, this);
    if (reqId == 0) {
        Release();
        return;
    }
    _lastRequestId = reqId;
}

// ---- conversion: core callback (WORKER THREAD) -----------------------------

void CTextService::_ConvertCallbackThunk(void* user_data, uint64_t request_id,
                                         int32_t status, const char* text_utf8) {
    // CORE WORKER THREAD. Do the minimum: copy out the (worker-owned, transient)
    // text, package it, and PostMessage to the STA window. NOTHING here touches
    // TSF or the composition.
    CTextService* self = static_cast<CTextService*>(user_data);
    if (self == nullptr) return;

    ConvertResult* r = new (std::nothrow) ConvertResult();
    if (r == nullptr) {
        // Can't deliver; release the ref taken in _FireConversion to avoid leak.
        self->Release();
        return;
    }
    r->pThis = self;          // carries the ref we took in _FireConversion
    r->request_id = request_id;
    r->status = status;
    r->text = (status == DS_OK) ? dsime::Utf8ToUtf16(text_utf8) : std::wstring();

    // If the window is gone (deactivated mid-flight), we still must release the
    // ref. PostMessage fails -> clean up here.
    HWND wnd = self->_msgWnd;
    if (wnd == nullptr || !::PostMessageW(wnd, WM_DSIME_CONVERT_RESULT, 0,
                                          reinterpret_cast<LPARAM>(r))) {
        self->Release();
        delete r;
    }
    // On success, ownership of `r` (and the ref) passes to the window proc.
}

// ---- streaming conversion: core callback (WORKER THREAD) --------------------

void CTextService::_StreamCallbackThunk(void* user_data, uint64_t request_id,
                                        int32_t status, int32_t is_final,
                                        const char* text_utf8) {
    // CORE WORKER THREAD. Package the update and PostMessage it to the STA window.
    // PARTIALS (is_final==0) carry NO ref and post WM_DSIME_CONVERT_PARTIAL; only
    // the TERMINAL call (is_final==1) carries the per-request ref and posts
    // WM_DSIME_CONVERT_RESULT (whose handler releases it). Partials are always
    // queued before the terminal call from this one worker task, so FIFO delivery
    // keeps `this` alive while partials are processed.
    CTextService* self = static_cast<CTextService*>(user_data);
    if (self == nullptr) return;

    ConvertResult* r = new (std::nothrow) ConvertResult();
    if (r == nullptr) {
        // Can't deliver. Only the terminal call owns the ref, so release it then.
        if (is_final) self->Release();
        return;
    }
    r->pThis = is_final ? self : nullptr;   // ref ownership only on the terminal call
    r->request_id = request_id;
    r->status = status;
    r->text = (status == DS_OK) ? dsime::Utf8ToUtf16(text_utf8) : std::wstring();

    HWND wnd = self->_msgWnd;
    UINT msg = is_final ? WM_DSIME_CONVERT_RESULT : WM_DSIME_CONVERT_PARTIAL;
    if (wnd == nullptr ||
        !::PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(r))) {
        if (is_final) self->Release();
        delete r;
    }
    // On success, ownership of `r` (and, for the terminal call, the ref) passes
    // to the window proc.
}

// ---- conversion: result handling (STA thread) ------------------------------

void CTextService::_OnConvertResultOnStaThread(uint64_t request_id, int32_t status,
                                               const std::wstring& text,
                                               bool is_final) {
    // Drop stale results: a newer request (or a commit/reset that zeroed
    // _lastRequestId) has superseded this one.
    if (request_id != _lastRequestId) return;

    // If the composition ended meanwhile, ignore.
    if (!_HasComposition() || _pCompositionContext == nullptr) return;

    // A streamed PARTIAL: preview the sentence as it arrives. Only the terminal
    // call may write to the document, so this never commits.
    if (!is_final) {
        if (status == DS_OK && !text.empty()) {
            _displayText = text;
            _showingConverted = true;
            _UpdateCompositionText(_pCompositionContext, _displayText, TRUE);
        }
        return;
    }

    // Terminal. Space asked for convert-and-commit, so a success lands straight
    // in the document in one step.
    if (status == DS_OK && !text.empty() && _commitOnResult) {
        _CommitComposition(_pCompositionContext, text);
        _ResetBuffer();
        return;
    }

    // Failure, empty result, or a preview-only request: keep the raw buffer on
    // screen and drop the pending-commit flag, so the user can edit it or just
    // press Space again to retry. (Optionally surface ds_last_error in a future
    // status UI — today a failed conversion is indistinguishable from a slow
    // one.)
    _commitOnResult = false;
}

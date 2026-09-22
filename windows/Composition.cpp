// Composition.cpp — composition orchestration, the floating box, and the queue.
//
// This file ties the STA-thread composition state in CTextService to the edit
// sessions (EditSessions.cpp), the floating box (InputWindow.cpp), and the dsime
// core's async conversion.
//
// Flow recap (all on the STA thread except where noted):
//   _StartComposition    -> opens a zero-width ITfComposition (first pinyin key).
//                           Nothing is ever written into it.
//   _UpdateInputBox      -> repaints the floating box and follows the caret.
//   _CommitComposition   -> writes final text and ends the composition (Enter).
//   _EndComposition      -> ends the composition writing nothing (Space, Esc).
//   _ClearTyping         -> forget the buffer. Deliberately does NOT touch the
//                           core session, which may be busy with a queued job.
//   _PumpQueue           -> drive the head of the queue: convert it, or insert
//                           the result it already has, or drop it and move on.
//   _StartConversion     -> STA thread: hand one job to the core, non-streaming.
//   _ConvertCallbackThunk -> CORE WORKER THREAD: package the outcome and
//                           PostMessage it back to the STA window.
//   _OnConvertResultOnStaThread -> STA thread: mark the job converted and pump.
//
// Why non-streaming: the box shows a pending count, not a preview, so there is
// nothing to do with partial tokens — streaming would only buy SSE parsing, a
// PostMessage per token, and bandwidth. Going non-streaming also keeps the
// request shape identical across the queue, which is what the provider's prefix
// cache and the core's context bookkeeping both want.

#include "TextService.h"
#include "Globals.h"
#include "InputWindow.h"
#include "Trace.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <new>

// Prototypes for the edit-session submitters defined in EditSessions.cpp.
HRESULT Dsime_RequestStartComposition(CTextService* pSvc, ITfContext* pic,
                                      TfClientId tid, ITfComposition** ppComp);
HRESULT Dsime_RequestEndComposition(CTextService* pSvc, ITfContext* pic,
                                    TfClientId tid, ITfComposition* pComp,
                                    const std::wstring& finalText, BOOL hasFinal);

// ---- job lifetime ----------------------------------------------------------

PendingJob::~PendingJob() {
    if (anchor) anchor->Release();
    if (context) context->Release();
    if (docMgr) docMgr->Release();
}

// ---- composition orchestration --------------------------------------------

HRESULT CTextService::_StartComposition(ITfContext* pic) {
    if (_pComposition) return S_OK;  // already anchoring this typing session

    ITfComposition* pComp = nullptr;
    HRESULT hr = Dsime_RequestStartComposition(this, pic, _tid, &pComp);
    if (FAILED(hr) || !pComp) return FAILED(hr) ? hr : E_FAIL;

    // Keep the composition and its owning context alive for the whole session.
    _pComposition = pComp;            // ownership transferred from edit session
    _pCompositionContext = pic;
    _pCompositionContext->AddRef();
    return S_OK;
}

HRESULT CTextService::_CommitComposition(ITfContext* pic,
                                         const std::wstring& text) {
    if (!_pComposition) return S_OK;
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
    if (!_pComposition) return S_OK;
    // Discard: write empty replacement text so nothing is inserted. hasFinal
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

void CTextService::_AbandonTyping() {
    // Drop the typing session *without* an edit session.
    //
    // This runs from focus callbacks (OnSetFocus, OnKillThreadFocus), where
    // starting a synchronous session is not safe. The composition is zero-width
    // and holds no text, so there is nothing to clean out of the document — we
    // simply let go and TSF terminates the range itself, which is what Deactivate
    // has always done. The buffer has to go, though: its caret belongs to a
    // document the user has left.
    DsimeTrace(L"abandonTyping: dropping buffer of %u chars",
               static_cast<unsigned>(_pinyin.size()));
    if (_pComposition) {
        _pComposition->Release();
        _pComposition = nullptr;
    }
    if (_pCompositionContext) {
        _pCompositionContext->Release();
        _pCompositionContext = nullptr;
    }
    _pinyin.clear();
    _composing = false;
}

void CTextService::_ClearTyping() {
    // Only the frontend's typing state. In particular this must NOT call
    // _session.Reset(): the core session may be mid-conversion for a sentence
    // the user already committed with Space, and resetting cancels in-flight
    // work. The buffer the core converts is set per job, in _PumpQueue.
    _pinyin.clear();
    _composing = false;
    _writeFailed = false;
    _UpdateInputBox();
}

void CTextService::_ReanchorIfComposing(ITfContext* pic) {
    if (!_composing || !_pComposition || pic != _pCompositionContext) return;

    // A queued sentence just landed in this document, so the caret the
    // composition was holding has moved out from under it. The composition is
    // zero-width and holds no text, so the fix is to replace it rather than to
    // try to move it. Doing nothing here would leave the next Space capturing an
    // anchor in the wrong place, and a stray composition marker on screen.
    //
    // MUST be called from a key event (or a TSF callback): the end/start pair
    // below are synchronous edit sessions, and TSF refuses those anywhere else.
    // Its one caller is _HandleKey, via _reanchorDue.
    _suppressTermination = true;
    _EndComposition(pic);
    _suppressTermination = false;

    if (SUCCEEDED(_StartComposition(pic))) {
        _composing = true;
    } else {
        // The document stopped being writable mid-sentence. Stop eating keys so
        // they reach the host instead of disappearing into a buffer we can no
        // longer place.
        _composing = false;
        _pinyin.clear();
    }
    _UpdateInputBox();
}

// ---- the floating box ------------------------------------------------------

ITfContext* CTextService::_CaretContext() {
    // While typing, the box follows the caret of the document the composition is
    // anchored in. Once Space (or Enter, or Esc) has ended that composition the
    // box is still on screen — it is showing the pending count — and it has to
    // keep following the caret from there: an Enter that moves to the next line,
    // a scroll, a click somewhere else. That caret belongs to the focused
    // document, so that is what we probe.
    //
    // Without this the box froze wherever the last sentence was typed: there was
    // no composition context left to probe, so the caret probe was skipped
    // entirely and the box sat on the old line while the queue drained.
    if (_pCompositionContext) {
        _pCompositionContext->AddRef();
        return _pCompositionContext;
    }
    if (_pThreadMgr == nullptr) return nullptr;

    ITfDocumentMgr* pdim = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&pdim)) || pdim == nullptr) return nullptr;
    ITfContext* pic = nullptr;
    if (FAILED(pdim->GetTop(&pic))) pic = nullptr;  // GetTop AddRefs on success
    pdim->Release();
    return pic;
}

void CTextService::_UpdateInputBox(bool canProbeCaret) {
    if (_inputBox == nullptr) return;

    if (!_threadFocused) {
        // The caret belongs to an app the user has left; a box tracking it would
        // float over whatever they moved to.
        DsimeTrace(L"box: hide (no thread focus)");
        _inputBox->Hide();
        return;
    }
    if (_pinyin.empty() && _PendingConversions() == 0 && !_writeFailed) {
        // Idle. Skip the caret probe below — that is a synchronous edit session,
        // and the layout timer would otherwise run one six times a second for
        // nothing.
        DsimeTrace(L"box: hide (idle)");
        _inputBox->Hide();
        return;
    }
    DsimeTrace(L"box: show pinyin=%u pending=%u jobs=%u failed=%d composing=%d",
               static_cast<unsigned>(_pinyin.size()),
               static_cast<unsigned>(_PendingConversions()),
               static_cast<unsigned>(_jobs.size()),
               static_cast<int>(_writeFailed), static_cast<int>(_composing));

    // Anchor first: SetContent triggers a layout, and laying out against a stale
    // caret would visibly jump before correcting itself.
    if (canProbeCaret) {
        if (ITfContext* pProbe = _CaretContext()) {
            HWND host = nullptr;
            RECT caret = {};
            if (Dsime_GetCaretPos(this, pProbe, &host, &caret)) {
                if (host) _inputBox->SetHost(host);
                _inputBox->SetAnchor(caret);
            }
            pProbe->Release();
        }
    }

    _inputBox->SetContent(dsime::Utf8ToUtf16(_pinyin),
                          static_cast<unsigned>(_PendingConversions()), _writeFailed);
}

std::string CTextService::_ContextKeyForFocus() {
    if (!_pThreadMgr) return std::string();

    ITfDocumentMgr* pdim = nullptr;
    if (FAILED(_pThreadMgr->GetFocus(&pdim)) || !pdim) return std::string();

    HWND hwnd = nullptr;
    ITfContext* pic = nullptr;
    if (SUCCEEDED(pdim->GetTop(&pic)) && pic) {
        ITfContextView* pView = nullptr;
        if (SUCCEEDED(pic->GetActiveView(&pView)) && pView) {
            pView->GetWnd(&hwnd);
            pView->Release();
        }
        pic->Release();
    }
    pdim->Release();
    if (!hwnd) return std::string();

    // {exe}|{window class}. Deliberately application-level rather than per
    // window: the domain vocabulary the context buys us is a property of the
    // application, and keying on the title would restart from nothing on every
    // new browser tab.
    wchar_t exe[MAX_PATH] = {};
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc) {
            DWORD n = ARRAYSIZE(exe);
            if (!::QueryFullProcessImageNameW(hProc, 0, exe, &n)) exe[0] = L'\0';
            ::CloseHandle(hProc);
        }
    }
    std::wstring exeName(exe);
    const size_t slash = exeName.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeName = exeName.substr(slash + 1);

    wchar_t cls[256] = {};
    ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));

    std::string key = dsime::Utf16ToUtf8(exeName);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (key.empty()) key = "unknown";
    key += '|';
    key += dsime::Utf16ToUtf8(std::wstring(cls));
    return key;
}

// ---- the queue -------------------------------------------------------------

size_t CTextService::_PendingConversions() const {
    // What the box's 待转换 badge counts, and what "is there anything to show"
    // is asked of. Literal jobs (queued punctuation) are excluded: they were
    // never conversions, and counting them would report the user a model request
    // they did not make — the box would jump to 待转换 2 the moment a comma
    // followed a Space.
    size_t n = 0;
    for (const auto& job : _jobs) {
        if (!job->literal) ++n;
    }
    return n;
}

void CTextService::_PumpQueue() {
    // Iterative, not recursive: a queue whose jobs all fail instantly (no engine)
    // would otherwise drain at a recursion depth equal to its length, which is
    // not bounded by a config field — only by how fast the user can press Space.
    for (;;) {
        if (_jobs.empty()) {
            if (_msgWnd) ::KillTimer(_msgWnd, DSIME_PUMP_TIMER_ID);
            _UpdateInputBox();
            return;
        }

        PendingJob* job = _jobs.front().get();

        if (!job->converted) {
            if (_activeJobId != 0) return;  // its conversion is already in flight

            if (_session.valid() && _StartConversion(job)) {
                _UpdateInputBox();
                return;  // the terminal callback resumes the pump
            }
            // Nothing was dispatched — no engine (bad config, missing API key),
            // or the core refused the buffer. Fall through with no result, which
            // _FinishJob degrades to writing the raw pinyin.
            job->converted = true;
        }

        if (!_FinishJob(job)) {
            // TF_E_LOCKED: a retry timer is armed and the job stays at the head.
            _UpdateInputBox();
            return;
        }
        _jobs.pop_front();
    }
}

bool CTextService::_StartConversion(PendingJob* job) {
    // The core reads the buffer and the context key when the request is issued,
    // so setting them here — and only here — is what lets one session serve
    // every queued sentence, in order.
    _session.SetInput(job->pinyin);
    _session.SetContextKey(job->contextKey);

    // We hand a borrowed `this` to the worker thread via the callback's
    // user_data, so take a reference now; the terminal callback releases it.
    AddRef();
    uint64_t id = _session.Convert(&CTextService::_ConvertCallbackThunk, this);
    if (id == 0) {
        // The core saw an empty buffer and will never call back, so the ref we
        // just took has to come back now. The caller then finishes the job with
        // no result.
        Release();
        return false;
    }
    job->requestId = id;
    _activeJobId = id;
    // %hs, not %s: in a wide printf %s wants a wchar_t*, and the buffer is UTF-8.
    DsimeTrace(L"queue: dispatched id=%llu pinyin='%hs'", id, job->pinyin.c_str());
    return true;
}

bool CTextService::_FinishJob(PendingJob* job) {
    if (job->cancelled) {
        // The core dropped this deliberately (teardown). Nothing to write, and
        // nowhere useful to write it.
        return true;
    }

    // Writing the raw pinyin when there is no Chinese is the same escape hatch
    // Enter offers, and it keeps a provider outage from costing the user a
    // sentence. It is also self-announcing: what lands is visibly pinyin.
    std::wstring text = job->converted && !job->result.empty()
                            ? job->result
                            : dsime::Utf8ToUtf16(job->pinyin);
    if (text.empty()) return true;

    // Move the caret past the sentence just written. Unconditionally, which is
    // what Weasel does and the only version of this that has ever been observed
    // to work here.
    //
    // Two earlier attempts at being clever both failed. `_composing && …` is
    // false in the common case — the user has pressed Space and is waiting — so
    // the caret stayed where it was, which left it *in front of* the sentence
    // that had just appeared, and the next thing typed (a literal space, say)
    // landed in front of it. Testing "is this document still focused" instead
    // looked right but the comparison silently failed in Notepad3, so it behaved
    // exactly like the version it replaced.
    //
    // Setting the selection on a document that is no longer focused is harmless:
    // it moves that document's caret, which is where the sentence just went, and
    // does not steal the user's focus.
    const BOOL moveCaret = TRUE;

    // Writing a queued sentence opens a composition of its own at the anchor, and
    // TSF allows only ONE composition per context — so starting ours terminates
    // the zero-width one the user is typing into. That arrives as
    // OnCompositionTerminated, where it is indistinguishable from the app yanking
    // the composition away, and that handler throws the in-progress buffer out.
    // The result was that a sentence landing while the user was already typing the
    // next one silently ate whatever they had typed since: the box emptied itself
    // for no reason the user could see.
    //
    // The composition really is gone either way, so there is nothing to save —
    // only the buffer, which never depended on it (the insertion anchor is
    // captured fresh, at Space). Suppressing the termination keeps the buffer and
    // leaves _reanchorDue to rebuild the composition on the next keystroke.
    _suppressTermination = true;
    HRESULT hr = Dsime_RequestInsertText(this, job->context, _tid, job->anchor, text,
                                         moveCaret);
    _suppressTermination = false;

    DsimeTrace(L"queue: finishJob hr=%08X moveCaret=%d len=%u retries=%d",
               static_cast<unsigned>(hr), static_cast<int>(moveCaret),
               static_cast<unsigned>(text.size()), job->insertRetries);

    // Unconditionally, and before the failure branches: the _attempt_ is what
    // kills a live composition, whether or not the write then succeeded.
    // _ReanchorIfComposing is a no-op when nothing is being typed.
    _reanchorDue = true;

    if (hr == TF_E_LOCKED && job->insertRetries < DSIME_MAX_INSERT_RETRIES) {
        // The document is mid-edit. Wait rather than losing the sentence.
        ++job->insertRetries;
        if (_msgWnd) ::SetTimer(_msgWnd, DSIME_PUMP_TIMER_ID, DSIME_PUMP_TIMER_MS, nullptr);
        return false;
    }
    if (FAILED(hr)) {
        _LoseText(text, hr);
        return true;
    }
    _writeFailed = false;
    return true;
}

void CTextService::_LoseText(const std::wstring& text, HRESULT hr) {
    // The one case where the queue genuinely cannot deliver: the document is
    // gone, read-only, or went away while we were waiting. Put the sentence
    // where the user can retrieve it and say so in the box, rather than dropping
    // it silently.
    //
    // Record the HRESULT first. By the time this runs the user has nothing to go
    // on but a red badge, and the difference between TF_E_SYNCHRONOUS (the edit
    // session was refused), TS_E_READONLY and TF_E_DISCONNECTED is the whole
    // diagnosis. Appended to only on failure, so the file stays empty in normal
    // use and is safe to leave in a shipping build.
    {
        wchar_t path[MAX_PATH] = {};
        DWORD n = ::GetTempPathW(ARRAYSIZE(path), path);
        if (n > 0 && n < ARRAYSIZE(path) - 20) {
            ::wcscat_s(path, L"dsinput-error.log");
            FILE* f = nullptr;
            if (::_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f != nullptr) {
                ::fwprintf(f, L"insert failed: hr=0x%08X len=%u\n",
                           static_cast<unsigned>(hr),
                           static_cast<unsigned>(text.size()));
                ::fclose(f);
            }
        }
    }

    if (!text.empty()) {
        // The clipboard is a shared resource and another app may hold it open;
        // retry briefly. This runs from a posted message, not a key event, so a
        // few milliseconds here cannot be felt.
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (::OpenClipboard(_msgWnd)) {
                ::EmptyClipboard();
                const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h) {
                    void* p = ::GlobalLock(h);
                    if (p) {
                        ::memcpy(p, text.c_str(), bytes);
                        ::GlobalUnlock(h);
                        if (!::SetClipboardData(CF_UNICODETEXT, h)) ::GlobalFree(h);
                    } else {
                        ::GlobalFree(h);
                    }
                }
                ::CloseClipboard();
                break;
            }
            ::Sleep(2);
        }
    }
    _writeFailed = true;
    // The pump repaints once it has finished with this job.
}

void CTextService::_ReleaseJobsForDocument(ITfDocumentMgr* pdim, ITfContext* pic) {
    if (pdim == nullptr && pic == nullptr) return;

    bool droppedActive = false;
    for (auto it = _jobs.begin(); it != _jobs.end();) {
        const bool matches = (pic != nullptr) ? ((*it)->context == pic)
                                              : ((*it)->docMgr == pdim);
        if (matches) {
            if ((*it)->requestId != 0 && (*it)->requestId == _activeJobId) {
                droppedActive = true;
            }
            it = _jobs.erase(it);
        } else {
            ++it;
        }
    }
    if (!droppedActive) return;

    // Its terminal callback is still owed — the core guarantees one — but there
    // is no longer a job to hand it to, so forget the id and let the callback be
    // ignored. Cancelling makes it arrive promptly instead of after the HTTP
    // timeout.
    _activeJobId = 0;
    _session.Cancel();
    _PumpQueue();
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
        // Can't deliver; release the ref taken in _StartConversion to avoid a leak.
        self->Release();
        return;
    }
    r->pThis = self;          // carries the ref we took in _StartConversion
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

// ---- conversion: result handling (STA thread) ------------------------------

void CTextService::_OnConvertResultOnStaThread(uint64_t request_id, int32_t status,
                                               const std::wstring& text) {
    DsimeTrace(L"queue: result id=%llu status=%d len=%u activeId=%llu", request_id,
               status, static_cast<unsigned>(text.size()), _activeJobId);
    if (_activeJobId == 0 || request_id != _activeJobId) return;  // stale
    _activeJobId = 0;

    auto it = std::find_if(
        _jobs.begin(), _jobs.end(),
        [request_id](const std::unique_ptr<PendingJob>& j) {
            return j->requestId == request_id;
        });
    if (it == _jobs.end()) {
        // The job was dropped (its document went away) while the request was in
        // flight. Its ref is balanced by the window proc; just carry on.
        _PumpQueue();
        return;
    }

    PendingJob* job = it->get();
    job->converted = true;
    if (status == DS_OK) {
        job->result = text;
    } else if (status == DS_ERR_CANCELLED) {
        job->cancelled = true;
    }
    // Any other status leaves `result` empty, which _FinishJob turns into the
    // raw-pinyin fallback.
    _PumpQueue();
}

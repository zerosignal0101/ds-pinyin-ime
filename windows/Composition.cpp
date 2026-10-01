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
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <new>

// ---- the failure log -------------------------------------------------------

// One line to %TEMP%\dspinyinime-error.log. The file is empty unless something went
// wrong, so it is safe to leave in a shipping build and it is the first thing to
// ask a user for — it is also the only record that survives a conversion that
// never produced text, which otherwise leaves no evidence at all.
//
// Two classes of failure land here and each line names its own, so the file
// reads unambiguously. Deliberately no document text: it lives on disk and
// outlives the document it described — the same reason the conversation context
// is memory-only. Lengths and status codes are enough to tell the cases apart
// (`DS_ERR_*` in `core/include/dsime.h`).
static void AppendErrorLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    DWORD n = ::GetTempPathW(ARRAYSIZE(path), path);
    if (n == 0 || n >= ARRAYSIZE(path) - 20) return;
    ::wcscat_s(path, L"dspinyinime-error.log");

    FILE* f = nullptr;
    if (::_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || f == nullptr) return;
    va_list args;
    va_start(args, fmt);
    ::vfwprintf(f, fmt, args);
    va_end(args);
    ::fclose(f);
}

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
    // Chosen words go with the buffer: they are annotations ON the pinyin, not
    // text in the document, so nothing about them survives a commit or an Esc.
    _chosen.clear();
    _Resegment();
    _composing = false;
    _writeFailed = false;
    _UpdateInputBox();
}

// ---- word selection ---------------------------------------------------------

void CTextService::_InitLexicon() {
    // Once per process, before the first query. The core refuses a second call
    // rather than pretending to accept it, so a re-activation must not make one.
    static bool s_tried = false;
    if (s_tried) return;
    s_tried = true;

    // Beside this DLL. Not the host's directory: we are loaded *into* the host,
    // so current_exe would name notepad.exe, and not the working directory
    // either. regsvr32 recorded the exact path this DLL was registered from, and
    // dsime.lex installs next to it, so this is the one location guaranteed to
    // be the right one.
    wchar_t self[MAX_PATH] = {};
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&DsimeTrace), &mod) ||
        ::GetModuleFileNameW(mod, self, MAX_PATH) == 0) {
        DsimeTrace(L"lexicon: cannot locate our own module; running without candidates");
        return;
    }
    std::wstring dir(self);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash + 1);
    const std::wstring lex = dir + L"dsime.lex";

    if (!dsime::LexiconSetPath(dsime::Utf16ToUtf8(lex))) {
        DsimeTrace(L"lexicon: set_path refused (already mapped?): %s",
                   dsime::Utf16ToUtf8(dsime::LastError()).c_str());
        return;
    }
    DsimeTrace(L"lexicon: available=%d path=%s", dsime::LexiconAvailable() ? 1 : 0,
               dsime::Utf16ToUtf8(lex).c_str());
}

void CTextService::_Resegment() {
    _cands.clear();
    _activeStart = 0;
    _activeLen = 0;
    if (_pinyin.empty()) {
        _segs.Free();
        return;
    }
    if (!_segs.Segment(_pinyin)) return;

    // The actionable segment is the FIRST one: the traditional behaviour, where
    // picking a word advances the cursor to the next. Only that one gets a
    // candidate list — the rest would be fetched, formatted and never drawn.
    for (int32_t i = 0; i < _segs.count(); ++i) {
        if (!_segs.HasWord(i)) continue;  // digits, capitals, unknown names
        const int32_t n = _segs.CandCount(i);
        if (n <= 0) continue;
        // A span in the buffer, NOT the code. "mei you" is 8 bytes and the
        // letters the user typed are 6, so sizing the deletion off the code
        // silently refuses every multi-syllable word — which is nearly all of
        // them. Single-syllable words only ever worked because their code
        // happens to equal the input.
        const int32_t start = _segs.Start(i);
        const int32_t end = _segs.End(i);
        if (start < 0 || end <= start || static_cast<size_t>(end) > _pinyin.size()) continue;
        // Anything the core could not parse ahead of the active segment — an
        // unmatched run, a comma the user typed mid-sentence — is consumed along
        // with it, because the deletion is from the front and a hole in the
        // middle would leave the segmenter looking at what is no longer a word
        // boundary. "Consumed" is not "dropped": _SelectCandidate carries that run
        // into the chosen word's text so it keeps its place in the payload.
        _activeStart = static_cast<size_t>(start);
        _activeLen = static_cast<size_t>(end);
        _cands.reserve(static_cast<size_t>(n));
        for (int32_t c = 0; c < n; ++c) {
            _cands.push_back(_segs.Cand(i, c));
        }
        break;
    }
}

std::string CTextService::_ConversionPayload() const {
    // Direct concatenation, no separator. The chosen words are Chinese and the
    // tail is pinyin, and the system prompt's mixed-input rule is what tells the
    // model to keep the Chinese verbatim and convert the rest.
    std::string out;
    for (const ChosenWord& w : _chosen) out += w.text;
    out += _pinyin;
    return out;
}

bool CTextService::_SelectCandidate(size_t n) {
    if (n >= _cands.size() || _activeLen == 0) return false;
    if (_activeLen > _pinyin.size()) return false;
    ChosenWord w;
    // The exact bytes consumed, so Backspace restores the buffer verbatim. Storing
    // the code here instead would splice a spaced "mei you" into the middle of
    // the next segment's letters and the whole thing would re-segment as garbage.
    w.pinyin = _pinyin.substr(0, _activeLen);
    // Whatever sits in FRONT of the active segment is not part of the word, and it
    // cannot simply be dropped either: the payload is the chosen words concatenated
    // in selection order, so a leading run that is discarded here comes back out
    // in the wrong place. Type `nihao,shijie`, choose 你好, then choose 世界 — the
    // comma is now the first thing in the buffer, and the active segment starts at
    // offset 1, so a choice that took only the word produced "你好世界" and the
    // comma was gone. Carrying the run as part of this word's text puts it back
    // exactly where it was typed: "你好,世界".
    w.text = _pinyin.substr(0, _activeStart) + _cands[n];
    _chosen.push_back(w);
    _pinyin.erase(0, _activeLen);
    _Resegment();
    return true;
}

bool CTextService::_UndoLastChoice() {
    if (_chosen.empty()) return false;
    // Backspace on a chosen word reverts the choice rather than deleting a
    // character: nothing was written to the document, so there is no character to
    // delete — the word is an annotation on the buffer, and putting its pinyin
    // back is the only undo that matches what the user sees.
    const ChosenWord w = _chosen.back();
    _chosen.pop_back();
    _pinyin += w.pinyin;
    _Resegment();
    return true;
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
    // `_modeFlash` is the exception: Ctrl+Space asked for the box to show the new
    // mode even though there is nothing else to put in it. Mirrors the same test
    // in DSPinyinIMEBoxWnd::_ShowOrHide — the two are separate copies on purpose (the
    // box is presentation and this is the state), but they must agree, and a
    // state added to one without the other shows up as a box that will not go
    // away or a flash that never appears.
    if (_pinyin.empty() && _chosen.empty() && _PendingConversions() == 0 && !_writeFailed &&
        !_modeFlash) {
        // Idle. Skip the caret probe below — that is a synchronous edit session,
        // and the layout timer would otherwise run one six times a second for
        // nothing.
        DsimeTrace(L"box: hide (idle)");
        _inputBox->Hide();
        return;
    }
    // The pre-edit line shows the WHOLE buffer, not just the unselected tail: a
    // chosen word is already Chinese and is part of what Space will send, so
    // leaving it out would make the box misreport the payload. This is also why
    // the idle test above has to consider `_chosen` — a buffer of nothing but
    // chosen words is not idle, it is waiting for a Space that costs no call.
    // The line itself is _PreEditText() rather than the payload: same words, but
    // the pinyin half broken into syllables.
    std::wstring shown = dsime::Utf8ToUtf16(_PreEditText());
    DsimeTrace(L"box: show pinyin=%u chosen=%zu cands=%u pending=%u jobs=%u failed=%d "
               L"composing=%d english=%d flash=%d",
               static_cast<unsigned>(_pinyin.size()), _chosen.size(),
               static_cast<unsigned>(_cands.size()),
               static_cast<unsigned>(_PendingConversions()),
               static_cast<unsigned>(_jobs.size()),
               static_cast<int>(_writeFailed), static_cast<int>(_composing),
               static_cast<int>(_englishMode), static_cast<int>(_modeFlash));

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

    // Mode before content: SetContent can hide the box, and SetMode can bring it
    // back, so doing it the other way round would flash a stale frame.
    _inputBox->SetMode(_englishMode, _modeFlash);
    // UTF-8 -> UTF-16 once, here: the box is wide-only and never sees the
    // multibyte form, so the candidate list cannot be half-converted anywhere.
    std::vector<std::wstring> candText;
    candText.reserve(_cands.size());
    for (const std::string& c : _cands) candText.push_back(dsime::Utf8ToUtf16(c));
    _inputBox->SetContent(shown, static_cast<unsigned>(_PendingConversions()), _writeFailed,
                          candText);
}

std::string CTextService::_PreEditText() const {
    // DISPLAY ONLY. Space still sends _ConversionPayload() — the words the user
    // chose and the remaining pinyin concatenated with nothing between them,
    // because that is what the system prompt's mixed-input rule describes.
    //
    // What goes on screen is a different string for one reason: "nihaoshijie" is
    // not something a person can read, and the whole basis for pressing a digit
    // is a guess about where the syllables are. The candidate list is the
    // segmentation's opinion, and this line is the same opinion drawn — the one
    // place the user can see "it read that as ni hao shi jie, so it is offering
    // 你好" and decide whether to accept it. Separators therefore come from the
    // segments the core just returned, never from re-deriving syllables here.
    std::string out;
    for (const ChosenWord& w : _chosen) out += w.text;
    if (_pinyin.empty()) return out;
    if (!out.empty()) out += ' ';
    // No segmentation (no dictionary, or a buffer changed without a resegment):
    // show the letters. A word per segment would be better than this, but
    // inventing a reading the core never produced is worse than none.
    if (!_segs.valid() || _segs.count() <= 0) return out + _pinyin;
    for (int32_t i = 0; i < _segs.count(); ++i) {
        if (i > 0) out += ' ';
        out += _segs.Pinyin(i);
    }
    return out;
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

    // {exe}|{pid}.
    //
    // Per *process*, not per window and not per window class. Keying on the class
    // was the bug: every Notepad3 window shares one, so opening a second file in
    // a second Notepad3 taught the model the first file's vocabulary and billed
    // the user for it on every sentence. The pid separates the instances, which
    // is the closest a text service can get to "which document" without reading
    // the title — and the title would restart from nothing on every browser tab.
    //
    // Note the granularity it does NOT give: one process is one context, so
    // Electron and VS Code still share across their windows. That is accepted.
    wchar_t exe[MAX_PATH] = {};
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return std::string();  // no window, no identity: no context
    {
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

    // From here on the run is never computed. The engine holds one context store
    // per process already, so it cannot collide across processes; the pid is in
    // the key to keep it readable in traces and to keep an exe name we could not
    // read ("unknown") from merging two unrelated programs.
    std::string key = dsime::Utf16ToUtf8(exeName);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (key.empty()) key = "unknown";
    key += '|';
    key += std::to_string(pid);
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
            // Nothing was dispatched, so there is no request and no callback to
            // report one. Fall through with no result, which _FinishJob degrades
            // to writing the raw pinyin.
            //
            // This is the one silent failure the conversion callback cannot
            // cover, so it is logged here: a session only fails to exist when
            // the engine could not be built at all (an unreadable or malformed
            // config.json), which otherwise shows up as pinyin appearing on the
            // screen and no explanation anywhere.
            AppendErrorLog(L"conversion not attempted: engine unavailable len=%u\n",
                           static_cast<unsigned>(job->pinyin.size()));
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

    // A refused session (TF_E_LOCKED: the document is mid-edit) and a queued one
    // (TF_S_ASYNC: TSF took it but has not run it) mean the same thing to us —
    // nothing has been written yet. Wait and retry rather than accepting the
    // result.
    //
    // TF_S_ASYNC is a *success* code, so accepting it here is the quiet failure:
    // FAILED() is false, the job is popped, and the sentence is gone with no
    // error and no badge. That is why it is listed explicitly.
    const bool retryable = (hr == TF_E_LOCKED || hr == TF_S_ASYNC);
    if (retryable && job->insertRetries < DSIME_MAX_INSERT_RETRIES) {
        ++job->insertRetries;
        if (_msgWnd) ::SetTimer(_msgWnd, DSIME_PUMP_TIMER_ID, DSIME_PUMP_TIMER_MS, nullptr);
        return false;
    }
    if (FAILED(hr) || retryable) {
        // Out of retries with nothing written, or the document refused. Same
        // treatment either way: the sentence goes to the clipboard and the box
        // says so, because dropping it silently is the one outcome the queue
        // exists to prevent. (The retryable codes are not FAILED, so without the
        // second test this case fell straight through to "success".)
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
    // diagnosis.
    AppendErrorLog(L"insert failed: hr=0x%08X len=%u\n", static_cast<unsigned>(hr),
                   static_cast<unsigned>(text.size()));

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
    } else {
        // The conversion failed, so _FinishJob will write the pinyin verbatim.
        // That fallback is deliberate — it costs the user a retype, not a
        // sentence — but until now it was also the end of the story: a sentence
        // of pinyin appeared, the box said nothing, and nothing anywhere said
        // why. The status code is the whole diagnosis.
        AppendErrorLog(L"conversion failed: status=%d len=%u\n", status,
                       static_cast<unsigned>(job->pinyin.size()));
    }
    // Any other status leaves `result` empty, which _FinishJob turns into the
    // raw-pinyin fallback.
    _PumpQueue();
}

// KeyEventSink.cpp — ITfKeyEventSink: decide which keys we eat and act on them.
//
// TSF calls OnTestKeyDown first to ask "would you consume this key?" without
// side effects, then OnKeyDown to actually handle it. Both must agree, so they
// share _IsKeyEaten for the decision. The real work lives in _HandleKey.
//
// Input alphabet (no candidate window — the whole design): a-z, the apostrophe
// and the punctuation we keep all build one raw ASCII buffer, drawn in our own
// floating box. NOTHING converts while typing — no timer, no network — so what
// you see is exactly what you typed, and the document is not touched until a
// conversion is ready.
//
// Space is the only key that asks the model for anything: it hands the whole
// buffer to the queue and disappears from the box, so typing continues
// immediately. Enter writes the raw buffer verbatim, with no conversion — the
// escape hatch for English or identifiers. Esc discards what is being typed but
// leaves the queue running. Backspace edits the buffer. Any other key passes
// through to the app untouched.

#include "TextService.h"
#include "Globals.h"
#include "InputWindow.h"
#include "Trace.h"

#include <new>

namespace {

// Is this a pinyin-building character? Lower-case latin letters and the
// apostrophe (syllable separator, e.g. xi'an).
bool IsPinyinChar(WPARAM vk, wchar_t ch) {
    if (vk >= 'A' && vk <= 'Z') return true;          // letters (VK is upper)
    if (ch == L'\'') return true;                       // apostrophe
    return false;
}

// Translate a VK + keyboard state into the produced character, honoring the
// current Shift/CapsLock so we can tell letters apart and reject shifted
// punctuation we don't want. Returns 0 if it doesn't map to a useful char.
wchar_t VkToChar(WPARAM vk, LPARAM /*lParam*/) {
    BYTE keyState[256];
    if (!::GetKeyboardState(keyState)) return 0;
    wchar_t buf[4] = {};
    UINT scan = 0;  // ToUnicode tolerates 0 scan code for our purposes
    int n = ::ToUnicode(static_cast<UINT>(vk), scan, keyState, buf, 4, 0);
    if (n == 1) return buf[0];
    return 0;
}

// Modifier state as of the key event being handled. GetKeyState, never
// GetAsyncKeyState: the async call reads the hardware live, so releasing Shift
// while its own key is still being processed would change the answer underneath
// us. These agree with the GetKeyboardState VkToChar uses.
bool CtrlDown()  { return (::GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool AltDown()   { return (::GetKeyState(VK_MENU) & 0x8000) != 0; }
bool ShiftDown() { return (::GetKeyState(VK_SHIFT) & 0x8000) != 0; }

// We only want bare keys (no Ctrl/Alt) to feed the buffer, so Ctrl+C etc. always
// pass through to the app.
bool CtrlOrAltDown() { return CtrlDown() || AltDown(); }

// Map an ASCII punctuation char to its full-width (全角) equivalent, or 0 if it
// isn't one we remap. \uXXXX escapes keep this independent of the source-file
// encoding (the MSVC build doesn't pass /utf-8).
wchar_t FullWidthPunct(wchar_t ch) {
    switch (ch) {
        case L',':  return 0xFF0C;  // ，
        case L'.':  return 0x3002;  // 。
        case L'?':  return 0xFF1F;  // ？
        case L'!':  return 0xFF01;  // ！
        case L';':  return 0xFF1B;  // ；
        case L':':  return 0xFF1A;  // ：
        case L'(':  return 0xFF08;  // （
        case L')':  return 0xFF09;  // ）
        case L'\\': return 0x3001;  // 、
        default:    return 0;
    }
}

// The two kinds of key we own while typing pinyin: the buffer's alphabet, and
// the punctuation we render full-width. English mode owns neither — that IS the
// mode — so both the decision phase (_IsKeyEaten, which must not claim a key the
// handler will not act on) and the handling phase (_HandleKey) ask through these
// rather than each testing the flag for itself and drifting apart.
bool IsPinyinKey(bool english, WPARAM vk, wchar_t ch) {
    return !english && IsPinyinChar(vk, ch);
}

bool IsFullWidthKey(bool english, wchar_t ch) {
    return !english && FullWidthPunct(ch) != 0;
}

// Which digit selects which candidate. Candidate 0 is labelled 2, so the labels
// run 2..9.
//
// The digit row is spelled with the ASCII characters, not VK_2..VK_9: the SDK's
// winuser.h only *documents* that VK_0..VK_9 are the same as '0'..'9' — it does
// not define the macros, so naming them does not compile.
int CandidateIndex(WPARAM vk) {
    if (vk < static_cast<WPARAM>('2') || vk > static_cast<WPARAM>('9')) return -1;
    return static_cast<int>(vk) - static_cast<int>('2');
}

bool IsCandidateKey(bool english, WPARAM vk) {
    return !english && CandidateIndex(vk) >= 0;
}

}  // namespace

// Does that digit have a candidate to pick? Asked by both phases (see below), so
// the two cannot disagree about whether a digit selects or types.
bool CTextService::_HasCandidateFor(WPARAM vk) const {
    const int n = CandidateIndex(vk);
    return n >= 0 && !_cands.empty() && static_cast<size_t>(n) < _cands.size();
}

// ---- ITfKeyEventSink::OnSetFocus (foreground/background) -------------------

STDMETHODIMP CTextService::OnSetFocus(BOOL /*fForeground*/) {
    // Distinct from ITfThreadMgrEventSink::OnSetFocus; this one just tells us
    // whether our key sink is foreground. Nothing to do.
    return S_OK;
}

// ---- decision: would we eat this key? --------------------------------------

BOOL CTextService::_IsKeyEaten(ITfContext* /*pic*/, WPARAM wParam, LPARAM lParam) {
    // Every Space is logged, which is what tells the two ways Ctrl+Space can fail
    // apart. A line here with ctrl=1 means the chord reached the text service and
    // the fault is ours. No line at all for Ctrl+Space, while a plain Space still
    // logs, means the key never arrived — and the usual cause is that it is not
    // ours to receive: Windows binds "输入法/非输入法切换" to Ctrl+Space per
    // language, handles it below TSF, and answers by switching the language out of
    // the IME and back. The giveaway in the log is a "deactivate" with no key line
    // before it.
    if (wParam == VK_SPACE) {
        DsimeTrace(L"test: space ctrl=%d alt=%d shift=%d english=%d",
                   static_cast<int>(CtrlDown()), static_cast<int>(AltDown()),
                   static_cast<int>(ShiftDown()), static_cast<int>(_englishMode));
    }

    // Ctrl+Space toggles Chinese/English, and it has to be claimed BEFORE the
    // modifier early-out below — that early-out is exactly why the chord reaches
    // the host today. Alt must be up: on several layouts AltGr reports as
    // Ctrl+Alt, and Ctrl+Shift+Space belongs to the app. Shift is allowed.
    if (wParam == VK_SPACE && CtrlDown() && !AltDown()) return TRUE;

    // Never intercept while a modifier is down — let shortcuts through.
    if (CtrlOrAltDown()) return FALSE;

    switch (wParam) {
        case VK_SPACE:
            // Space converts when a buffer is live. With nothing being typed it
            // is still ours while the queue owes this document sentences — as a
            // literal space, which has to land behind them (see _EnqueueIdleChar)
            // — and otherwise belongs to the app.
            return (_composing || !_jobs.empty()) ? TRUE : FALSE;
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_BACK:
            // Enter (write the raw buffer), Esc (discard) and Backspace (edit)
            // only act on a live typing session; with nothing being typed they
            // belong to the app.
            return _composing ? TRUE : FALSE;
        default:
            break;
    }

    // A digit that would pick a candidate. This has to be answered here and not
    // just in _HandleKey: the test phase is what the host asks to find out
    // whether we want the key, and disagreeing with the handler means the host
    // types the digit itself and the buffer never learns about the choice.
    //
    // Only asked when the candidates are actually on screen. A digit with no
    // candidate to pick falls through to the printable-character clause below and
    // becomes part of the sentence, which is what a user typing "3" means.
    if (IsCandidateKey(_englishMode, wParam) && _HasCandidateFor(wParam)) return TRUE;

    wchar_t ch = VkToChar(wParam, lParam);
    // Any bare a-z / apostrophe feeds the buffer. Shift is deliberately not
    // excluded: a capital typed mid-buffer extends what is being typed rather
    // than stranding the buffer, and it is kept as a capital — see the mapping in
    // _HandleKey, which is what lets "shiyongAI" come back as 使用AI.
    if (IsPinyinKey(_englishMode, wParam, ch)) return TRUE;
    // Punctuation is ours too: appended to the buffer while typing, emitted as
    // its full-width (全角) form when idle.
    if (IsFullWidthKey(_englishMode, ch)) return TRUE;
    // Any other printable character is ours while there is something for it to
    // belong to. Two such things:
    //   * a live buffer, which it extends — a digit or a '+' in the middle of
    //     pinyin is part of the sentence (see the mapping in _HandleKey, which
    //     has to agree or the host would type it and the buffer never see it);
    //   * sentences still owed to this document, because it has to take its turn
    //     behind them rather than land in front.
    // With neither, the host handles it, which is both correct and cheaper. In
    // English mode this clause is the whole story: it is what queues a word
    // behind a sentence still converting, and what lets the host type it
    // natively when nothing is pending.
    if ((_composing || !_jobs.empty()) && ch >= 0x20 && ch != 0x7F) return TRUE;
    return FALSE;
}

// ---- test phase (no side effects) ------------------------------------------

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                   BOOL* pfEaten) {
    // No caret probe here: this runs on every keystroke and must stay free of
    // side effects — repositioning the box is done from the layout sink and the
    // layout timer instead.
    *pfEaten = _IsKeyEaten(pic, wParam, lParam);

    // Ctrl+Space is switched HERE, in the test phase, and that is a deliberate
    // exception to "a test has no side effects".
    //
    // The trace from Notepad3 says why. With a composition live — which is to
    // say, whenever there is pinyin to flush, the only case where this key does
    // anything — TSF calls OnTestKeyDown, this service answers "eaten", and
    // OnKeyDown is never called. An ordinary Space gets both callbacks; a
    // Ctrl+Space with a buffer gets one. So a handler in OnKeyDown switched the
    // mode with an empty buffer and silently did nothing with a full one.
    //
    // Registering the chord as a preserved key — the documented answer — is
    // worse: it takes the key at the test stage for good, while OnPreservedKey is
    // delivered from that same missing handle phase. The chord went completely
    // dead, `hr=00000000` and not one callback afterwards.
    //
    // So this is the only callback the host delivers in both states, and the work
    // happens here. Weasel does the same thing for the same class of host
    // misbehaviour — its OnTestKeyDown runs the engine and the composition update
    // while its OnKeyDown only eats the key — so this is a known-good shape for a
    // Windows IME rather than a liberty.
    //
    // _ToggleEnglishMode drops a repeat inside 200 ms, which is what makes it
    // safe here: the hosts that DO reach OnKeyDown, and the ones that call
    // OnTestKeyDown more than once for a single press, cannot turn one chord into
    // two switches. (MS Word 2010 x64 is one of the latter.)
    if (*pfEaten && wParam == VK_SPACE && CtrlDown() && !AltDown()) {
        DsimeTrace(L"test: ctrl+space handled in the test phase");
        _ToggleEnglishMode(pic);
    }
    return S_OK;
}

STDMETHODIMP CTextService::OnTestKeyUp(ITfContext* /*pic*/, WPARAM /*wParam*/,
                                 LPARAM /*lParam*/, BOOL* pfEaten) {
    // We act on key-down only; report not-eaten so key-up flows to the app.
    *pfEaten = FALSE;
    return S_OK;
}

STDMETHODIMP CTextService::OnKeyUp(ITfContext* /*pic*/, WPARAM /*wParam*/,
                             LPARAM /*lParam*/, BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

// Empty, and deliberately so. This is the hook for a preserved key registered
// with ITfKeystrokeMgr::PreserveKey, and that route was tried for Ctrl+Space and
// removed: the trace showed registration succeeding (`hr=00000000`) and the chord
// then producing NO callbacks at all — PreserveKey takes the key at the test
// stage, while OnPreservedKey is delivered from the handle stage, which is the
// one Notepad3 does not reach for this chord. See OnTestKeyDown for where the
// switch actually lives and why.
STDMETHODIMP CTextService::OnPreservedKey(ITfContext* /*pic*/, REFGUID /*rguid*/,
                                    BOOL* pfEaten) {
    *pfEaten = FALSE;
    return S_OK;
}

// ---- handle phase (the real work) ------------------------------------------

STDMETHODIMP CTextService::OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                               BOOL* pfEaten) {
    if (!_IsKeyEaten(pic, wParam, lParam)) {
        *pfEaten = FALSE;
        return S_OK;
    }
    *pfEaten = TRUE;
    return _HandleKey(pic, wParam, lParam, pfEaten);
}

bool CTextService::_EnqueueJob(ITfContext* pic, std::string pinyin,
                               std::wstring literal) {
    std::unique_ptr<PendingJob> job(new (std::nothrow) PendingJob());
    if (!job) return false;

    // Captured NOW, not when the result arrives: focus is allowed to move, and
    // "insert at whatever is selected then" would write the sentence into an
    // unrelated place — a correctness bug, not an edge case.
    if (!Dsime_CaptureInsertAnchor(this, pic, &job->anchor)) {
        DsimeTrace(L"enqueue: ANCHOR CAPTURE FAILED, nothing queued");
        return false;
    }

    job->context = pic;
    pic->AddRef();

    ITfDocumentMgr* pdm = nullptr;
    if (SUCCEEDED(pic->GetDocumentMgr(&pdm)) && pdm) {
        job->docMgr = pdm;  // GetDocumentMgr already AddRef'd it
    }

    if (literal.empty()) {
        job->contextKey = _ContextKeyForFocus();
        job->pinyin = std::move(pinyin);
    } else {
        // Born converted: the pump will hand it straight to the inserter. No
        // context key either — this is a character the user typed, not a request
        // for the model, so it has no business in the conversation history.
        job->literal = true;
        job->converted = true;
        job->result = std::move(literal);
    }

    _jobs.push_back(std::move(job));
    return true;
}

bool CTextService::_EnqueueIdleChar(ITfContext* pic, const std::wstring& text) {
    // With nothing queued the host's own handling of the key is both correct and
    // cheaper — there is no ordering to preserve.
    if (_jobs.empty()) return false;

    // Join the literal job already at the tail rather than mint one per
    // character. This is what makes English mode usable: a word is eight keys,
    // and eight jobs would mean eight anchor captures (each a synchronous edit
    // session) and eight separate compositions opened in the document, for text
    // that ends up in exactly the same place. Typing never moves the document, so
    // the anchors are identical and the merged run lands in the same order.
    //
    // `_jobs.size() > 1` is what keeps the pump's own job out of reach: the head
    // is the one being inserted, and a lone literal head is popped by the
    // _PumpQueue at the end of this function, synchronously, so it can never be
    // seen here.
    //
    // The context test is not decoration. A queued job outlives a focus change —
    // _AbandonTyping drops the typing state, not the queue — so without it an
    // alt-tab mid-word would merge one document's characters into another
    // document's job, and they would land in whichever came first.
    PendingJob* tail = _jobs.size() > 1 ? _jobs.back().get() : nullptr;
    const bool merge = tail != nullptr && tail->literal && tail->context == pic;
    if (merge) {
        tail->result += text;
        DsimeTrace(L"idle: merged '%s' into the tail literal (%u chars)", text.c_str(),
                   static_cast<unsigned>(tail->result.size()));
    } else {
        if (!_EnqueueJob(pic, std::string(), text)) return false;
        DsimeTrace(L"idle: queued '%s' behind %u job(s)", text.c_str(),
                   static_cast<unsigned>(_jobs.size()));
    }

    _writeFailed = false;
    _UpdateInputBox();
    _PumpQueue();
    return true;
}

// The buffer's punctuation as the document should show it.
//
// The buffer stores the punctuation we own as ASCII, on the understanding that a
// conversion hands it to the model, which renders it full-width. THIS path never
// reaches the model — everything was picked from the dictionary, and what is left
// of the buffer is punctuation — so the mapping has to happen here, or a sentence
// the user built entirely from the dictionary lands with English commas in it.
// Nine characters only, the same nine FullWidthPunct owns: letters, digits and
// anything already full-width pass through untouched.
std::wstring ToFullWidthPunct(const std::string& utf8) {
    std::wstring wide = dsime::Utf8ToUtf16(utf8);
    for (wchar_t& c : wide) {
        if (c > 0 && c < 0x80) {
            if (const wchar_t full = FullWidthPunct(c)) c = full;
        }
    }
    return wide;
}

// Snapshot the buffer and the caret into a job for the queue.
bool CTextService::_EnqueueConversion(ITfContext* pic) {
    const std::string payload = _ConversionPayload();
    if (payload.empty()) return false;

    // Two shapes, decided by whether any pinyin is left. Words the user picked are
    // already Chinese, so a buffer with nothing but chosen words needs no model
    // call — it goes through the literal path, which reuses the existing queue,
    // the existing write session and the existing InsertTextAtSelection, so the
    // text lands in the document through exactly the path a comma already uses.
    // Anything still unselected is a real conversion, and the model is told (in
    // the system prompt) to keep the Chinese parts verbatim.
    const bool all_chosen = _pinyin.empty();
    if (all_chosen) {
        // Full-width on the way out, not on the way in: the buffer's ASCII commas
        // are what the segmenter sees, and a full-width one would be an opaque
        // multi-byte span inside a sentence the user is still editing. Every other
        // path out of here is written by the model, which does this for us.
        if (!_EnqueueJob(pic, std::string(), ToFullWidthPunct(payload))) return false;
    } else if (!_EnqueueJob(pic, payload, std::wstring())) {
        return false;
    }

    // The buffer now belongs to the queue. Close the zero-width composition so
    // nothing is left anchored at this caret while the user types the next
    // sentence, and clear the box down to its pending count.
    _EndComposition(pic);
    _ClearTyping();
    _PumpQueue();
    return true;
}

// The buffer, verbatim, as a literal job — Ctrl+Space interrupting what was being
// typed. NOT converted: the user is switching to English mid-word, and spending a
// request to turn an unfinished fragment into Chinese is not what they asked for.
// Same four steps as _EnqueueConversion, with the model left out of it.
void CTextService::_FlushBufferAsLiteral(ITfContext* pic) {
    // The payload, not just _pinyin: words the user already chose are Chinese and
    // belong on the document verbatim, exactly as the unfinished pinyin does.
    const std::string payload = _ConversionPayload();
    if (payload.empty()) return;

    // The preserved-key route can arrive before there is a context to anchor to.
    // Same treatment as a refused edit session — the buffer waits for a keystroke
    // rather than being written somewhere arbitrary.
    if (pic == nullptr) {
        DsimeTrace(L"flushLiteral: no context, deferred");
        _flushBufferAsLiteralDue = true;
        return;
    }

    // _EnqueueJob chooses its branch on `literal.empty()`, not on a flag, so an
    // empty buffer would take the *conversion* path and mint a job with nothing
    // in it. The guard above is what keeps that out of reach; this stays explicit
    // because the discrimination is by content and reads like an accident.
    if (_EnqueueJob(pic, std::string(), dsime::Utf8ToUtf16(payload))) {
        _EndComposition(pic);
        _ClearTyping();
        // Posted, not called: see WM_DSIME_PUMP. This runs from the Ctrl+Space
        // test phase, where asking for the insert session inline gets it
        // deferred, and a deferred session cannot produce the composition handle
        // the write path needs — so it degrades to a write the host never
        // repaints, and the sentence ends up in the document but invisible.
        if (_msgWnd) {
            ::PostMessageW(_msgWnd, WM_DSIME_PUMP, 0, 0);
        } else {
            _PumpQueue();  // no window to post to; better late than never
        }
        return;
    }

    // No anchor — the document refused the synchronous edit session. Deliberately
    // NOT falling back to writing the buffer now: that would put it in front of
    // sentences already owed to this document, which is the one ordering the
    // queue exists to preserve, and the user asked for it to take its turn. Leave
    // everything alone and retry on the next keystroke, where a session is
    // available; Space still converts the buffer in the meantime.
    DsimeTrace(L"flushLiteral: anchor failed, deferred");
    _flushBufferAsLiteralDue = true;
}

// Ctrl+Space: flip between writing Chinese and writing English.
void CTextService::_ToggleEnglishMode(ITfContext* pic) {
    // One press can arrive down both routes — the preserved key when it
    // registered, and the key sink as the fallback — and two toggles in a row are
    // indistinguishable from the switch not working at all. Drop the repeat.
    const ULONGLONG now = ::GetTickCount64();
    if (now - _lastToggleTick < 200) {
        DsimeTrace(L"mode: dropping a repeat within 200ms");
        return;
    }
    _lastToggleTick = now;

    // The buffer first, while the composition is still anchored to it.
    if (!_englishMode && !_pinyin.empty()) _FlushBufferAsLiteral(pic);

    _englishMode = !_englishMode;
    DsimeTrace(L"mode: english=%d", static_cast<int>(_englishMode));

    // Say so. The mode is the only thing on screen that distinguishes the two
    // states, so it flashes the box even when there is nothing else to show —
    // which is exactly the case Ctrl+Space is most often pressed in.
    //
    // Note this does NOT clear _writeFailed the way the conversion paths do: a
    // red badge means a sentence never reached the document, and a mode change is
    // no reason to stop saying so.
    _modeFlash = true;
    if (_msgWnd) {
        ::SetTimer(_msgWnd, DSIME_FLASH_TIMER_ID, DSIME_FLASH_TIMER_MS, nullptr);
    }
    _UpdateInputBox();
}

HRESULT CTextService::_HandleKey(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                 BOOL* pfEaten) {
    // A queued sentence landed in this document while the user was already typing
    // the next one, so the composition is anchored at a caret that has moved.
    // Replacing it needs synchronous edit sessions, which TSF only grants from a
    // key event — never from the posted message that delivered the result — so
    // the queue raised a flag and it is settled here, before this key is
    // processed. Doing it lazily is also what keeps the key itself correct: the
    // new composition is anchored at the current caret, which is where this
    // character belongs.
    if (_reanchorDue) {
        _reanchorDue = false;
        DsimeTrace(L"reanchor: due, applying before key");
        _ReanchorIfComposing(pic);
    }

    // A buffer Ctrl+Space wanted to hand to the queue but could not anchor. Same
    // idiom as _reanchorDue, and for the same reason: capturing an anchor is a
    // synchronous edit session, and TSF only grants those from a key event. The
    // buffer has stayed in the box, and Space still converts it, so the only
    // thing at stake is when it lands.
    if (_flushBufferAsLiteralDue) {
        _flushBufferAsLiteralDue = false;
        DsimeTrace(L"flushLiteral: due, applying before key");
        _FlushBufferAsLiteral(pic);
    }

    DsimeTrace(L"key: vk=%u composing=%d pinyin=%u jobs=%u english=%d",
               static_cast<unsigned>(wParam), static_cast<int>(_composing),
               static_cast<unsigned>(_pinyin.size()),
               static_cast<unsigned>(_jobs.size()), static_cast<int>(_englishMode));

    switch (wParam) {
        case VK_SPACE: {
            // Ctrl+Space is the mode switch, not a conversion. Claimed here as
            // well as in _IsKeyEaten because that is a test, and a test must be
            // free of side effects.
            if (CtrlDown() && !AltDown()) {
                _ToggleEnglishMode(pic);
                return S_OK;
            }

            // The one and only conversion trigger: hand the whole buffer to the
            // queue. The pinyin vanishes from the box immediately and the user
            // carries straight on with the next sentence; results land in the
            // document as they arrive, in order.
            //
            // "_pinyin is empty" is NOT the same as "nothing to send". A buffer the
            // user chose all the way through has no pinyin left and a payload of
            // chosen Chinese, and that payload is the entire point of selecting —
            // it goes down the literal path with no model call. Testing `_pinyin`
            // alone sent it to the idle-character branch, which returned the key
            // to the host when the queue was empty, and the words they had just
            // picked vanished with a bare space in their place.
            if (!_composing || (_pinyin.empty() && _chosen.empty())) {
                // A literal space, with sentences still owed to this document:
                // it has to queue behind them, or it lands in front of the
                // sentence the user just committed. See _EnqueueIdleChar.
                if (_EnqueueIdleChar(pic, L" ")) return S_OK;
                *pfEaten = FALSE;
                return S_OK;
            }
            if (_jobs.size() >= _maxPending) {
                // Backpressure. Note this eats the key rather than handing it
                // back: with a live buffer, Space is unambiguously ours, and
                // passing it through would drop a literal space into the
                // document while the pinyin sat unconverted in the box. Doing
                // nothing is self-correcting — the queue drains in a second or
                // two and the next Space converts. The box showing the pending
                // count is the user's feedback.
                return S_OK;
            }
            // A failure here (read-only document, selection gone) leaves the
            // buffer in the box to be edited or retried.
            (void)_EnqueueConversion(pic);
            return S_OK;
        }
        case VK_RETURN: {
            // Write the raw buffer verbatim — no conversion. The escape hatch for
            // English words, identifiers, or anything else the model would
            // otherwise try to turn into Chinese.
            if (!_composing) { *pfEaten = FALSE; return S_OK; }
            HRESULT hr = _CommitComposition(pic, dsime::Utf8ToUtf16(_pinyin));
            _ClearTyping();
            return hr;
        }
        case VK_ESCAPE: {
            // Discard what is being typed: the box clears and not a character
            // reaches the document. The queue is deliberately left running —
            // sentences already committed with Space are still owed.
            if (!_composing) { *pfEaten = FALSE; return S_OK; }
            _EndComposition(pic);
            _ClearTyping();
            return S_OK;
        }
        case VK_BACK: {
            if (!_composing) { *pfEaten = FALSE; return S_OK; }
            if (!_pinyin.empty()) {
                _pinyin.pop_back();
                _Resegment();
            } else if (!_UndoLastChoice()) {
                // No pinyin and no choice to revert: the buffer really is empty.
                _EndComposition(pic);
                _ClearTyping();
                return S_OK;
            } else {
                // Reverted a choice, so there is a buffer again — keep composing
                // rather than ending the composition and immediately reopening it.
                _UpdateInputBox();
                return S_OK;
            }
            if (_pinyin.empty() && _chosen.empty()) {
                // Nothing left to show or to anchor: drop the composition too.
                _EndComposition(pic);
                _ClearTyping();
                return S_OK;
            }
            _UpdateInputBox();
            return S_OK;
        }
        default: {
            // A digit picks a dictionary candidate when one is on screen, and
            // falls through to the ordinary character path when one is not. The
            // test phase asked the same question (_HasCandidateFor), so reaching
            // here with no candidate means the host was already told we would not
            // take the key.
            if (IsCandidateKey(_englishMode, wParam) && _HasCandidateFor(wParam) &&
                _SelectCandidate(static_cast<size_t>(CandidateIndex(wParam)))) {
                _UpdateInputBox();
                return S_OK;
            }

            wchar_t ch = VkToChar(wParam, lParam);

            // Idle: there is no buffer to add the character to, so it is either
            // queued behind what the document is owed, or emitted on the spot.
            //
            // Everything the host would have typed as text goes through the same
            // door, and for the same reason: a queued sentence is inserted at the
            // selection as it stands when its turn comes, so anything written now
            // moves the caret past itself and the sentence lands behind it — the
            // user reads "，你好" for what they meant as "你好，". Taking a number
            // matches the intent too: the character belongs to the sentence just
            // typed.
            //
            // (Keys that produce no character at all — arrows, F-keys, Tab — never
            // reach here: ToUnicode maps them to nothing, or to a control code
            // below the printable range, and _IsKeyEaten never ate them.)
            //
            // English mode starts here. Nothing is pinyin and nothing is
            // remapped, so every printable character takes the literal path and
            // then stops — it must NOT fall through to the buffer mapping below,
            // which would quietly start a pinyin session out of English text.
            // (This is reachable only with a non-empty queue: with an empty one
            // _IsKeyEaten let the key go, and the host types it natively.)
            if (_englishMode) {
                if (!_composing && ch >= 0x20 && ch != 0x7F &&
                    _EnqueueIdleChar(pic, std::wstring(1, ch))) {
                    return S_OK;
                }
                *pfEaten = FALSE;
                return S_OK;
            }

            if (!_composing) {
                if (wchar_t full = FullWidthPunct(ch)) {
                    const std::wstring tail(1, full);
                    if (_EnqueueIdleChar(pic, tail)) return S_OK;

                    DsimeTrace(L"punct: idle, emitting U+%04X", static_cast<unsigned>(full));
                    HRESULT hr = _StartComposition(pic);
                    if (FAILED(hr)) {
                        DsimeTrace(L"punct: startComposition FAILED hr=%08X, key released",
                                   static_cast<unsigned>(hr));
                        *pfEaten = FALSE;
                        return hr;
                    }
                    hr = _CommitComposition(pic, tail);
                    DsimeTrace(L"punct: commit hr=%08X", static_cast<unsigned>(hr));
                    _ClearTyping();
                    return hr;
                }
                // NOT a letter or the apostrophe, though: those are printable too,
                // and they belong to the buffer below. Swallowing them here queued
                // the pinyin one character at a time as literal text and wrote it
                // out unconverted. The test is the same one that decides the
                // buffer's alphabet, so the two cannot drift apart.
                if (ch >= 0x20 && ch != 0x7F && !IsPinyinChar(wParam, ch) &&
                    _EnqueueIdleChar(pic, std::wstring(1, ch))) {
                    return S_OK;
                }
            }

            // Map the key to its ASCII form for the buffer. Punctuation is
            // stored as ASCII — the model renders it full-width together with
            // the sentence.
            //
            // Letters keep the case the user typed, and the case comes from the
            // SHIFT KEY, not from `ch`. Capital letters in the buffer are what
            // makes an abbreviation survive: "shiyongAI" is converted to 使用AI,
            // while the flattened "shiyongai" gives the model no reason to prefer
            // AI over 爱 — it reads it as pinyin, because that is what the rest
            // of the buffer is.
            //
            // Reading the case off `ch` instead would break pinyin outright:
            // with CapsLock on, ToUnicode returns capitals for ordinary typing
            // too, so every sentence would reach the model as "NIHAO" — the very
            // shape this is using to mean "English". Shift is also the only
            // signal that is deliberate. Note this makes CapsLock+Shift produce a
            // lower-case letter, which is Windows' own behaviour and the harmless
            // direction to be wrong in.
            const bool shifted = ShiftDown();
            char ascii = 0;
            if (wParam >= 'A' && wParam <= 'Z') {
                // Taken from the VK, which is the upper-case letter whatever
                // Shift and CapsLock are doing, so the two sources below cannot
                // disagree about which letter this is.
                ascii = shifted ? static_cast<char>(wParam)
                                : static_cast<char>(wParam - 'A' + 'a');
            } else if (ch == L'\'') {
                ascii = '\'';
            } else if (ch > 0 && ch < 0x80 && FullWidthPunct(ch) != 0) {
                ascii = static_cast<char>(ch);
            } else if (_composing && ch >= 0x20 && ch < 0x7F) {
                // Anything else printable, typed into a live buffer, belongs to
                // the sentence being written: a digit or a '+' in the middle of
                // pinyin carries meaning, and the model is told to keep numbers,
                // code identifiers and URLs exactly as written.
                //
                // The key used to be released to the host here instead, and it
                // never arrived: a live composition is precisely the state in
                // which the host has nowhere to put a character it was not asked
                // to compose, so the symbol was silently dropped. Only
                // mid-buffer, though — with nothing being typed there is no
                // sentence for it to belong to, and the host's own handling is
                // both correct and cheaper.
                ascii = static_cast<char>(ch);
            }
            if (ascii == 0) { *pfEaten = FALSE; return S_OK; }

            if (!_composing) {
                // The zero-width composition is what proves the document is
                // writable. If it cannot be started we hand the key back rather
                // than swallowing it, so typing in a read-only field behaves as
                // if no IME were installed at all.
                HRESULT hr = _StartComposition(pic);
                if (FAILED(hr)) { *pfEaten = FALSE; return hr; }
                _composing = true;
            }
            _pinyin.push_back(ascii);
            // Re-segment on every keystroke, so the candidate row is live. The
            // dictionary is memory-mapped and the query is a binary search, which
            // is what makes this affordable per character; _Resegment clears the
            // list itself when there is no dictionary, so the box falls back to
            // showing the buffer alone.
            _Resegment();
            // Purely cosmetic from here: the buffer lives in the box, and the
            // document is not touched until Space.
            _UpdateInputBox();
            return S_OK;
        }
    }
}

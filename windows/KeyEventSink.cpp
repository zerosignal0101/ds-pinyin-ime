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

// Modifier keys held? We only want bare keys (no Ctrl/Alt) to feed the buffer,
// so Ctrl+C etc. always pass through to the app.
bool CtrlOrAltDown() {
    return (::GetKeyState(VK_CONTROL) & 0x8000) ||
           (::GetKeyState(VK_MENU) & 0x8000);
}

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

}  // namespace

// ---- ITfKeyEventSink::OnSetFocus (foreground/background) -------------------

STDMETHODIMP CTextService::OnSetFocus(BOOL /*fForeground*/) {
    // Distinct from ITfThreadMgrEventSink::OnSetFocus; this one just tells us
    // whether our key sink is foreground. Nothing to do.
    return S_OK;
}

// ---- decision: would we eat this key? --------------------------------------

BOOL CTextService::_IsKeyEaten(ITfContext* /*pic*/, WPARAM wParam, LPARAM lParam) {
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

    wchar_t ch = VkToChar(wParam, lParam);
    // Any bare a-z / apostrophe feeds the buffer. Shift is deliberately not
    // excluded: a capital mid-buffer should extend what is being typed rather
    // than strand the buffer.
    if (IsPinyinChar(wParam, ch)) return TRUE;
    // Punctuation is ours too: appended to the buffer while typing, emitted as
    // its full-width (全角) form when idle.
    if (FullWidthPunct(ch) != 0) return TRUE;
    // Any other printable character is ours *while sentences are still owed to
    // this document*, because it has to take its turn behind them rather than
    // land in front. With an empty queue the host handles it, which is both
    // correct and cheaper.
    if (!_jobs.empty() && ch >= 0x20 && ch != 0x7F) return TRUE;
    return FALSE;
}

// ---- test phase (no side effects) ------------------------------------------

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                   BOOL* pfEaten) {
    // Note: no caret probe here. This runs on every keystroke and must stay free
    // of side effects — repositioning the box is done from the layout sink and
    // the layout timer instead.
    *pfEaten = _IsKeyEaten(pic, wParam, lParam);
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
    if (!_EnqueueJob(pic, std::string(), text)) return false;
    DsimeTrace(L"idle: queued '%s' behind %u job(s)", text.c_str(),
               static_cast<unsigned>(_jobs.size()));
    _writeFailed = false;
    _UpdateInputBox();
    _PumpQueue();
    return true;
}

// Snapshot the buffer and the caret into a job for the queue.
bool CTextService::_EnqueueConversion(ITfContext* pic) {
    if (!_EnqueueJob(pic, _pinyin, std::wstring())) return false;

    // The buffer now belongs to the queue. Close the zero-width composition so
    // nothing is left anchored at this caret while the user types the next
    // sentence, and clear the box down to its pending count.
    _EndComposition(pic);
    _pinyin.clear();
    _composing = false;
    _writeFailed = false;
    _UpdateInputBox();
    _PumpQueue();
    return true;
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

    DsimeTrace(L"key: vk=%u composing=%d pinyin=%u jobs=%u", static_cast<unsigned>(wParam),
               static_cast<int>(_composing), static_cast<unsigned>(_pinyin.size()),
               static_cast<unsigned>(_jobs.size()));

    switch (wParam) {
        case VK_SPACE: {
            // The one and only conversion trigger: hand the whole buffer to the
            // queue. The pinyin vanishes from the box immediately and the user
            // carries straight on with the next sentence; results land in the
            // document as they arrive, in order.
            if (!_composing || _pinyin.empty()) {
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
            if (!_pinyin.empty()) _pinyin.pop_back();
            if (_pinyin.empty()) {
                // Nothing left to show or to anchor: drop the composition too.
                _EndComposition(pic);
                _ClearTyping();
                return S_OK;
            }
            _UpdateInputBox();
            return S_OK;
        }
        default: {
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

            // Map the produced character to its ASCII form for the buffer.
            // Punctuation is stored as ASCII — the model renders it full-width
            // together with the sentence.
            char ascii = 0;
            if (ch >= L'A' && ch <= L'Z') {
                ascii = static_cast<char>(ch - L'A' + 'a');
            } else if (ch >= L'a' && ch <= L'z') {
                ascii = static_cast<char>(ch);
            } else if (ch == L'\'') {
                ascii = '\'';
            } else if (ch > 0 && ch < 0x80 && FullWidthPunct(ch) != 0) {
                ascii = static_cast<char>(ch);
            } else if (wParam >= 'A' && wParam <= 'Z') {
                // Defensive: VK said letter but ToUnicode didn't agree. Derive
                // from VK directly.
                ascii = static_cast<char>(wParam - 'A' + 'a');
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
            // Purely cosmetic from here: the buffer lives in the box, and the
            // document is not touched until Space.
            _UpdateInputBox();
            return S_OK;
        }
    }
}

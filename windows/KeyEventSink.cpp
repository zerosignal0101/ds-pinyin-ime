// KeyEventSink.cpp — ITfKeyEventSink: decide which keys we eat and act on them.
//
// TSF calls OnTestKeyDown first to ask "would you consume this key?" without
// side effects, then OnKeyDown to actually handle it. Both must agree, so they
// share _IsKeyEaten for the decision. The real work lives in _HandleKey, which
// drives the composition (start / update / commit / cancel).
//
// Input alphabet (no candidate UI — the whole design): a-z, the apostrophe and
// the punctuation we keep all build one raw ASCII buffer, shown verbatim as the
// pre-edit. NOTHING converts while typing — no timer, no network — so what you
// see is exactly what you typed.
//
// Space is the only key that asks the model for anything: it converts the whole
// buffer and writes the resulting sentence straight into the document, in one
// step (see _commitOnResult). Enter writes the raw buffer verbatim, with no
// conversion — the escape hatch for English or identifiers. Esc discards
// everything without writing a character. Backspace edits the buffer. Any other
// key passes through to the app untouched.

#include "TextService.h"
#include "Globals.h"

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
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_BACK:
            // Space (convert + commit), Enter (write the raw buffer), Esc
            // (discard) and Backspace (edit) only act on a live composition; with
            // an empty buffer they belong to the app.
            return _HasComposition() ? TRUE : FALSE;
        case VK_UP:
        case VK_DOWN:
        case VK_TAB:
            // Up/down (and Tab = next) cycle through LLM candidates, but only once
            // a converted sentence is shown; otherwise let the key reach the app.
            return (_HasComposition() && _showingConverted) ? TRUE : FALSE;
        default:
            break;
    }

    wchar_t ch = VkToChar(wParam, lParam);
    // Any bare a-z / apostrophe feeds the buffer. Shift is deliberately not
    // excluded: a capital mid-buffer should extend what is being typed rather
    // than strand the pre-edit.
    if (IsPinyinChar(wParam, ch)) return TRUE;
    // Punctuation is ours too: appended to the buffer while composing, emitted
    // as its full-width (全角) form when idle.
    if (FullWidthPunct(ch) != 0) return TRUE;
    return FALSE;
}

// ---- test phase (no side effects) ------------------------------------------

STDMETHODIMP CTextService::OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                   BOOL* pfEaten) {
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

HRESULT CTextService::_HandleKey(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                                 BOOL* pfEaten) {
    switch (wParam) {
        case VK_SPACE: {
            // The one and only conversion trigger. The whole buffer goes to the
            // model, and the terminal result is written straight into the
            // document (via _commitOnResult) rather than merely previewed.
            if (_commitOnResult) {
                // Already waiting on a result: swallow the repeat instead of
                // firing a second, redundant request.
                return S_OK;
            }
            if (!_HasComposition() || _pinyin.empty()) {
                *pfEaten = FALSE;
                return S_OK;
            }
            _commitOnResult = true;
            if (!_FireConversion()) _commitOnResult = false;
            return S_OK;
        }
        case VK_RETURN: {
            // Write the raw buffer verbatim — no conversion. The escape hatch for
            // English words, identifiers, or anything else the model would
            // otherwise try to turn into Chinese.
            if (!_HasComposition()) { *pfEaten = FALSE; return S_OK; }
            _session.Cancel();
            HRESULT hr = _CommitComposition(pic, dsime::Utf8ToUtf16(_pinyin));
            _ResetBuffer();
            return hr;
        }
        case VK_ESCAPE: {
            // Discard: drop any in-flight request and clear the pre-edit without
            // writing a single character to the document.
            if (!_HasComposition()) { *pfEaten = FALSE; return S_OK; }
            _session.Cancel();
            HRESULT hr = _EndComposition(pic);
            _ResetBuffer();
            (void)hr;
            return S_OK;
        }
        case VK_UP:
        case VK_DOWN:
        case VK_TAB: {
            // Cycle alternative LLM conversions of the current input. A cached
            // candidate shows instantly; going down (or Tab) past the last asks the
            // core to regenerate a different one (streamed back via the usual
            // handler); going up past the primary is a no-op (but still eaten).
            int32_t dir = (wParam == VK_UP) ? -1 : 1;
            dsime::CoreString alt = _session.CandidateCached(dir);
            if (!alt.empty()) {
                _displayText = alt.to_wstring();
                _showingConverted = true;
                return _UpdateCompositionText(pic, _displayText, TRUE);
            }
            if (dir > 0) _FireRegenerate();
            return S_OK;
        }
        case VK_BACK: {
            // Edit the buffer: drop the last character. Editing also supersedes a
            // conversion Space set running, so a pending result can never be
            // committed over the user's correction.
            _AbandonPendingConversion();
            if (!_pinyin.empty()) {
                _pinyin.pop_back();
            }
            if (_pinyin.empty()) {
                _session.Cancel();
                HRESULT hr = _EndComposition(pic);
                _ResetBuffer();
                (void)hr;
                return S_OK;
            }
            _session.SetInput(_pinyin);
            _showingConverted = false;
            _displayText = dsime::Utf8ToUtf16(_pinyin);
            return _UpdateCompositionText(pic, _displayText, TRUE);
        }
        default: {
            wchar_t ch = VkToChar(wParam, lParam);

            // Idle + punctuation: there is no buffer to add it to, so emit the
            // full-width (全角) symbol directly.
            if (!_HasComposition()) {
                if (wchar_t full = FullWidthPunct(ch)) {
                    std::wstring tail(1, full);
                    HRESULT hr = _StartComposition(pic);
                    if (FAILED(hr)) { *pfEaten = FALSE; return hr; }
                    hr = _CommitComposition(pic, tail);
                    _ResetBuffer();
                    return hr;
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

            // Typing again supersedes a conversion Space set running.
            _AbandonPendingConversion();

            if (!_HasComposition()) {
                HRESULT hr = _StartComposition(pic);
                if (FAILED(hr)) { *pfEaten = FALSE; return hr; }
            }
            _pinyin.push_back(ascii);
            _session.SetInput(_pinyin);

            // Show the raw buffer verbatim. Nothing converts here: the pre-edit
            // stays exactly what was typed until Space is pressed.
            _showingConverted = false;
            _displayText = dsime::Utf8ToUtf16(_pinyin);
            return _UpdateCompositionText(pic, _displayText, TRUE);
        }
    }
}

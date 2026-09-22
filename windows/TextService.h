// TextService.h — the DS Input TSF text service.
//
// CTextService is the single COM object that TSF instantiates from our CLSID.
// It implements, in one class:
//
//   ITfTextInputProcessorEx  — entry point; Activate/Deactivate lifecycle.
//   ITfThreadMgrEventSink     — learn when focus moves between documents.
//   ITfKeyEventSink           — preview/handle keystrokes (the input logic).
//   ITfCompositionSink        — told when our composition is terminated by TSF.
//   ITfDisplayAttributeProvider — supplies the underline style for the pre-edit.
//   ITfThreadFocusSink        — (optional) thread focus, used to hide UI.
//
// THREADING (the crux of this IME):
//   TSF runs us on a single-threaded apartment (STA) UI thread. Every TSF call
//   and every composition mutation MUST happen on that thread. The dsime core's
//   conversion callback, however, fires on a Tokio worker thread. We therefore:
//     1. Create a hidden message-only window (CTextService::_msgWnd) owned by
//        the STA thread during Activate.
//     2. In the core callback (DsConvertCallback, a static C function) we copy
//        the result, stash it in a heap struct, and PostMessage it to that
//        window. PostMessage is the one Win32 call safe to invoke from any
//        thread to hand work to another thread.
//     3. The window proc (on the STA thread) picks the message up and runs an
//        edit session to replace the composition text — guarding on request_id
//        so a stale/late result for an older buffer is dropped.
//
// COMPOSITION LIFECYCLE (nothing converts while typing — only Space asks the
// model for anything):
//   * First pinyin key with no active composition -> _StartComposition: open an
//     ITfComposition via ITfContextComposition::StartComposition inside an edit
//     session, then write the raw buffer and apply the underline display attr.
//   * Subsequent pinyin / punctuation / Backspace -> _UpdateCompositionText with
//     the updated raw buffer. No network traffic and no timer.
//   * Space -> _FireConversion and set _commitOnResult. Partials redraw the
//     pre-edit as the sentence streams in; the terminal result is written and
//     the composition ended — convert and commit in one step.
//   * Enter -> commit the raw buffer verbatim, with no conversion.
//   * Esc -> discard everything: the pre-edit is cleared and nothing is written.
//   * TSF may terminate the composition itself (focus loss, app teardown) ->
//     OnCompositionTerminated clears our state.

#pragma once

#include <windows.h>
#include <msctf.h>
#include <string>

#include "DsimeCore.h"

// Window message we post from the core worker thread to the STA thread to
// deliver a finished conversion. lParam owns a heap ConvertResult* and carries
// the per-request ref taken in _FireConversion (the proc releases it).
#define WM_DSIME_CONVERT_RESULT  (WM_USER + 0x100)
// A streamed PARTIAL update (cumulative text). lParam owns a heap ConvertResult*
// but does NOT carry the per-request ref — only the terminal RESULT does.
#define WM_DSIME_CONVERT_PARTIAL (WM_USER + 0x101)

class CTextService final : public ITfTextInputProcessorEx,
                           public ITfThreadMgrEventSink,
                           public ITfThreadFocusSink,
                           public ITfKeyEventSink,
                           public ITfCompositionSink,
                           public ITfDisplayAttributeProvider {
public:
    CTextService();

    // ---- IUnknown ----
    STDMETHODIMP          QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG)  AddRef() override;
    STDMETHODIMP_(ULONG)  Release() override;

    // ---- ITfTextInputProcessor / Ex ----
    STDMETHODIMP Activate(ITfThreadMgr* ptim, TfClientId tid) override;
    STDMETHODIMP Deactivate() override;
    STDMETHODIMP ActivateEx(ITfThreadMgr* ptim, TfClientId tid, DWORD dwFlags) override;

    // ---- ITfThreadMgrEventSink ----
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext* pic) override;
    STDMETHODIMP OnPopContext(ITfContext* pic) override;

    // ---- ITfThreadFocusSink ----
    STDMETHODIMP OnSetThreadFocus() override;
    STDMETHODIMP OnKillThreadFocus() override;

    // ---- ITfKeyEventSink ----
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

    // ---- ITfCompositionSink ----
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition* pComposition) override;

    // ---- ITfDisplayAttributeProvider ----
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) override;

    // ---- accessors used by the edit-session and lang-bar helpers ----
    ITfThreadMgr* ThreadMgr() const { return _pThreadMgr; }
    TfClientId    ClientId()  const { return _tid; }
    ITfComposition* Composition() const { return _pComposition; }
    dsime::Engine&  Engine()  { return _engine; }
    dsime::Session& CoreSession() { return _session; }

private:
    ~CTextService();

    // ---- sink (un)advise helpers, implemented in TextService.cpp ----
    BOOL _InitThreadMgrEventSink();
    void _UninitThreadMgrEventSink();
    BOOL _InitKeyEventSink();
    void _UninitKeyEventSink();
    BOOL _InitThreadFocusSink();
    void _UninitThreadFocusSink();
    BOOL _InitDisplayAttributeGuidAtom();
    BOOL _InitLanguageBar();
    void _UninitLanguageBar();

    // ---- the hidden marshaling window (TextService.cpp) ----
    BOOL _CreateMessageWindow();
    void _DestroyMessageWindow();
    static LRESULT CALLBACK _MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- key handling (KeyEventSink.cpp) ----
    // Returns TRUE if the key is one this IME consumes in the current state.
    BOOL _IsKeyEaten(ITfContext* pic, WPARAM wParam, LPARAM lParam);
    HRESULT _HandleKey(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);

    // ---- composition / edit sessions (Composition.cpp, EditSessions.cpp) ----
    HRESULT _StartComposition(ITfContext* pic);
    HRESULT _UpdateCompositionText(ITfContext* pic, const std::wstring& text, BOOL underline);
    HRESULT _CommitComposition(ITfContext* pic, const std::wstring& text);
    HRESULT _EndComposition(ITfContext* pic);
    BOOL    _HasComposition() const { return _pComposition != nullptr; }

    // ---- conversion plumbing (Composition.cpp) ----
    // STA thread: snapshot the buffer and issue ds_session_convert_stream. Returns
    // true when a request was actually issued, so its callback is guaranteed.
    bool _FireConversion();
    void _FireRegenerate();  // STA thread: ask the core for a different candidate
    // Drop a conversion that Space set running, because the user went back to
    // editing and its result must not land over the new input.
    void _AbandonPendingConversion();
    static void _ConvertCallbackThunk(void* user_data, uint64_t request_id,
                                      int32_t status, const char* text_utf8);
    // Streaming thunk (CORE WORKER THREAD): posts partials as WM_DSIME_CONVERT_PARTIAL
    // and the terminal outcome as WM_DSIME_CONVERT_RESULT (which carries the ref).
    static void _StreamCallbackThunk(void* user_data, uint64_t request_id,
                                     int32_t status, int32_t is_final,
                                     const char* text_utf8);
    void _OnConvertResultOnStaThread(uint64_t request_id, int32_t status,
                                     const std::wstring& text, bool is_final);

    // Reset the pinyin buffer + core session after commit / cancel.
    void _ResetBuffer();

private:
    LONG _cRef = 1;  // born referenced (class factory does the first AddRef)

    // TSF wiring, valid between Activate and Deactivate.
    ITfThreadMgr* _pThreadMgr = nullptr;
    TfClientId    _tid = TF_CLIENTID_NULL;

    DWORD _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    DWORD _dwThreadFocusSinkCookie    = TF_INVALID_COOKIE;

    // The active composition, non-null only while composing. Held with a ref.
    ITfComposition* _pComposition = nullptr;
    // Context that owns the active composition; we keep a ref so async edit
    // sessions target the right document even if focus has wandered.
    ITfContext* _pCompositionContext = nullptr;

    // Display-attribute atom for our underline, resolved once in Activate.
    TfGuidAtom _gaDisplayAttribute = TF_INVALID_GUIDATOM;

    // Language-bar button (Settings launcher).
    ITfLangBarItemButton* _pLangBarButton = nullptr;

    // Core engine + session (RAII). Engine outlives session by member order:
    // _engine is declared before _session so it is destroyed *after* it.
    dsime::Engine  _engine;
    dsime::Session _session;

    // Raw ASCII typed so far: lower-case pinyin letters, the apostrophe syllable
    // separator, and the punctuation we keep in the buffer (the model renders it
    // full-width). Source of truth for what we send to the core; the composition
    // may show either this or the converted Chinese.
    std::string _pinyin;
    // What the composition currently displays (so commit knows what to write
    // when no conversion has arrived yet).
    std::wstring _displayText;
    // TRUE while _displayText holds converted Chinese (vs. raw pinyin).
    bool _showingConverted = false;

    // The id of the most recent ds_session_convert we issued. Results carrying a
    // smaller/older id are stale and dropped.
    uint64_t _lastRequestId = 0;

    // TRUE from the moment Space issues a conversion until its terminal result
    // lands: a request is in flight, and whatever comes back is to be written to
    // the document rather than merely previewed. Cleared on any terminal
    // outcome, so a failed conversion leaves the buffer editable and Space can
    // simply be pressed again to retry.
    bool _commitOnResult = false;

    // Hidden message-only window for cross-thread marshaling; STA-thread-owned.
    HWND _msgWnd = nullptr;
};

// Heap payload posted from the worker thread to the STA thread. Owned by the
// receiver (the window proc deletes it).
struct ConvertResult {
    CTextService* pThis;       // borrowed; valid because we hold a ref while
                               // a request is in flight (see _FireConversion)
    uint64_t      request_id;
    int32_t       status;
    std::wstring  text;        // already converted to UTF-16
};

// TextService.h — the DS Pinyin IME TSF text service.
//
// CTextService is the single COM object that TSF instantiates from our CLSID.
// It implements, in one class:
//
//   ITfTextInputProcessorEx  — entry point; Activate/Deactivate lifecycle.
//   ITfThreadMgrEventSink     — learn when focus moves between documents.
//   ITfKeyEventSink           — preview/handle keystrokes (the input logic).
//   ITfCompositionSink        — told when our composition is terminated by TSF.
//   ITfTextLayoutSink         — follow the caret as the document reflows.
//   ITfThreadFocusSink        — thread focus, used to hide the floating box.
//
// THREADING (the crux of this IME):
//   TSF runs us on a single-threaded apartment (STA) UI thread. Every TSF call
//   and every document mutation MUST happen on that thread. The dsime core's
//   conversion callback, however, fires on a Tokio worker thread. We therefore:
//     1. Create a hidden message-only window (CTextService::_msgWnd) owned by
//        the STA thread during Activate.
//     2. In the core callback (DsConvertCallback, a static C function) we copy
//        the result, stash it in a heap struct, and PostMessage it to that
//        window. PostMessage is the one Win32 call safe to invoke from any
//        thread to hand work to another thread.
//     3. The window proc (on the STA thread) picks the message up and enqueues
//        the sentence for insertion.
//
// INPUT MODEL (nothing converts while typing — only Space asks the model):
//   * First pinyin key with no live session -> _StartComposition opens a
//     ZERO-WIDTH ITfComposition at the caret. It holds no text and never will:
//     the pinyin is drawn in our own floating box (InputWindow.h) instead. The
//     composition earns its keep by (a) proving the document is writable — a
//     failure means we hand the key to the host rather than swallowing it,
//     (b) telling TSF and the app that a text edit is in progress, which is
//     what stops a search box from querying on every keystroke, and (c) giving
//     us an insertion point that TSF keeps valid across edits.
//   * Subsequent keys -> append to the raw buffer and repaint the box. No
//     network traffic, no document writes, no timer.
//   * Shift+letter -> still buffer, but kept as a CAPITAL, so an abbreviation
//     reaches the model as one: "shiyongAI" converts to 使用AI, where the
//     flattened "shiyongai" would just be read as more pinyin.
//   * Space -> snapshot the buffer and the caret into a PendingJob and hand it
//     to the queue. The pinyin disappears at once and typing continues.
//   * Enter -> write the raw buffer verbatim, with no conversion.
//   * Esc -> discard what is being typed. The queue keeps running. Deliberately
//     does NOT leave English mode: a key that silently changes the input mode is
//     worse than one that does nothing, and the box already says which mode is on.
//   * Ctrl+Space -> toggle Chinese/English (_ToggleEnglishMode). ENGLISH MODE
//     HAS NO BUFFER: nothing is pinyin and nothing is remapped to full-width, so
//     every printable key takes the literal path above — queued behind any
//     sentence still on its way, and handed to the host when the queue is empty,
//     which is what makes typing a whole sentence of English feel native. A
//     buffer in progress when the mode flips is flushed verbatim, not converted.
//   * TSF may terminate the composition itself (focus loss, app teardown) ->
//     OnCompositionTerminated clears our typing state.
//
// THE QUEUE. Conversions run strictly one at a time, in the order the user
// asked for them, and each result is inserted at the anchor captured when its
// Space was pressed — so a sentence still lands in the document it was typed
// into even if focus has since moved elsewhere. This is deliberately frontend
// state (see CLAUDE.md): the core's session is a single-flight buffer, and
// keeping the queue here is what makes "the next request sees the previous
// result" and "the results appear in order" true by construction.
//
// The queue also carries *literal* jobs — punctuation typed with nothing in the
// buffer while sentences are still on their way. It has to be in the queue for
// the order to come out right; see PendingJob::literal.

#pragma once

#include <windows.h>
#include <msctf.h>

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "DsimeCore.h"

class DSPinyinIMEBoxWnd;

// Window message we post from the core worker thread to the STA thread to
// deliver a finished conversion. lParam owns a heap ConvertResult* and carries
// the per-request ref taken in _StartConversion (the proc releases it).
#define WM_DSIME_CONVERT_RESULT  (WM_USER + 0x100)

// Posted to ourselves from ITfTextLayoutSink::OnLayoutChange, which runs with
// the document locked. Repositioning the box needs a synchronous read session,
// and those are not safe from inside a lock — so the callback only records that
// a reposition is due, and the work happens here, as soon as the lock is gone.
#define WM_DSIME_RELOCATE        (WM_USER + 0x101)

// Posted to ourselves to run a queued literal insertion.
//
// The insert sessions use TF_ES_ASYNCDONTCARE, because they normally run from a
// posted message, where TF_ES_SYNC is not permitted. Ask for one during a *key
// event* instead — which is what happens when Ctrl+Space flushes the buffer from
// the test phase — and TSF defers it: the session has not run yet, so the
// composition handle it was supposed to produce is still null, and the caller
// concludes it could not open one and takes the bare-write fallback. That path is
// documented not to notify, so the text lands in the document and the host never
// repaints it: the sentence is there and invisible. Posting the pump puts the
// insert back in the context it was written for.
#define WM_DSIME_PUMP            (WM_USER + 0x102)

// Timer on the hidden window: re-locates the floating box while the user is
// typing. Hosts that never fire ITfTextLayoutSink (or that move the caret
// without changing the text) are only tracked this way.
#define DSIME_LAYOUT_TIMER_ID    1
#define DSIME_LAYOUT_TIMER_MS    150

// Timer on the hidden window: retry a queued insertion the document refused
// because it was mid-edit (TF_E_LOCKED).
#define DSIME_PUMP_TIMER_ID      2
#define DSIME_PUMP_TIMER_MS      50
#define DSIME_MAX_INSERT_RETRIES 20

// Timer on the hidden window: how long Ctrl+Space's "English"/"Chinese" notice
// stays on screen when there is nothing else for the box to show. It must be an
// id of its own: SetTimer on an id that is already armed *replaces* that timer,
// interval and all, so sharing the pump's id 2 would let a mode change quietly
// cancel a pending insertion retry.
#define DSIME_FLASH_TIMER_ID     3
#define DSIME_FLASH_TIMER_MS     900

// One sentence on its way to the document.
//
// Owned by _jobs (a std::list, and it must stay one: a vector reallocating on
// push_back would leave the in-flight job's address dangling under the worker
// thread). Destructor releases the COM references.
struct PendingJob {
    PendingJob() = default;
    ~PendingJob();
    PendingJob(const PendingJob&) = delete;
    PendingJob& operator=(const PendingJob&) = delete;

    // Phase 1 -> phase 2. The conversion is issued first; once its terminal
    // result lands, `converted` flips and `result` holds the Chinese.
    bool converted = false;
    // Text the user typed that needs no conversion. Three things arrive this
    // way, and all of them are born `converted`, so the pump hands them straight
    // to the inserter and never contacts the model:
    //
    //   * punctuation (and spaces, digits, other printables) typed with nothing
    //     in the buffer while sentences are still owed to the document;
    //   * every character typed in English mode;
    //   * the buffer itself, when Ctrl+Space interrupts it.
    //
    // Why they are queue jobs at all, rather than being written on the spot: a
    // queued sentence is inserted with ITfInsertAtSelection::InsertTextAtSelection,
    // which lands at *the selection as it stands when its turn comes* (the anchor
    // only decides where the composition is parked, and starting one sets the
    // selection there). Writing the comma immediately therefore moves the caret
    // past it, and the sentence arrives behind it: the user reads "，你好" and
    // blames the conversion. Taking a number and waiting is what makes the comma
    // land after the text it follows, and it matches what was meant anyway.
    //
    // Consecutive literal *characters* from English mode are merged into one job
    // rather than one job each — see _EnqueueIdleChar.
    bool literal = false;
    // The core dropped this request deliberately (teardown). No result, and no
    // pinyin fallback either — there is nowhere left to put it.
    bool cancelled = false;
    uint64_t requestId = 0;
    std::wstring result;
    int insertRetries = 0;

    // Captured when Space was pressed, not when the result arrives: focus is
    // allowed to move, and inserting at "whatever is selected now" would write
    // the sentence into an unrelated place.
    ITfContext* context = nullptr;      // AddRef'd
    ITfRange* anchor = nullptr;         // AddRef'd, collapsed, forward gravity
    ITfDocumentMgr* docMgr = nullptr;   // AddRef'd; identity check for focus

    std::string contextKey;             // conversation context to file under
    std::string pinyin;                 // snapshot of the buffer
};

class CTextService final : public ITfTextInputProcessorEx,
                           public ITfThreadMgrEventSink,
                           public ITfThreadFocusSink,
                           public ITfKeyEventSink,
                           public ITfCompositionSink,
                           public ITfTextLayoutSink {
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

    // ---- ITfTextLayoutSink ----
    STDMETHODIMP OnLayoutChange(ITfContext* pic, TfLayoutCode lcode, ITfContextView* pView) override;

    // ---- accessors used by the edit-session helpers ----
    ITfThreadMgr* ThreadMgr() const { return _pThreadMgr; }
    TfClientId    ClientId()  const { return _tid; }
    ITfComposition* Composition() const { return _pComposition; }
    dsime::Engine&  Engine()  { return _engine; }
    dsime::Session& CoreSession() { return _session; }
    // TRUE while we own the keystroke stream. Read by the caret probe to decide
    // where the floating box belongs.
    bool Composing() const { return _composing; }

private:
    ~CTextService();

    // ---- sink (un)advise helpers, implemented in TextService.cpp ----
    BOOL _InitThreadMgrEventSink();
    void _UninitThreadMgrEventSink();
    BOOL _InitKeyEventSink();
    void _UninitKeyEventSink();
    BOOL _InitThreadFocusSink();
    void _UninitThreadFocusSink();
    BOOL _InitLanguageBar();
    void _UninitLanguageBar();
    // Per-context layout sink: advised on the focused context, unadvised when it
    // stops being focused. There is no thread-wide equivalent.
    void _UnadviseLayoutSink();
    void _AdviseLayoutSink(ITfContext* pic);

    // ---- the hidden marshaling window (TextService.cpp) ----
    BOOL _CreateMessageWindow();
    void _DestroyMessageWindow();
    static LRESULT CALLBACK _MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // ---- key handling (KeyEventSink.cpp) ----
    BOOL _IsKeyEaten(ITfContext* pic, WPARAM wParam, LPARAM lParam);
    HRESULT _HandleKey(ITfContext* pic, WPARAM wParam, LPARAM lParam, BOOL* pfEaten);

    // ---- composition / edit sessions (Composition.cpp, EditSessions.cpp) ----
    HRESULT _StartComposition(ITfContext* pic);
    HRESULT _CommitComposition(ITfContext* pic, const std::wstring& text);
    HRESULT _EndComposition(ITfContext* pic);
    // Throw away what is being typed without touching the document or the queue.
    void    _ClearTyping();
    // Same, but for focus callbacks: drops the composition without an edit
    // session, which is not safe to start from there.
    void    _AbandonTyping();
    // A queued sentence landed in `pic`, moving the document out from under a
    // live (zero-width) composition. Replace it so the next Space still captures
    // a caret in the right place.
    void    _ReanchorIfComposing(ITfContext* pic);

    // ---- Chinese/English mode (KeyEventSink.cpp) -----------------------------
    // Flip the mode, flushing any buffer first (verbatim — the user asked to
    // stop typing pinyin, not to have it converted), and flash the box so the
    // new mode is visible even with nothing else to show.
    void    _ToggleEnglishMode(ITfContext* pic);
    // Hand the buffer to the queue as a literal job: the same four steps as
    // _EnqueueConversion, minus the model. A failure to capture the anchor leaves
    // the buffer alone and sets _flushBufferAsLiteralDue, so nothing is lost and
    // nothing is written out of turn.
    void    _FlushBufferAsLiteral(ITfContext* pic);

    // ---- the floating input box ---------------------------------------------
    // `canProbeCaret` gates the synchronous caret probe. It MUST be false on the
    // paths that run from a TSF callback — a focus change, a composition torn
    // down under us — because those can run with the document locked, where a
    // synchronous edit session is at best refused and at worst deadlocks. Key
    // handling and the posted messages (the layout timer, WM_DSIME_RELOCATE) are
    // the callers allowed to ask.
    void _UpdateInputBox(bool canProbeCaret = true);
    // The context whose caret the box follows, AddRef'd (caller releases).
    ITfContext* _CaretContext();
    // {exe}|{window class} of the window owning the focused document, UTF-8.
    std::string _ContextKeyForFocus();

    // ---- the conversion queue -----------------------------------------------
    // Mint a job around `pic`'s current caret and push it. `literal` is text
    // that needs no conversion (empty for a normal conversion job), in which
    // case `pinyin` is ignored. False when there is no caret to anchor to, in
    // which case nothing was queued and the caller still owns its text.
    bool _EnqueueJob(ITfContext* pic, std::string pinyin, std::wstring literal);
    // Idle, with sentences still owed to this document: give `text` — a
    // punctuation mark, a space, a digit, anything the host would have typed —
    // a place in the queue behind them instead of letting it land in front.
    // False when it does not apply (nothing is queued, or there is no caret to
    // anchor to), in which case the caller keeps its normal handling of the key.
    bool _EnqueueIdleChar(ITfContext* pic, const std::wstring& text);
    // Snapshot the buffer and the caret into a job and hand it to the queue.
    // False when the anchor could not be captured (the sentence stays in the box
    // to be retried or edited).
    bool _EnqueueConversion(ITfContext* pic);
    // Jobs in the queue that will actually ask the model something. This, not
    // _jobs.size(), is what the box's 待转换 badge counts.
    size_t _PendingConversions() const;
    // Take the head of the queue as far as it will go: issue its conversion, or
    // insert its result, or drop it.
    void _PumpQueue();
    // STA thread: hand one job to the core. The terminal callback balances the
    // AddRef taken here. False when nothing was dispatched (no engine, or the
    // core refused the buffer), in which case no callback is coming.
    bool _StartConversion(PendingJob* job);
    // STA thread: the job has its Chinese (or has failed). Insert it. Returns
    // false when the job was NOT delivered and should stay at the head of the
    // queue — only the TF_E_LOCKED retry path does that.
    bool _FinishJob(PendingJob* job);
    // The sentence could not be written. Put it on the clipboard, say so in the
    // box, and leave the HRESULT in the error log rather than dropping it on the
    // floor.
    void _LoseText(const std::wstring& text, HRESULT hr);
    // Drop queued sentences whose document is going away. Exactly one of `pdim`
    // (document torn down) or `pic` (context popped) is meaningful per caller.
    void _ReleaseJobsForDocument(ITfDocumentMgr* pdim, ITfContext* pic);

    static void _ConvertCallbackThunk(void* user_data, uint64_t request_id,
                                      int32_t status, const char* text_utf8);
    void _OnConvertResultOnStaThread(uint64_t request_id, int32_t status,
                                     const std::wstring& text);

private:
    LONG _cRef = 1;  // born referenced (class factory does the first AddRef)

    // TSF wiring, valid between Activate and Deactivate.
    ITfThreadMgr* _pThreadMgr = nullptr;
    TfClientId    _tid = TF_CLIENTID_NULL;

    DWORD _dwThreadMgrEventSinkCookie = TF_INVALID_COOKIE;
    DWORD _dwThreadFocusSinkCookie    = TF_INVALID_COOKIE;
    DWORD _dwLayoutSinkCookie         = TF_INVALID_COOKIE;
    ITfContext* _pLayoutSinkContext   = nullptr;  // AddRef'd

    // The active composition, non-null only while composing. Held with a ref.
    ITfComposition* _pComposition = nullptr;
    // Context that owns the active composition; we keep a ref so async edit
    // sessions target the right document even if focus has wandered.
    ITfContext* _pCompositionContext = nullptr;

    // Language-bar button (Settings launcher).
    ITfLangBarItemButton* _pLangBarButton = nullptr;

    // Core engine + session (RAII). Engine outlives session by member order:
    // _engine is declared before _session so it is destroyed *after* it.
    dsime::Engine  _engine;
    dsime::Session _session;

    // Raw ASCII typed so far: lower-case pinyin letters, the apostrophe syllable
    // separator, and the punctuation we keep in the buffer (the model renders it
    // full-width). This never reaches the document — it is drawn in the box.
    //
    // Its meaning narrowed when word selection arrived: it is now only the
    // **unselected tail**. Words the user has already chosen have been moved into
    // `_chosen` and their pinyin deleted from here, which is why the segmenter is
    // only ever asked about a buffer that starts at a word boundary.
    std::string _pinyin;

    // ---- word selection (KeyEventSink.cpp, Composition.cpp) ------------------
    //
    // A word the user picked from the dictionary, in the order they picked it.
    // These are NOT committed to the document: they accumulate here, and Space
    // sends `chosen text + remaining pinyin` as one request. Selecting everything
    // therefore costs no model call at all — the payload is already Chinese.
    struct ChosenWord {
        // The exact bytes the choice removed from `_pinyin` (letters only, no
        // syllable separators), so Backspace restores the buffer verbatim.
        std::string pinyin;
        // What the user picked, preceded by any bytes in the buffer that stood in
        // front of the word's segment. Those bytes are not part of the word but
        // they are part of the sentence and they were typed *there*, so the
        // payload concatenates them in this position rather than dropping them.
        std::string text;
    };
    std::vector<ChosenWord> _chosen;

    // Live segmentation of `_pinyin`, and the candidate list for the segment the
    // user can act on (the first one). Both are recomputed on every keystroke by
    // _Resegment, which is the only writer — the key handler and the box only
    // read them, so there is never a second copy to drift.
    dsime::SegResult _segs;
    std::vector<std::string> _cands;   // candidate words, in order; index 0 is label 2
    size_t _activeStart = 0;            // span of that segment in _pinyin, from ds_seg_start
    size_t _activeLen = 0;              // one past its last byte — i.e. how much to consume

    // Recompute `_segs` / `_cands` / the active span from `_pinyin`. Cheap and
    // synchronous (the dictionary is mmap'd); called after anything that changes
    // the buffer. Also called when there is no dictionary, where it just clears
    // the candidate list.
    void _Resegment();
    // Whether virtual key `vk` names a candidate that is actually on screen.
    // Both key phases ask this, because a test phase that disagrees with the
    // handler is how the host ends up typing a digit the IME meant to consume.
    bool _HasCandidateFor(WPARAM vk) const;
    // The payload a Space should send: chosen words followed by the raw tail.
    // Never displayed — the model sees the concatenation, the user sees
    // _PreEditText.
    std::string _ConversionPayload() const;
    // What the floating box's first line shows: the same words, with the pinyin
    // half broken into syllables along the segmentation the candidates came from.
    std::string _PreEditText() const;
    // Pick candidate `n` (0-based) of the active segment. False when there is no
    // active segment or no such candidate — the caller then falls through to
    // whatever the key would otherwise do.
    bool _SelectCandidate(size_t n);
    // Undo the most recent choice, putting its pinyin back. False if there is none.
    bool _UndoLastChoice();
    // Point the core at dsime.lex, once per process. Failure is not fatal: the IME
    // simply runs without candidates.
    void _InitLexicon();

    // TRUE while we own the keystroke stream: the buffer is being built and the
    // zero-width composition anchoring it is live. This — NOT the presence of
    // _pComposition — is what the key handler branches on. The two can diverge
    // (TSF may terminate the composition out from under us), and if they do,
    // treating a stale handle as "we are composing" would have Backspace delete
    // document text while the box still shows pinyin.
    bool _composing = false;

    // English mode: nothing typed is pinyin, nothing is remapped, and there is no
    // buffer — see the INPUT MODEL note above. Per activation (TSF gives us one
    // CTextService per thread), not persisted, and not reset by focus changes:
    // it is a mode the user chose, not a property of a document.
    bool _englishMode = false;

    // The mode notice is on screen because Ctrl+Space put it there, and the box
    // should show itself even with an empty buffer until the flash timer takes it
    // away. Without this, switching mode with nothing queued would give no
    // feedback at all; with it left on, a panel would sit over the document for
    // as long as English mode lasted.
    bool _modeFlash = false;

    // Ctrl+Space found a buffer it could not hand to the queue — the anchor could
    // not be captured, which happens when the document refuses a synchronous edit
    // session. The flush is retried at the top of the next keystroke, where one
    // is available; until then the buffer stays in the box and Space still
    // converts it, so nothing is stranded.
    bool _flushBufferAsLiteralDue = false;

    // Tick of the last mode change. Ctrl+Space can arrive down both routes — the
    // preserved key and the key sink — and two toggles in a row are
    // indistinguishable from the switch not working at all, so a repeat inside
    // 200 ms is dropped.
    ULONGLONG _lastToggleTick = 0;

    // Set while we replace a composition ourselves, so the termination sink does
    // not mistake it for the app yanking our composition away and drop the
    // buffer the user is still typing.
    bool _suppressTermination = false;

    // The queue. Strictly serial: only the head is ever in flight.
    std::list<std::unique_ptr<PendingJob>> _jobs;
    uint64_t _activeJobId = 0;  // request id of the in-flight conversion, else 0
    // Configured depth (config `queue_max_pending`); 0 means unbounded. Read at
    // Activate and refreshed whenever the thread regains focus, which is when a
    // change made in Settings can first be noticed. Only ever read from the STA
    // thread, so no synchronisation is needed.
    size_t _maxPending = 0;

    // A queued sentence could not be written to its document. Its text is on the
    // clipboard; the box shows a red badge until the next successful write.
    bool _writeFailed = false;

    // Thread focus. While it is lost the box stays hidden (the caret it was
    // tracking belongs to an app the user has left) but the queue keeps running.
    bool _threadFocused = true;

    // The floating input box. Created in Activate, destroyed in Deactivate.
    DSPinyinIMEBoxWnd* _inputBox = nullptr;

    // A layout change asked for a reposition and the message is still in our
    // queue; coalesces a scrolling burst into one probe.
    bool _relocatePending = false;

    // A queued sentence landed in the document while the user was already typing
    // the next one, leaving the live composition anchored at a caret that has
    // since moved. Replacing the composition needs a synchronous edit session,
    // and TSF only grants those from a key event or a TSF callback — never from
    // the posted message that delivered the result. So the queue raises this flag
    // and the next keystroke does the work, before that key is processed.
    bool _reanchorDue = false;

    // Hidden message-only window for cross-thread marshaling; STA-thread-owned.
    HWND _msgWnd = nullptr;
};

// Heap payload posted from the worker thread to the STA thread. Owned by the
// receiver (the window proc deletes it).
struct ConvertResult {
    CTextService* pThis;       // borrowed; valid because we hold a ref while
                               // a request is in flight (see _StartConversion)
    uint64_t      request_id;
    int32_t       status;
    std::wstring  text;        // already converted to UTF-16
};

// ---- edit-session helpers shared across the .cpp files ---------------------

// Insert `text` at `anchor`. `moveCaret` places the selection after the inserted
// run — only correct when the user is still typing into that document, hence the
// caller's decision. Returns the session HRESULT (TF_E_LOCKED when the document
// is mid-edit and the caller should retry).
HRESULT Dsime_RequestInsertText(CTextService* pSvc, ITfContext* pic, TfClientId tid,
                                ITfRange* anchor, const std::wstring& text,
                                BOOL moveCaret);

// A collapsed, forward-gravity clone of the caret, for a queued sentence to be
// inserted at later. False when there is no selection to capture.
bool Dsime_CaptureInsertAnchor(CTextService* pSvc, ITfContext* pic,
                               ITfRange** ppAnchor);

// Screen-space caret rect for `pic`, plus the HWND hosting it (owner and DPI
// source for the floating box). False when no position can be determined at all
// — in which case the caller keeps the box where it is rather than moving it.
bool Dsime_GetCaretPos(CTextService* pSvc, ITfContext* pic, HWND* pHost, RECT* pCaret);

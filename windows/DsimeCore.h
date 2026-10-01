// DsimeCore.h — thin, header-only C++ RAII wrapper over the dsime C ABI.
//
// The C contract (core/include/dsime.h) is authoritative; this wrapper adds no
// behaviour, only ownership/lifetime safety and UTF-8<->UTF-16 helpers. We use
// the EXACT symbol names and signatures declared in dsime.h — see that header
// for documented semantics, threading rules, and who-frees-what.
//
// Ownership model the TSF service uses (per the team-lead brief):
//   * ONE DsEngine for the whole text-service activation (created in
//     CTextService::Activate, freed in Deactivate). Internally synchronized, so
//     it can be shared by sessions and read from the Settings UI.
//   * ONE DsSession per activation (the IME tracks a single composition at a
//     time). Calls into a DsSession must be serialized onto the TSF UI/STA
//     thread; the only thing that touches it off-thread is the *core*, never us.
//
// All strings crossing the boundary are UTF-8. TSF speaks UTF-16, so convert at
// the edge with the Utf8/Utf16 helpers below.

#pragma once

#include <windows.h>
#include <string>
#include <utility>

// The authoritative C ABI. Relative include so the windows/ tree is
// self-contained against the in-repo core header.
extern "C" {
#include "../core/include/dsime.h"
}

namespace dsime {

// ---- UTF conversion helpers ----------------------------------------------

// UTF-8 (from core) -> UTF-16 (for TSF / Win32). Returns empty string on bad
// input rather than throwing; the IME must never crash the host.
inline std::wstring Utf8ToUtf16(const char* utf8) {
    if (utf8 == nullptr || *utf8 == '\0') return std::wstring();
    int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (needed <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');  // -1 drops the NUL
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), needed);
    return out;
}

inline std::wstring Utf8ToUtf16(const std::string& utf8) {
    return Utf8ToUtf16(utf8.c_str());
}

// UTF-16 (TSF / Win32) -> UTF-8 (for core).
inline std::string Utf16ToUtf8(const wchar_t* utf16) {
    if (utf16 == nullptr || *utf16 == L'\0') return std::string();
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, utf16, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::string Utf16ToUtf8(const std::wstring& utf16) {
    return Utf16ToUtf8(utf16.c_str());
}

// ---- RAII guard for a "caller frees" char* returned by the core ----------

// Wraps a char* that dsime.h documents as caller-owned and releases it with
// ds_string_free. Move-only.
class CoreString {
public:
    CoreString() = default;
    explicit CoreString(char* owned) : p_(owned) {}
    ~CoreString() { reset(); }

    CoreString(const CoreString&) = delete;
    CoreString& operator=(const CoreString&) = delete;

    CoreString(CoreString&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    CoreString& operator=(CoreString&& other) noexcept {
        if (this != &other) { reset(); p_ = other.p_; other.p_ = nullptr; }
        return *this;
    }

    const char* c_str() const { return p_ ? p_ : ""; }
    bool empty() const { return p_ == nullptr || *p_ == '\0'; }
    std::wstring to_wstring() const { return Utf8ToUtf16(c_str()); }
    std::string  to_string()  const { return std::string(c_str()); }

    void reset() {
        if (p_) { ds_string_free(p_); p_ = nullptr; }
    }

private:
    char* p_ = nullptr;
};

// ---- Engine wrapper -------------------------------------------------------

// Owns a DsEngine*. Created once per text-service activation.
class Engine {
public:
    Engine() = default;
    ~Engine() { reset(); }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&& o) noexcept : e_(o.e_) { o.e_ = nullptr; }
    Engine& operator=(Engine&& o) noexcept {
        if (this != &o) { reset(); e_ = o.e_; o.e_ = nullptr; }
        return *this;
    }

    // config_path == nullptr uses the per-user default
    // (%APPDATA%/DSPinyinIME/DSPinyinIME/config/config.json). Returns false on
    // fatal init error; call LastError() for a message.
    bool Create(const char* config_path = nullptr) {
        reset();
        e_ = ds_engine_new(config_path);
        return e_ != nullptr;
    }

    bool valid() const { return e_ != nullptr; }
    DsEngine* raw() const { return e_; }

    CoreString GetConfigJson() const {
        return CoreString(e_ ? ds_engine_get_config_json(e_) : nullptr);
    }
    int32_t SetConfigJson(const char* json_utf8) {
        return e_ ? ds_engine_set_config_json(e_, json_utf8) : DS_ERR_CONFIG;
    }
    int32_t ReloadConfig() {
        return e_ ? ds_engine_reload_config(e_) : DS_ERR_CONFIG;
    }
    CoreString ConfigPath() const {
        return CoreString(e_ ? ds_engine_config_path(e_) : nullptr);
    }
    // Configured queue depth (config `queue_max_pending`). The queue is the
    // frontend's, so this is the one number it needs from the core to enforce
    // the bound. 0 means "no bound".
    uint32_t QueueMaxPending() const {
        return e_ ? ds_engine_queue_max_pending(e_) : 0;
    }
    // Forget every remembered window's conversation context. It lives in memory
    // only, so this forgets what the running engine holds and deletes whatever an
    // older build wrote beside the config file.
    int32_t ClearContexts() { return e_ ? ds_engine_clear_contexts(e_) : DS_ERR_CONFIG; }

    void reset() {
        if (e_) { ds_engine_free(e_); e_ = nullptr; }
    }

private:
    DsEngine* e_ = nullptr;
};

// ---- Session wrapper ------------------------------------------------------

// Owns a DsSession*. One per activation. Not internally synchronized: every
// method here must be called from the TSF UI/STA thread.
class Session {
public:
    Session() = default;
    ~Session() { reset(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // The engine must outlive the session (the core bumps the engine refcount,
    // but keep the C++ Engine object alive too for clean teardown order).
    bool Create(const Engine& engine) {
        reset();
        if (!engine.valid()) return false;
        s_ = ds_session_new(engine.raw());
        return s_ != nullptr;
    }

    bool valid() const { return s_ != nullptr; }
    DsSession* raw() const { return s_; }

    // Replace the raw pinyin buffer with the whole string typed so far.
    void SetInput(const std::string& pinyin_utf8) {
        if (s_) ds_session_set_input(s_, pinyin_utf8.c_str());
    }

    CoreString GetInput() const {
        return CoreString(s_ ? ds_session_get_input(s_) : nullptr);
    }

    // Name the window this session is typing into, e.g. "notepad3.exe|4242": the
    // executable plus its process id. The conversation context is filed under
    // this key, so the model keeps seeing the domain and terminology of what is
    // already written there and starts clean anywhere else. Set it before each
    // conversion; the core copies the string, and holds the context in memory for
    // the life of the engine. Empty (the default) disables context for the
    // request — which is what to send when the window cannot be identified.
    void SetContextKey(const std::string& key_utf8) {
        if (s_) ds_session_set_context_key(s_, key_utf8.c_str());
    }

    // Kick off async conversion. callback fires on a CORE WORKER THREAD; it must
    // marshal to the UI thread before touching composition state. Returns a
    // monotonic request id, or 0 if the buffer is empty (no callback then).
    uint64_t Convert(DsConvertCallback callback, void* user_data) {
        return s_ ? ds_session_convert(s_, callback, user_data) : 0;
    }

    // Like Convert, but streams: callback fires with is_final=0 for each partial
    // (cumulative) update, then exactly once with is_final=1 for the terminal
    // outcome. Honors the `stream` config flag. Returns a request id, or 0 if the
    // buffer is empty (then no callback fires).
    uint64_t ConvertStream(DsStreamCallback callback, void* user_data) {
        return s_ ? ds_session_convert_stream(s_, callback, user_data) : 0;
    }

    // Move to another already-fetched candidate for the current input
    // (direction > 0 = next, < 0 = previous). Empty when there is none in that
    // direction — going down, the caller then Regenerate()s. Synchronous (cache
    // only, no network); the remote conversion always supersedes the n-gram guess
    // so this only ever cycles LLM outputs.
    CoreString CandidateCached(int32_t direction) const {
        return CoreString(s_ ? ds_session_candidate_cached(s_, direction) : nullptr);
    }

    // Ask the provider for a DIFFERENT conversion of the current buffer, avoiding
    // every candidate already shown, and append it. Same streaming contract as
    // ConvertStream. Returns a request id, or 0 if the buffer is empty.
    uint64_t Regenerate(DsStreamCallback callback, void* user_data) {
        return s_ ? ds_session_regenerate(s_, callback, user_data) : 0;
    }

    void Cancel() { if (s_) ds_session_cancel(s_); }
    void Reset()  { if (s_) ds_session_reset(s_); }

    void reset() {
        if (s_) { ds_session_free(s_); s_ = nullptr; }
    }

private:
    DsSession* s_ = nullptr;
};

// ---- Lexicon -----------------------------------------------------------------
//
// Segmentation and candidate lookup, mirroring the stateless `ds_lexicon_*` C
// entry points. It is *not* part of Session and holds no conversion state: these
// calls start no request and know nothing about the queue, so they can run on
// every keystroke. Which words the user has already picked is the frontend's
// business (see CTextService::_chosen), and the core only ever sees the
// unselected tail.

// One buffer's segmentation, owned. Freed on scope exit.
class SegResult {
public:
    SegResult() = default;
    ~SegResult() { Free(); }

    SegResult(const SegResult&) = delete;
    SegResult& operator=(const SegResult&) = delete;
    SegResult(SegResult&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }
    SegResult& operator=(SegResult&& o) noexcept {
        if (this != &o) { Free(); r_ = o.r_; o.r_ = nullptr; }
        return *this;
    }

    // Segment `pinyin_utf8`. Returns false only on a NULL argument; a missing
    // dictionary still succeeds, with one opaque segment and no candidates.
    bool Segment(const std::string& pinyin_utf8) {
        Free();
        return ds_lexicon_segment(pinyin_utf8.c_str(), &r_) == 0 && r_ != nullptr;
    }

    void Free() {
        if (r_) { ds_lexicon_free(r_); r_ = nullptr; }
    }

    bool valid() const { return r_ != nullptr; }
    int32_t count() const { return r_ ? ds_seg_count(r_) : 0; }

    bool HasWord(int32_t i) const { return r_ && ds_seg_has_word(r_, i) != 0; }
    std::string Pinyin(int32_t i) const { return CStr(ds_seg_pinyin(r_, i)); }
    std::string Best(int32_t i) const { return CStr(ds_seg_best(r_, i)); }

    // Byte range of segment `i` in the string that was passed to Segment().
    //
    // This is NOT the length of Pinyin(i): a code is space-separated syllables
    // ("mei you", 8 bytes) where the input is the letters the user typed
    // ("meiyou", 6). Every use of a segment as a range into a caller-owned
    // string has to go through these two, never the code.
    int32_t Start(int32_t i) const { return r_ ? ds_seg_start(r_, i) : -1; }
    int32_t End(int32_t i) const { return r_ ? ds_seg_end(r_, i) : -1; }

    int32_t CandCount(int32_t i) const { return r_ ? ds_seg_cand_count(r_, i) : 0; }

    // Candidate `n` (0-based) of segment `i`. The UI labels these from 2.
    std::string Cand(int32_t i, int32_t n) const { return CStr(ds_seg_cand(r_, i, n)); }

private:
    static std::string CStr(const char* p) {
        return p ? std::string(p) : std::string();
    }
    DsSegResult* r_ = nullptr;
};

// Whether a dictionary was found. False means the IME runs without candidates,
// which is a supported state, not an error.
inline bool LexiconAvailable() { return ds_lexicon_available() != 0; }

// Tell the core where dsime.lex is. Once per process, before the first query;
// a call after the dictionary is mapped is refused. The lexicon ships beside
// dsime.dll (regsvr32 records the exact path the DLL registered from), so the
// frontend passes that directory — see CTextService::_InitLexicon.
inline bool LexiconSetPath(const std::string& path_utf8) {
    return ds_lexicon_set_path(path_utf8.c_str()) == 0;
}

// Thread-local last-error string from the core, as UTF-16.
inline std::wstring LastError() {
    return Utf8ToUtf16(ds_last_error());
}

}  // namespace dsime

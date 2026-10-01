/*
 * dsime.h — C ABI for the DS Pinyin IME core engine (Rust crate `dsime`).
 *
 * Stable interface shared by every platform frontend (Windows TSF today).
 * All strings are UTF-8, NUL-terminated. Pointers returned by the library that
 * are documented as "caller frees" MUST be released with ds_string_free().
 *
 * Threading: ds_session_convert() is non-blocking. The result callback is
 * invoked from a background worker thread; marshal to your UI thread before
 * touching composition state. Calls into a single DsSession from multiple
 * threads must be externally serialized. DsEngine is internally synchronized
 * and may be shared by many sessions.
 */
#ifndef DSIME_H
#define DSIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DsEngine DsEngine;
typedef struct DsSession DsSession;

/* status codes passed to DsConvertCallback */
#define DS_OK            0
#define DS_ERR_NETWORK   1
#define DS_ERR_AUTH      2
#define DS_ERR_API       3
#define DS_ERR_CANCELLED 4
#define DS_ERR_CONFIG    5
#define DS_ERR_INTERNAL  6

/*
 * Result callback.
 *   user_data : the pointer passed to ds_session_convert.
 *   request_id: the id returned by ds_session_convert.
 *   status    : DS_OK or a DS_ERR_* code.
 *   text_utf8 : on DS_OK, the converted Chinese sentence; on error, a human
 *               readable message (may be NULL). Valid ONLY during the call —
 *               copy it if you need it later.
 * The callback runs on a worker thread. It must not call back into the same
 * DsSession synchronously; hop to your UI thread first.
 */
typedef void (*DsConvertCallback)(void *user_data,
                                  uint64_t request_id,
                                  int32_t status,
                                  const char *text_utf8);

/*
 * Streaming result callback (see ds_session_convert_stream).
 *   is_final == 0 : a PARTIAL update. status is DS_OK and text_utf8 is the
 *                   CUMULATIVE Chinese text so far — replace your pre-edit with
 *                   it. Fires zero or more times. Do NOT release per-request
 *                   resources here.
 *   is_final == 1 : the TERMINAL outcome. On DS_OK, text_utf8 is the final
 *                   (sanitized) sentence; on a DS_ERR_* status it is a message.
 *                   Fires EXACTLY ONCE and is always the last call for a given
 *                   request_id — release per-request resources here.
 * text_utf8 is valid ONLY during the call; copy it if needed. Runs on a worker
 * thread — hop to your UI thread before touching composition state.
 */
typedef void (*DsStreamCallback)(void *user_data,
                                 uint64_t request_id,
                                 int32_t status,
                                 int32_t is_final,
                                 const char *text_utf8);

/* ---- Engine lifecycle ---------------------------------------------------- */

/* Create an engine. config_path may be NULL to use the per-user default path.
 * Returns NULL on fatal error (see ds_last_error). */
DsEngine *ds_engine_new(const char *config_path);
void      ds_engine_free(DsEngine *engine);

/* Re-read the config file from disk. Returns DS_OK or DS_ERR_CONFIG. */
int32_t   ds_engine_reload_config(DsEngine *engine);

/* Current configuration as a JSON object string. Caller frees. */
char     *ds_engine_get_config_json(DsEngine *engine);

/* Replace configuration from a JSON object string and persist it to disk.
 * Returns DS_OK or DS_ERR_CONFIG (see ds_last_error for details). */
int32_t   ds_engine_set_config_json(DsEngine *engine, const char *json_utf8);

/* The configured config file path (caller frees). */
char     *ds_engine_config_path(DsEngine *engine);

/* Forget every remembered input window's conversation context. Returns DS_OK or
 * DS_ERR_CONFIG.
 *
 * The context is a record of what the user has typed, so callers need a way to
 * be rid of it that does not mean hunting for files. Contexts live in memory
 * only; this also deletes whatever a version that did write them to disk left
 * beside the config file. */
int32_t   ds_engine_clear_contexts(DsEngine *engine);

/* How many conversions the frontend should let pile up before it stops
 * accepting more (config `queue_max_pending`). Returns 0 for a NULL engine,
 * which the caller should read as "no bound" rather than "reject everything".
 *
 * The queue itself lives in the frontend — only it can decide on its UI thread
 * whether to take another sentence — so this is the one number it needs from
 * the core to enforce the configured depth. */
uint32_t  ds_engine_queue_max_pending(DsEngine *engine);

/* ---- Session lifecycle --------------------------------------------------- */

DsSession *ds_session_new(DsEngine *engine);
void       ds_session_free(DsSession *session);

/* Replace the raw pinyin (ASCII) buffer with the full string typed so far. */
void       ds_session_set_input(DsSession *session, const char *pinyin_ascii);

/* The current raw pinyin buffer (caller frees). Never NULL. */
char      *ds_session_get_input(DsSession *session);

/* Name the window this session is typing into, e.g. "notepad3.exe|4242" — the
 * executable plus its process id, which is as close to "this document" as a text
 * service can get without reading the title. The conversation context is filed
 * under this key, so the model keeps seeing the domain, terminology and wording
 * of what is already written there, and starts clean anywhere else. Set it
 * before each conversion: one session outlives any single document.
 *
 * Contexts are held in memory for the life of the engine, so they are lost when
 * the frontend drops it — a restart, or switching to another input method and
 * back. An empty key (or never calling this) disables context for that request,
 * which is what a frontend that cannot identify its window should send.
 * The string is copied — the caller keeps ownership. */
void       ds_session_set_context_key(DsSession *session, const char *key_utf8);

/* Kick off async conversion of the current buffer. Cancels any previous
 * in-flight request for this session. Returns a monotonic request id, or 0 if
 * the buffer is empty (callback is not invoked in that case).
 *
 * EXACTLY-ONCE: for every call that returns a non-zero id, `callback` is
 * invoked exactly once, on a worker thread. If a newer request (or a cancel /
 * reset) supersedes this one before it finishes, the callback still fires, with
 * status DS_ERR_CANCELLED. This lets the frontend safely tie per-request
 * resources (e.g. a retained context pointer) to the callback.
 *
 * One conversion may issue TWO requests to the provider: a reasoning model that
 * spends its whole token budget thinking returns empty content, and that is
 * retried once with thinking switched off. Nothing about the contract above
 * changes, but a frontend sizing its own patience against the configured
 * timeout should allow for two of them.
 *
 * ds_engine_free() honours this too: it cancels outstanding work and waits
 * (bounded) for the pending callbacks to land before tearing the engine down.
 * The wait is capped, so a frontend that cannot tolerate even that should
 * release its per-request resources without relying on the callback. */
uint64_t   ds_session_convert(DsSession *session,
                              DsConvertCallback callback,
                              void *user_data);

/* Like ds_session_convert, but streams the conversion: `callback` fires with
 * is_final=0 for each partial (cumulative) update as tokens arrive, then exactly
 * once with is_final=1 for the terminal outcome. Lowers perceived latency by
 * filling the pre-edit incrementally. Same supersession semantics: a newer
 * request makes this one's terminal call status DS_ERR_CANCELLED, and stale
 * partials are suppressed. Honors the `stream` config flag (when false, no
 * partials fire — only the single terminal call). Returns a request id, or 0 if
 * the buffer is empty. Tie per-request resources to the is_final=1 call. */
uint64_t   ds_session_convert_stream(DsSession *session,
                                     DsStreamCallback callback,
                                     void *user_data);

/* ---- Candidate cycling (up/down) ---------------------------------------- */
/* The LLM conversion is authoritative. After it lands, the user can cycle
 * through alternative LLM conversions of the SAME input with up/down. */

/* Move to another already-fetched candidate for the current buffer and return it
 * (caller frees, never NULL). direction > 0 -> NEXT candidate; direction < 0 ->
 * PREVIOUS. Returns "" when there is none in that direction: up past the primary
 * conversion, or down past the last cached candidate — in the down case the
 * frontend should then call ds_session_regenerate to fetch a fresh one.
 * Synchronous and cheap (consults only the cache, never the network). The set is
 * replaced whenever a new conversion completes for changed input. */
char      *ds_session_candidate_cached(DsSession *session, int32_t direction);

/* Ask the provider for a DIFFERENT conversion of the current buffer, avoiding
 * every candidate already shown, and append it so ds_session_candidate_cached can
 * revisit it. Same streaming callback contract and supersession semantics as
 * ds_session_convert_stream; returns a request id, or 0 if the buffer is empty.
 * Call this when the user asks for another candidate (down) and the cache is
 * exhausted. */
uint64_t   ds_session_regenerate(DsSession *session,
                                 DsStreamCallback callback,
                                 void *user_data);

/* Cancel any in-flight request (its callback fires with DS_ERR_CANCELLED). */
void       ds_session_cancel(DsSession *session);

/* Clear the buffer and cancel in-flight work (call after commit / escape). */
void       ds_session_reset(DsSession *session);

/* ---- Utilities ----------------------------------------------------------- */

void        ds_string_free(char *s);
const char *ds_last_error(void); /* thread-local last error, never NULL */

/* SemVer of the core library (static string, do not free). */
const char *ds_version(void);

/* ---- Lexicon (segmentation + candidate lookup) ----------------------------
 *
 * A local pinyin dictionary, compiled ahead of time into `dsime.lex` (the `dslex`
 * tool, from rime-frost) and memory-mapped read-only on first use. It gives the
 * IME a candidate list while the user is still typing, so a short word does not
 * need a round trip to the model.
 *
 * These calls are STATELESS and independent of DsSession: they do not touch the
 * conversion queue, do not start a request, and are safe to call on every
 * keystroke. Nothing here is tied to the single-flight rule that governs
 * ds_session_convert — that rule exists because a request is in flight, and these
 * have none.
 *
 * Every one of them degrades rather than fails. If the dictionary is missing,
 * unreadable, or built by a different version, ds_lexicon_available() reports 0
 * and ds_lexicon_segment() returns a single opaque segment, which is exactly how
 * the IME behaved before this feature existed. Callers need no fallback path of
 * their own.
 *
 * Threading: the shared dictionary is opened under a one-time initialiser and is
 * read-only afterwards, so any thread may query it. A DsSegResult handle,
 * however, is owned by the caller that received it and is not thread-safe; in
 * practice it lives and dies on the STA thread within one key event. */

typedef struct DsSegResult DsSegResult;

/* Point the lexicon at its data file. Absolute path, UTF-8.
 *
 * Call once, during activation, BEFORE the first query — a call made after the
 * dictionary is already mapped returns DS_ERR_CONFIG and changes nothing rather
 * than silently appearing to take effect. The frontend passes the path next to
 * dsime.dll, since the DLL, the settings EXE and the lexicon must all be
 * co-located (regsvr32 records the exact path it registered from).
 *
 * If this is never called, the path is derived from the config file's directory,
 * which is what the CLI example and the tests rely on. */
int32_t    ds_lexicon_set_path(const char *utf8_path);

/* 1 if a dictionary was found and mapped, 0 if the IME must run without one. */
int32_t    ds_lexicon_available(void);

/* Segment a pinyin buffer. `pinyin_utf8` is the raw ASCII the user has typed that
 * has not been selected yet — digits, punctuation and capitals are all allowed
 * and come back as opaque segments that can never be selected.
 *
 * On success writes a handle to *out and returns DS_OK. `*out` is NULL on
 * failure. Free it with ds_lexicon_free. */
int32_t    ds_lexicon_segment(const char *pinyin_utf8, DsSegResult **out);

void       ds_lexicon_free(DsSegResult *result);

int32_t    ds_seg_count(const DsSegResult *result);

/* The segment's code — space-joined syllables, which is the key to pass back to
 * ds_lexicon_candidates. For a segment with no dictionary entry this is the raw
 * input slice instead, and ds_seg_has_word() is 0.
 *
 * THIS IS NOT THE SEGMENT'S LENGTH IN THE BUFFER. "meiyou" has the code "mei you"
 * (8 bytes) and the span "meiyou" (6 bytes): a code is what the dictionary is keyed
 * by, a span is what the user typed. To take a candidate you need the span, and
 * ds_seg_start/ds_seg_end are what give it to you. */
const char *ds_seg_pinyin(const DsSegResult *result, int32_t index);

/* Byte offsets of segment `index` within the string passed to
 * ds_lexicon_segment: [start, end). Together they are how much of the buffer a
 * selected word consumes, which is almost never strlen(code) — a multi-syllable
 * code carries separator spaces the input does not, and an apostrophe the user
 * typed is in the span but not in the code.
 *
 * Read them from the SAME result handle, and re-read them on every keystroke: the
 * segmentation is recomputed as the user types, and a length remembered from an
 * earlier one would slice the wrong bytes. */
int32_t    ds_seg_start(const DsSegResult *result, int32_t index);
int32_t    ds_seg_end(const DsSegResult *result, int32_t index);

/* 1 if the dictionary matched this segment, so it can be selected. */
int32_t    ds_seg_has_word(const DsSegResult *result, int32_t index);

/* Highest-weighted word for the segment — candidate #1 — or NULL. */
const char *ds_seg_best(const DsSegResult *result, int32_t index);

/* How many candidates this segment has, 0..8. A short code simply reports fewer;
 * the list is never padded. */
int32_t    ds_seg_cand_count(const DsSegResult *result, int32_t index);

/* Candidate `n` (0-based) of segment `index`, or NULL.
 *
 * The UI labels these from 2, not 1: candidate 0 is shown as `2`. There is no
 * label 1, and the digit key 1 never selects a candidate. */
const char *ds_seg_cand(const DsSegResult *result, int32_t index, int32_t n);

/* Candidates for a code, highest weight first, at most 8, as a NUL-separated,
 * double-NUL-terminated block. The caller frees it with ds_string_free. Returns
 * NULL if the code is unknown. Provided so a frontend can fetch a list without
 * holding a whole segmentation — useful when only the active segment is wanted. */
char      *ds_lexicon_candidates(const char *code_utf8);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSIME_H */

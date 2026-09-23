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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSIME_H */

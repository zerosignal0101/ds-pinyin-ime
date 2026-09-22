// Trace.h — opt-in diagnostic log for the TSF frontend.
//
// Why this exists: TSF behaviour cannot be unit-tested and cannot be observed
// from outside the host. When a sentence does not reach the document, the only
// evidence is that it did not — every internal decision (was the key eaten,
// which branch ran, did the edit session run synchronously or get deferred, what
// did the document say) is invisible. This writes one line per key decision and
// per edit session so a single reproduction answers the question instead of
// prompting another guess.
//
// Enabled by the *existence* of %TEMP%\dsinput-trace.on — a marker file rather
// than an environment variable, because the interesting processes (explorer, a
// browser, an editor) are long-lived and cannot be relaunched with a new
// environment on a whim. Drop the marker, reproduce, read the log.
//
// When the marker is absent this costs one cached GetFileAttributes call for the
// life of the process, so it is safe to leave compiled in.

#pragma once

#include <windows.h>

// printf-style, wide. Appends one line, tagged with the time and the calling
// thread id (so worker-thread and STA-thread interleaving is visible).
void DsimeTrace(const wchar_t* fmt, ...);

// False whenever the marker is absent. Callers that would do real work merely to
// build a trace message (reading the document back, say) must test this first —
// DsimeTrace can only skip the write, not the work that produced its arguments.
bool DsimeTraceEnabled();

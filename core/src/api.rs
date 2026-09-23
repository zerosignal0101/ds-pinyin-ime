//! OpenAI-compatible chat-completions client used to convert pinyin → Chinese.

use crate::config::Config;
use crate::context::WindowContext;
use serde::{Deserialize, Serialize};
use std::borrow::Cow;
use std::time::Duration;

/// Wraps the compacted context note so the model can tell it apart from the
/// sentence it is being asked to convert. A constant, so the message stays
/// byte-stable from one request to the next — the cache matches whole prefix
/// units, and any edit to an earlier message invalidates everything after it.
const CONTEXT_SUMMARY_LEAD: &str =
    "Context from earlier in this same input window. Use it only to disambiguate \
     terminology, names and style; it is not text to continue or repeat:";

/// Error categories mirrored to the C `DS_ERR_*` status codes.
#[derive(Debug)]
pub enum ConvertError {
    Network(String),
    Auth(String),
    Api(String),
    /// The provider answered — and the answer had no sentence in it.
    ///
    /// This is its own category, rather than an [`ConvertError::Api`], because it
    /// is the one failure a differently-shaped *retry* can fix: a reasoning model
    /// that spends its whole budget thinking returns empty content with
    /// `finish_reason: "length"`, which is a property of the input and the
    /// settings, not an outage. See [`convert`] for the retry. The status code is
    /// deliberately still 3: the C ABI, and every frontend's handling of it, is
    /// unchanged.
    EmptyCompletion {
        message: String,
        /// What the provider said about why, passed through so the retry can
        /// tell "the budget ran out" from "the model chose to say nothing".
        finish_reason: Option<String>,
    },
    Cancelled,
    Config(String),
}

impl ConvertError {
    /// Map to the integer status code exposed across the FFI boundary.
    pub fn status_code(&self) -> i32 {
        match self {
            ConvertError::Network(_) => 1,             // DS_ERR_NETWORK
            ConvertError::Auth(_) => 2,                // DS_ERR_AUTH
            ConvertError::Api(_) => 3,                 // DS_ERR_API
            ConvertError::EmptyCompletion { .. } => 3, // DS_ERR_API
            ConvertError::Cancelled => 4,              // DS_ERR_CANCELLED
            ConvertError::Config(_) => 5,              // DS_ERR_CONFIG
        }
    }

    pub fn message(&self) -> String {
        match self {
            ConvertError::Network(m) => format!("network error: {m}"),
            ConvertError::Auth(m) => format!("auth error: {m}"),
            ConvertError::Api(m) => format!("api error: {m}"),
            // Same prefix as `Api`: the string a frontend's Test button shows for
            // this case has to stay what it always was.
            ConvertError::EmptyCompletion { message, .. } => format!("api error: {message}"),
            ConvertError::Cancelled => "cancelled".to_string(),
            ConvertError::Config(m) => format!("config error: {m}"),
        }
    }

    /// True when the provider stopped because it ran out of completion tokens —
    /// the reasoning-model failure the rescue retry exists for. A completion that
    /// came back empty for any other stated reason is left to the ordinary
    /// raw-pinyin fallback.
    fn budget_exhausted(&self) -> bool {
        matches!(
            self,
            ConvertError::EmptyCompletion {
                finish_reason: Some(reason),
                ..
            } if reason == "length"
        )
    }
}

#[derive(Serialize)]
struct ChatRequest<'a> {
    model: &'a str,
    messages: Vec<ChatMessage<'a>>,
    /// gpt-5 / o-series reject any non-default temperature, so we omit it for them.
    /// Providers that use a thinking mode also ignore it while thinking is on.
    #[serde(skip_serializing_if = "Option::is_none")]
    temperature: Option<f32>,
    /// Classic OpenAI-compatible token cap (DeepSeek et al.).
    #[serde(skip_serializing_if = "Option::is_none")]
    max_tokens: Option<u32>,
    /// gpt-5 / o-series replacement for `max_tokens`.
    #[serde(skip_serializing_if = "Option::is_none")]
    max_completion_tokens: Option<u32>,
    /// DeepSeek-style thinking switch (`{"thinking":{"type":"enabled"}}`).
    /// Omitted unless the config explicitly sets `thinking`.
    #[serde(skip_serializing_if = "Option::is_none")]
    thinking: Option<ThinkingRequest<'a>>,
    /// Reasoning-effort hint; omitted when the config leaves it empty.
    #[serde(skip_serializing_if = "Option::is_none")]
    reasoning_effort: Option<&'a str>,
    stream: bool,
}

/// The `{"type": …}` body of the `thinking` request field.
#[derive(Serialize)]
struct ThinkingRequest<'a> {
    #[serde(rename = "type")]
    kind: &'a str,
}

#[derive(Serialize)]
struct ChatMessage<'a> {
    role: &'a str,
    /// `Cow` so the one derived message (the context-summary lead-in) can be
    /// assembled on the fly while everything else stays borrowed.
    content: Cow<'a, str>,
}

/// Token accounting as the provider reports it. Anchors the context estimate on
/// a real count rather than a local guess — see `WindowContext::estimated_tokens`.
#[derive(Debug, Clone, Copy, Default, Deserialize)]
pub struct Usage {
    #[serde(default)]
    pub prompt_tokens: u32,
    #[serde(default)]
    pub completion_tokens: u32,
}

/// A finished conversion: the sanitized text plus what it cost.
#[derive(Debug, Clone)]
pub struct Completed {
    pub text: String,
    pub usage: Usage,
}

#[derive(Deserialize)]
struct ChatResponse {
    #[serde(default)]
    choices: Vec<ChatChoice>,
    #[serde(default)]
    usage: Usage,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: ChoiceMessage,
    /// `"length"` when the model ran out of budget — the case that comes back as
    /// empty content rather than an error.
    #[serde(default)]
    finish_reason: Option<String>,
}

#[derive(Deserialize)]
struct ChoiceMessage {
    #[serde(default)]
    content: String,
}

/// One SSE chunk of a streamed completion: `{"choices":[{"delta":{"content":"…"}}]}`.
/// Providers that report `usage` at the end of a stream are taken up on it; the
/// rest fall back to the token estimate.
#[derive(Deserialize)]
struct StreamChunk {
    #[serde(default)]
    choices: Vec<StreamChoice>,
    #[serde(default)]
    usage: Option<Usage>,
}

#[derive(Deserialize)]
struct StreamChoice {
    #[serde(default)]
    delta: StreamDelta,
    /// `"length"` when the budget ran out. The last chunk carries it, and it is
    /// the only thing that distinguishes an empty stream that got cut off from
    /// one where the model chose to say nothing — the difference between an
    /// actionable error message and a shrug.
    #[serde(default)]
    finish_reason: Option<String>,
}

#[derive(Deserialize, Default)]
struct StreamDelta {
    #[serde(default)]
    content: Option<String>,
}

#[derive(Deserialize)]
struct ApiErrorEnvelope {
    error: ApiErrorBody,
}

#[derive(Deserialize)]
struct ApiErrorBody {
    message: String,
}

/// When regenerating an alternative, instruct the model to avoid the conversions
/// the user has already rejected (`exclude`). Returns `None` for the normal path
/// (no exclusions) so the primary prompt stays untouched.
fn regen_instruction(exclude: &[String]) -> Option<String> {
    if exclude.is_empty() {
        return None;
    }
    let shown = exclude
        .iter()
        .map(|c| format!("- {c}"))
        .collect::<Vec<_>>()
        .join("\n");
    Some(format!(
        "These conversions of the same input were already shown and rejected:\n{shown}\n\
         Provide a DIFFERENT, equally natural whole-sentence conversion. It must not \
         equal any rejected one. Output only the alternative, no explanation."
    ))
}

/// System prompt, the window's context, then the pinyin — plus an optional
/// regeneration instruction.
///
/// Order is cost-relevant: the system prompt and the context summary are the
/// most stable part of the prefix, the append-only turn history follows, and the
/// volatile regeneration instruction goes last. A regeneration therefore never
/// invalidates the cacheable prefix.
fn build_messages<'a>(
    cfg: &'a Config,
    ctx: &'a WindowContext,
    pinyin: &'a str,
    regen: &'a Option<String>,
) -> Vec<ChatMessage<'a>> {
    let mut messages = Vec::with_capacity(4 + ctx.turns.len() * 2);
    messages.push(ChatMessage {
        role: "system",
        content: Cow::Borrowed(&cfg.system_prompt),
    });
    if let Some(summary) = &ctx.summary {
        messages.push(ChatMessage {
            role: "system",
            content: Cow::Owned(format!("{CONTEXT_SUMMARY_LEAD}\n{summary}")),
        });
    }
    for turn in &ctx.turns {
        messages.push(ChatMessage {
            role: "user",
            content: Cow::Borrowed(&turn.pinyin),
        });
        messages.push(ChatMessage {
            role: "assistant",
            content: Cow::Borrowed(&turn.chinese),
        });
    }
    messages.push(ChatMessage {
        role: "user",
        content: Cow::Borrowed(pinyin),
    });
    if let Some(instr) = regen {
        messages.push(ChatMessage {
            role: "system",
            content: Cow::Borrowed(instr),
        });
    }
    messages
}

/// Bump temperature when regenerating so the alternative actually differs;
/// the normal path keeps the configured (lower) temperature.
fn effective_temperature(cfg: &Config, exclude: &[String]) -> f32 {
    if exclude.is_empty() {
        cfg.temperature
    } else {
        cfg.temperature.max(0.8)
    }
}

/// OpenAI's gpt-5 and o-series (reasoning) models diverge from the classic
/// chat-completions schema: they reject `max_tokens` (require
/// `max_completion_tokens`) and reject any non-default `temperature`. Other
/// OpenAI-compatible providers (DeepSeek, etc.) still use the classic schema, so
/// we only switch for models whose id marks them as one of these families.
fn is_restricted_openai_model(model: &str) -> bool {
    let m = model.trim().to_ascii_lowercase();
    // Match family prefixes so future point releases (gpt-5.5, o3-mini, …) are
    // covered. A trailing boundary check avoids matching unrelated ids like
    // "o1ololo-custom" only when the prefix is the whole token start.
    const FAMILIES: &[&str] = &["gpt-5", "gpt-6", "o1", "o3", "o4"];
    FAMILIES.iter().any(|fam| {
        m == *fam
            || m.strip_prefix(fam)
                .is_some_and(|rest| rest.starts_with(['-', '.']))
    })
}

/// Build the (temperature, max_tokens, max_completion_tokens) triple for a
/// request, selecting the right token-cap field and dropping temperature for
/// restricted OpenAI models.
///
/// `thinking_off` is what *this request* will ask for, not what the config says
/// — the rescue attempt ([`convert`]) turns thinking off for one request, and a
/// second reader of the same config field is exactly how the two would drift
/// apart. Everything that decides a request field takes the same value.
fn token_params(
    cfg: &Config,
    exclude: &[String],
    thinking_off: bool,
) -> (Option<f32>, Option<u32>, Option<u32>) {
    if is_restricted_openai_model(&cfg.model) {
        // These models only accept the default temperature (1); sending the
        // configured (lower) value errors, so omit it entirely.
        (None, None, Some(cfg.max_tokens))
    } else {
        // A provider ignores temperature while its thinking mode is on (which is
        // the default for reasoning models), so only send it when thinking is
        // off for this request.
        let temperature = if thinking_off {
            Some(effective_temperature(cfg, exclude))
        } else {
            None
        };
        (temperature, Some(cfg.max_tokens), None)
    }
}

/// True when the config explicitly turns the provider's thinking mode off — the
/// only configuration in which `temperature` is honoured.
fn thinking_disabled(cfg: &Config) -> bool {
    cfg.thinking.trim().eq_ignore_ascii_case("disabled")
}

/// Split the config's thinking knobs into the two request fields. Both are
/// omitted when unset, so a custom OpenAI-compatible endpoint that rejects them
/// can be used by leaving the settings empty.
///
/// With `thinking_off` the request turns thinking off regardless of what the
/// config asked for, which is the rescue attempt's whole point. `reasoning_effort`
/// is passed through untouched even then: the measured rescue request keeps the
/// configured effort, and there is no reason to vary two knobs at once.
fn thinking_params(
    cfg: &Config,
    thinking_off: bool,
) -> (Option<ThinkingRequest<'_>>, Option<&str>) {
    let kind = match cfg.thinking.trim().to_ascii_lowercase().as_str() {
        "enabled" => Some("enabled"),
        "disabled" => Some("disabled"),
        _ => None,
    };
    let effort = match cfg.reasoning_effort.trim() {
        "" => None,
        e => Some(e),
    };
    let kind = if thinking_off { Some("disabled") } else { kind };
    (kind.map(|kind| ThinkingRequest { kind }), effort)
}

/// Send one conversion request. `client` is a shared, connection-pooled client.
/// `ctx` is the window's conversation context — pass `ContextSnapshot::default()`
/// for none. `exclude` lists already-shown conversions to avoid (empty for the
/// normal path; non-empty when regenerating an alternative).
///
/// **This may issue two requests.** A reasoning model can spend the entire
/// `max_tokens` budget on its hidden chain of thought and return empty content
/// with `finish_reason: "length"` — measured on hard unsegmented pinyin, and
/// repeatedly enough to cost a user sentences. That failure is not an outage and
/// repeating the same request does not fix it; asking for the same conversion
/// with thinking turned off does (measured: ~10s and empty, versus 0.6s and the
/// answer). So the one retry is shaped differently, and the second attempt reuses
/// the same context — the domain vocabulary in it is what makes the answer right
/// (`时域形式` rather than a plausible-looking wrong term).
///
/// The retry is deliberately narrow: only an empty completion that the provider
/// itself attributed to the budget, and never when this request already turned
/// thinking off (that would be the identical request twice). Everything else —
/// an outage, an auth failure, a model that simply said nothing — reports through
/// unchanged, and the frontend's raw-pinyin fallback covers it.
///
/// A frontend sizing its own patience against `timeout_ms` should allow for two
/// attempts; see `ds_session_convert` in `dsime.h`.
pub async fn convert(
    client: &reqwest::Client,
    cfg: &Config,
    ctx: &WindowContext,
    pinyin: &str,
    exclude: &[String],
) -> Result<Completed, ConvertError> {
    // One reader of the config, used both for the first attempt's request fields
    // and as the retry's guard: the guard means "a retry would send the identical
    // request", and that is only true if both come from this one value.
    let thinking_off = thinking_disabled(cfg);

    let first = convert_attempt(client, cfg, ctx, pinyin, exclude, thinking_off).await;
    let Err(first_err) = first else {
        return first;
    };
    if thinking_off || !first_err.budget_exhausted() {
        return Err(first_err);
    }

    match convert_attempt(client, cfg, ctx, pinyin, exclude, true).await {
        Ok(done) => Ok(done),
        // Both attempts empty: report both, so the one string a frontend shows
        // carries the whole story. Anything else the rescue hit (an endpoint that
        // does not know the `thinking` field, say) is reported as itself — its
        // status code is the more actionable one.
        Err(ConvertError::EmptyCompletion { message, .. }) => {
            let first_message = first_err.message();
            Err(ConvertError::EmptyCompletion {
                message: format!("{first_message}; retried with thinking disabled: {message}"),
                finish_reason: None,
            })
        }
        Err(other) => Err(other),
    }
}

/// One request, no retry. `thinking_off` overrides the config for this request
/// only — see [`convert`].
async fn convert_attempt(
    client: &reqwest::Client,
    cfg: &Config,
    ctx: &WindowContext,
    pinyin: &str,
    exclude: &[String],
    thinking_off: bool,
) -> Result<Completed, ConvertError> {
    let regen = regen_instruction(exclude);
    let (temperature, max_tokens, max_completion_tokens) = token_params(cfg, exclude, thinking_off);
    let (thinking, reasoning_effort) = thinking_params(cfg, thinking_off);
    let body = ChatRequest {
        model: &cfg.model,
        messages: build_messages(cfg, ctx, pinyin, &regen),
        temperature,
        max_tokens,
        max_completion_tokens,
        thinking,
        reasoning_effort,
        stream: false,
    };

    let (content, finish_reason, usage) = post_chat(client, cfg, &body).await?;
    let text = sanitize(&content);
    if text.is_empty() {
        return Err(empty_completion(finish_reason.as_deref()));
    }
    Ok(Completed { text, usage })
}

/// Fold a window's history into a short context note, replacing everything older
/// than the turns the caller keeps.
///
/// The request is the same conversation the conversions send, plus the
/// compaction instruction as a final user message — so it shares a prefix with
/// the normal requests and hits the same cache entry.
pub async fn compact_context(
    client: &reqwest::Client,
    cfg: &Config,
    ctx: &WindowContext,
) -> Result<String, ConvertError> {
    // No rescue here, deliberately: a compaction that comes back empty is
    // swallowed by the caller (`compact_if_needed` takes `.ok()`), and the
    // history it would have summarised is still there for the next attempt.
    let (temperature, max_tokens, max_completion_tokens) =
        token_params(cfg, &[], thinking_disabled(cfg));
    let (thinking, reasoning_effort) = thinking_params(cfg, thinking_disabled(cfg));
    let body = ChatRequest {
        model: &cfg.model,
        messages: build_messages(cfg, ctx, &cfg.context_prompt, &None),
        temperature,
        max_tokens,
        max_completion_tokens,
        thinking,
        reasoning_effort,
        stream: false,
    };

    let (content, finish_reason, _) = post_chat(client, cfg, &body).await?;
    let summary = extract_summary(&content);
    if summary.is_empty() {
        return Err(empty_completion(finish_reason.as_deref()));
    }
    Ok(summary)
}

/// POST one non-streaming chat request. Returns the first choice's content, its
/// `finish_reason`, and the usage the provider reported.
async fn post_chat(
    client: &reqwest::Client,
    cfg: &Config,
    body: &ChatRequest<'_>,
) -> Result<(String, Option<String>, Usage), ConvertError> {
    if cfg.api_key.trim().is_empty() {
        return Err(ConvertError::Config(
            "API key is not set — open Settings and add your key".to_string(),
        ));
    }

    let url = format!("{}/chat/completions", cfg.base_url.trim_end_matches('/'));
    let resp = client
        .post(&url)
        .bearer_auth(&cfg.api_key)
        .timeout(Duration::from_millis(cfg.timeout_ms))
        .json(body)
        .send()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    let status = resp.status();
    let text = resp
        .text()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    if !status.is_success() {
        // Try to surface the provider's error message.
        let detail = serde_json::from_str::<ApiErrorEnvelope>(&text)
            .map(|e| e.error.message)
            .unwrap_or_else(|_| text.chars().take(300).collect());
        return Err(match status.as_u16() {
            401 | 403 => ConvertError::Auth(detail),
            _ => ConvertError::Api(format!("HTTP {}: {}", status.as_u16(), detail)),
        });
    }

    let parsed: ChatResponse =
        serde_json::from_str(&text).map_err(|e| ConvertError::Api(format!("bad response: {e}")))?;
    let choice = parsed
        .choices
        .into_iter()
        .next()
        .ok_or_else(|| ConvertError::Api("empty choices".to_string()))?;
    Ok((choice.message.content, choice.finish_reason, parsed.usage))
}

/// An empty completion is a failure, not a success.
///
/// A reasoning model spends `max_tokens` on its hidden chain of thought before
/// emitting anything; when the budget runs out the reply comes back empty with
/// `finish_reason: "length"`. Reporting that as `Ok("")` made the IME look like
/// it had silently done nothing — and would have recorded an empty turn into the
/// conversation context.
fn empty_completion(finish_reason: Option<&str>) -> ConvertError {
    let why = match finish_reason {
        Some("length") => {
            " (finish_reason: length — the budget ran out, most likely consumed by \
             the model's hidden reasoning)"
        }
        Some(other) => other,
        None => "",
    };
    ConvertError::EmptyCompletion {
        message: format!(
            "the model returned an empty conversion{why}; raise `max_tokens` in Settings"
        ),
        finish_reason: finish_reason.map(str::to_string),
    }
}

/// Pull the `<summary>` block out of a compaction reply, dropping the
/// `<analysis>` scratchpad. Both are the shape `DEFAULT_CONTEXT_PROMPT` asks for;
/// only the summary is stored, so the model's reasoning never enters the context.
fn extract_summary(raw: &str) -> String {
    let text = raw.trim();
    if let Some(start) = text.find("<summary>") {
        let rest = &text[start + "<summary>".len()..];
        let body = rest.split("</summary>").next().unwrap_or(rest);
        return body.trim().to_string();
    }
    // No tags at all: strip any scratchpad and keep what is left.
    let mut out = String::new();
    let mut rest = text;
    while let Some(start) = rest.find("<analysis>") {
        out.push_str(&rest[..start]);
        match rest[start..].find("</analysis>") {
            Some(end) => rest = &rest[start + end + "</analysis>".len()..],
            None => {
                rest = "";
                break;
            }
        }
    }
    out.push_str(rest);
    out.trim().to_string()
}

/// Stream a conversion (SSE, `stream: true`). `on_delta` is called with the
/// *cumulative* text each time the model emits more, so the frontend can replace
/// the pre-edit incrementally. The cumulative text passed to `on_delta` is raw
/// (not sanitized) so partial quotes/whitespace may appear; only the returned
/// final value is sanitized.
pub async fn convert_stream<F>(
    client: &reqwest::Client,
    cfg: &Config,
    ctx: &WindowContext,
    pinyin: &str,
    exclude: &[String],
    mut on_delta: F,
) -> Result<Completed, ConvertError>
where
    F: FnMut(&str),
{
    if cfg.api_key.trim().is_empty() {
        return Err(ConvertError::Config(
            "API key is not set — open Settings and add your key".to_string(),
        ));
    }

    let url = format!("{}/chat/completions", cfg.base_url.trim_end_matches('/'));
    let regen = regen_instruction(exclude);
    // One attempt, no rescue. Unlike `convert` this path has no caller in any
    // frontend (the queue is non-streaming by design, and only the CLI example
    // streams), and a retry here would mean re-running a request whose partials
    // have already been handed out — safe today only because an empty completion
    // implies no partial ever fired, which is not an invariant worth building on.
    let (temperature, max_tokens, max_completion_tokens) =
        token_params(cfg, exclude, thinking_disabled(cfg));
    let (thinking, reasoning_effort) = thinking_params(cfg, thinking_disabled(cfg));
    let body = ChatRequest {
        model: &cfg.model,
        messages: build_messages(cfg, ctx, pinyin, &regen),
        temperature,
        max_tokens,
        max_completion_tokens,
        thinking,
        reasoning_effort,
        stream: true,
    };

    let mut resp = client
        .post(&url)
        .bearer_auth(&cfg.api_key)
        .timeout(Duration::from_millis(cfg.timeout_ms))
        .json(&body)
        .send()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?;

    let status = resp.status();
    if !status.is_success() {
        // Errors come back as a normal JSON body, not an SSE stream.
        let text = resp.text().await.unwrap_or_default();
        let detail = serde_json::from_str::<ApiErrorEnvelope>(&text)
            .map(|e| e.error.message)
            .unwrap_or_else(|_| text.chars().take(300).collect());
        return Err(match status.as_u16() {
            401 | 403 => ConvertError::Auth(detail),
            _ => ConvertError::Api(format!("HTTP {}: {}", status.as_u16(), detail)),
        });
    }

    // Parse the SSE stream line by line. We buffer raw bytes and only decode
    // complete lines (split on '\n') so a multi-byte UTF-8 char straddling a
    // chunk boundary is never decoded mid-sequence.
    let mut buf: Vec<u8> = Vec::new();
    let mut acc = String::new();
    let mut usage = Usage::default();
    let mut finish_reason: Option<String> = None;
    while let Some(chunk) = resp
        .chunk()
        .await
        .map_err(|e| ConvertError::Network(e.to_string()))?
    {
        buf.extend_from_slice(&chunk);
        while let Some(pos) = buf.iter().position(|&b| b == b'\n') {
            let line: Vec<u8> = buf.drain(..=pos).collect();
            let line = String::from_utf8_lossy(&line);
            let line = line.trim();
            let Some(payload) = line.strip_prefix("data:") else {
                continue; // comments / blank lines / other SSE fields
            };
            let payload = payload.trim();
            if payload.is_empty() {
                continue;
            }
            if payload == "[DONE]" {
                let text = sanitize(&acc);
                if text.is_empty() {
                    return Err(empty_completion(finish_reason.as_deref()));
                }
                return Ok(Completed { text, usage });
            }
            if let Ok(parsed) = serde_json::from_str::<StreamChunk>(payload) {
                if let Some(reported) = parsed.usage {
                    usage = reported;
                }
                if let Some(choice) = parsed.choices.into_iter().next() {
                    // Reported once, on the final chunk before [DONE].
                    if choice.finish_reason.is_some() {
                        finish_reason = choice.finish_reason;
                    }
                    if let Some(piece) = choice.delta.content {
                        if !piece.is_empty() {
                            acc.push_str(&piece);
                            on_delta(&acc);
                        }
                    }
                }
            }
        }
    }

    let text = sanitize(&acc);
    if text.is_empty() {
        return Err(empty_completion(finish_reason.as_deref()));
    }
    Ok(Completed { text, usage })
}

/// Models sometimes wrap output in quotes or trailing whitespace; strip that so
/// the frontend gets a clean pre-edit string.
fn sanitize(s: &str) -> String {
    let t = s.trim();
    let t = t
        .strip_prefix('"')
        .and_then(|x| x.strip_suffix('"'))
        .unwrap_or(t);
    t.trim().to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sanitize_strips_quotes_and_whitespace() {
        assert_eq!(sanitize("  你好  "), "你好");
        assert_eq!(sanitize("\"你好世界\""), "你好世界");
        assert_eq!(sanitize("你好\n"), "你好");
        // Inner quotes are preserved; only a fully-wrapping pair is stripped.
        assert_eq!(sanitize("他说\"好\""), "他说\"好\"");
    }

    #[test]
    fn status_codes_match_header() {
        assert_eq!(ConvertError::Network(String::new()).status_code(), 1);
        assert_eq!(ConvertError::Auth(String::new()).status_code(), 2);
        assert_eq!(ConvertError::Api(String::new()).status_code(), 3);
        assert_eq!(
            ConvertError::EmptyCompletion {
                message: String::new(),
                finish_reason: None,
            }
            .status_code(),
            3,
            "an empty completion is DS_ERR_API: the C ABI must not grow a code"
        );
        assert_eq!(ConvertError::Cancelled.status_code(), 4);
        assert_eq!(ConvertError::Config(String::new()).status_code(), 5);
    }

    #[test]
    fn restricted_openai_models_detected() {
        for m in [
            "gpt-5",
            "gpt-5.5",
            "gpt-5-mini",
            "GPT-5",
            "o1",
            "o1-mini",
            "o3",
            "o3-mini",
            "o4-mini",
            "gpt-6",
        ] {
            assert!(is_restricted_openai_model(m), "{m} should be restricted");
        }
        for m in [
            "deepseek-v4-flash",
            "deepseek-chat",
            "gpt-4o",
            "gpt-4o-mini",
            "gpt-4.1",
            "o1ololo", // not a real o1 release; must not match
            "",
        ] {
            assert!(!is_restricted_openai_model(m), "{m} should be classic");
        }
    }

    #[test]
    fn token_params_pick_field_per_model() {
        let base = Config {
            max_tokens: 256,
            temperature: 0.3,
            // Temperature is only honoured with thinking switched off.
            thinking: "disabled".to_string(),
            ..Config::default()
        };

        // Classic provider: temperature + max_tokens, no max_completion_tokens.
        let cfg = Config {
            model: "deepseek-v4-flash".to_string(),
            ..base.clone()
        };
        let (temp, max_tok, max_comp) = token_params(&cfg, &[], thinking_disabled(&cfg));
        assert_eq!(temp, Some(0.3));
        assert_eq!(max_tok, Some(256));
        assert_eq!(max_comp, None);

        // Restricted OpenAI: max_completion_tokens, no temperature/max_tokens.
        let cfg = Config {
            model: "gpt-5.5".to_string(),
            ..base
        };
        let (temp, max_tok, max_comp) = token_params(&cfg, &[], thinking_disabled(&cfg));
        assert_eq!(temp, None);
        assert_eq!(max_tok, None);
        assert_eq!(max_comp, Some(256));
    }

    #[test]
    fn restricted_request_serializes_without_temperature_or_max_tokens() {
        let body = ChatRequest {
            model: "gpt-5.5",
            messages: vec![],
            temperature: None,
            max_tokens: None,
            max_completion_tokens: Some(256),
            thinking: None,
            reasoning_effort: None,
            stream: false,
        };
        let json = serde_json::to_string(&body).unwrap();
        assert!(json.contains("max_completion_tokens"));
        assert!(!json.contains("\"temperature\""));
        assert!(!json.contains("\"max_tokens\""));
    }

    #[test]
    fn temperature_follows_this_requests_thinking_mode() {
        // A provider ignores temperature while its thinking mode is on, and that
        // mode is on by default — so no temperature may be sent.
        let cfg = Config {
            model: "deepseek-v4-flash".to_string(),
            temperature: 0.3,
            ..Config::default() // thinking: ""
        };
        assert_eq!(token_params(&cfg, &[], false).0, None);

        // The rescue attempt turns thinking off for one request without touching
        // the config, and that is the only case in which temperature is
        // meaningful — so it must follow the flag, not the stored value.
        assert_eq!(token_params(&cfg, &[], true).0, Some(0.3));

        // Explicitly switching thinking off in the config does the same thing.
        let cfg = Config {
            thinking: "disabled".to_string(),
            ..cfg
        };
        assert_eq!(token_params(&cfg, &[], true).0, Some(0.3));
    }

    #[test]
    fn thinking_params_serialize_only_when_configured() {
        // Defaults: the effort hint is sent, the thinking switch is not.
        let cfg = Config::default();
        let (thinking, effort) = thinking_params(&cfg, thinking_disabled(&cfg));
        assert!(thinking.is_none());
        assert_eq!(effort, Some("low"));

        // An explicit switch is sent; an emptied effort is omitted.
        let cfg = Config {
            thinking: "enabled".to_string(),
            reasoning_effort: String::new(),
            ..Config::default()
        };
        let (thinking, effort) = thinking_params(&cfg, thinking_disabled(&cfg));
        assert_eq!(thinking.map(|t| t.kind), Some("enabled"));
        assert!(effort.is_none());

        // Case/whitespace tolerant; an unrecognised value omits the field.
        let cfg = Config {
            thinking: " DISABLED ".to_string(),
            ..Config::default()
        };
        assert_eq!(
            thinking_params(&cfg, thinking_disabled(&cfg))
                .0
                .map(|t| t.kind),
            Some("disabled")
        );

        let cfg = Config {
            thinking: "sometimes".to_string(),
            ..Config::default()
        };
        assert!(thinking_params(&cfg, thinking_disabled(&cfg)).0.is_none());

        // The rescue flag overrides whatever the config asked for — including a
        // user who explicitly turned thinking *on*. Falling back to their
        // settings would cost them the sentence.
        let cfg = Config {
            thinking: "enabled".to_string(),
            ..Config::default()
        };
        let (thinking, effort) = thinking_params(&cfg, true);
        assert_eq!(thinking.map(|t| t.kind), Some("disabled"));
        assert_eq!(effort, Some("low"), "effort is left as configured");
    }

    #[test]
    fn default_request_carries_effort_and_no_temperature() {
        let cfg = Config::default();
        let thinking_off = thinking_disabled(&cfg);
        let (temperature, max_tokens, max_completion_tokens) =
            token_params(&cfg, &[], thinking_off);
        let (thinking, reasoning_effort) = thinking_params(&cfg, thinking_off);
        let body = ChatRequest {
            model: &cfg.model,
            messages: vec![],
            temperature,
            max_tokens,
            max_completion_tokens,
            thinking,
            reasoning_effort,
            stream: false,
        };
        let json = serde_json::to_string(&body).unwrap();
        assert!(json.contains("\"reasoning_effort\":\"low\""), "{json}");
        assert!(!json.contains("\"temperature\""), "{json}");
        assert!(!json.contains("\"thinking\""), "{json}");
    }
}

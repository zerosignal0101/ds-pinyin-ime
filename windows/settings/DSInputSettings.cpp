// DSInputSettings.cpp — standalone Win32 settings dialog for DS Input.
//
// A tiny separate process (DSInputSettings.exe) launched from the IME's
// language-bar menu. It shares the SAME core as the text service, so it reads
// and writes the SAME config file via ds_engine_get_config_json /
// ds_engine_set_config_json — there is exactly one source of truth.
//
// Why a separate exe (not in-proc UI): a TSF text service is loaded into every
// app that takes input; popping a real settings window from inside that host is
// awkward and risky. A standalone process keeps the IME DLL lean and the UI
// robust.
//
// JSON handling: the core hands us a JSON *object* string matching
// core::config::Config and expects the same shape back. To avoid pulling in a
// JSON library we do minimal, targeted parsing (find a key, read its value) and
// rebuild the object from the field values with proper escaping. The core
// fills any omitted field from its defaults, so we always send the full set.

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>  // CoInitializeEx / CoUninitialize (excluded by WIN32_LEAN_AND_MEAN)
#include <string>
#include <cstdlib>   // strtol
#include <cctype>    // isdigit

#include "../resource.h"
#include "../DsimeCore.h"

#pragma comment(lib, "comctl32.lib")

// ---- minimal JSON helpers --------------------------------------------------

namespace {

// Escape a UTF-8 string for embedding in a JSON string literal.
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    wsprintfA(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Find a top-level string value: "key": "value". Returns unescaped value.
// Good enough for the flat Config object the core emits (pretty-printed).
std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return std::string();
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t q = json.find('"', colon + 1);
    if (q == std::string::npos) return std::string();
    std::string out;
    for (size_t i = q + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            char n = json[++i];
            switch (n) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case '/': out += '/';  break;
                case 'u': {
                    if (i + 4 < json.size()) {
                        std::string hex = json.substr(i + 1, 4);
                        int cp = static_cast<int>(strtol(hex.c_str(), nullptr, 16));
                        i += 4;
                        // Encode the (BMP) code point as UTF-8.
                        if (cp < 0x80) {
                            out += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                }
                default: out += n; break;
            }
        } else if (c == '"') {
            break;  // end of string
        } else {
            out += c;
        }
    }
    return out;
}

// Find a top-level numeric value: "key": 123(.45). Returns the raw token text.
std::string JsonGetNumber(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return std::string();
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return std::string();
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    std::string num;
    while (i < json.size() &&
           (isdigit(static_cast<unsigned char>(json[i])) || json[i] == '.' ||
            json[i] == '-' || json[i] == '+' || json[i] == 'e' || json[i] == 'E')) {
        num += json[i++];
    }
    return num;
}

// Find a top-level boolean: "key": true|false. Falls back when absent.
//
// This needs its own reader: JsonGetString would see no quote after the colon
// and happily run on to the NEXT key's opening quote, returning that key's name.
bool JsonGetBool(const std::string& json, const std::string& key, bool fallback) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return fallback;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return fallback;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (json.compare(i, 4, "true") == 0) return true;
    if (json.compare(i, 5, "false") == 0) return false;
    return fallback;
}

// ---- dialog field <-> control glue ----------------------------------------

std::wstring GetText(HWND dlg, int id) {
    HWND h = ::GetDlgItem(dlg, id);
    int len = ::GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(len), L'\0');
    if (len > 0) ::GetWindowTextW(h, s.data(), len + 1);
    return s;
}

void SetText(HWND dlg, int id, const std::wstring& s) {
    ::SetDlgItemTextW(dlg, id, s.c_str());
}

void SetCheck(HWND dlg, int id, bool on) {
    ::SendDlgItemMessageW(dlg, id, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool GetCheck(HWND dlg, int id) {
    return ::SendDlgItemMessageW(dlg, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// The engine is shared by the dialog proc via a single global for simplicity
// (this is a one-window, one-engine process).
dsime::Engine g_engine;

// Config fields the dialog does not surface.
//
// They are read back at load time and re-emitted unchanged on save, because
// saving does not *merge*: ds_engine_set_config_json replaces the whole object,
// and the core's serde defaults then fill anything the new object omitted. A
// field we dropped would not be "left alone" — it would silently snap back to
// its default. `context_prompt` is the one that would hurt: it is a long,
// carefully tuned prompt, and a user who edited it by hand should not lose it by
// opening this dialog and pressing Save.
//
// (The rule generalises: every field in core::config::Config must appear either
// in the dialog or here.)
struct HiddenFields {
    std::string context_prompt;
    std::string context_compact_ratio;
    std::string context_max_windows;
};
HiddenFields g_hidden;

// Fields with the CONTEXT_ENABLED checkbox as their master switch.
const int kContextDependents[] = {
    IDC_CONTEXT_WINDOW_TOKENS, IDC_CONTEXT_KEEP_RECENT, IDC_CLEAR_CONTEXT,
};

void SyncContextEnable(HWND dlg) {
    const BOOL on = GetCheck(dlg, IDC_CONTEXT_ENABLED) ? TRUE : FALSE;
    for (int id : kContextDependents) {
        ::EnableWindow(::GetDlgItem(dlg, id), on);
    }
}

void LoadIntoDialog(HWND dlg) {
    dsime::CoreString json = g_engine.GetConfigJson();
    std::string j = json.to_string();

    SetText(dlg, IDC_BASE_URL,      dsime::Utf8ToUtf16(JsonGetString(j, "base_url")));
    SetText(dlg, IDC_API_KEY,       dsime::Utf8ToUtf16(JsonGetString(j, "api_key")));
    SetText(dlg, IDC_MODEL,         dsime::Utf8ToUtf16(JsonGetString(j, "model")));
    SetText(dlg, IDC_SYSTEM_PROMPT, dsime::Utf8ToUtf16(JsonGetString(j, "system_prompt")));
    SetText(dlg, IDC_TEMPERATURE,   dsime::Utf8ToUtf16(JsonGetNumber(j, "temperature")));
    SetText(dlg, IDC_MAX_TOKENS,    dsime::Utf8ToUtf16(JsonGetNumber(j, "max_tokens")));
    SetText(dlg, IDC_TIMEOUT_MS,    dsime::Utf8ToUtf16(JsonGetNumber(j, "timeout_ms")));
    SetText(dlg, IDC_REASONING_EFFORT, dsime::Utf8ToUtf16(JsonGetString(j, "reasoning_effort")));
    SetText(dlg, IDC_THINKING,      dsime::Utf8ToUtf16(JsonGetString(j, "thinking")));

    SetCheck(dlg, IDC_STREAM, JsonGetBool(j, "stream", true));
    SetCheck(dlg, IDC_CONTEXT_ENABLED, JsonGetBool(j, "context_enabled", true));
    SetText(dlg, IDC_CONTEXT_WINDOW_TOKENS,
            dsime::Utf8ToUtf16(JsonGetNumber(j, "context_window_tokens")));
    SetText(dlg, IDC_CONTEXT_KEEP_RECENT,
            dsime::Utf8ToUtf16(JsonGetNumber(j, "context_keep_recent")));
    SetText(dlg, IDC_QUEUE_MAX_PENDING,
            dsime::Utf8ToUtf16(JsonGetNumber(j, "queue_max_pending")));

    g_hidden.context_prompt        = JsonGetString(j, "context_prompt");
    g_hidden.context_compact_ratio = JsonGetNumber(j, "context_compact_ratio");
    g_hidden.context_max_windows   = JsonGetNumber(j, "context_max_windows");

    SyncContextEnable(dlg);

    dsime::CoreString path = g_engine.ConfigPath();
    SetText(dlg, IDC_CONFIG_PATH, L"Config: " + path.to_wstring());
}

// Build the JSON config object from the current dialog fields.
//
// EVERY field of core::config::Config has to be emitted here (or carried in
// g_hidden). The core replaces the whole object on save, and its serde defaults
// fill whatever is missing — so an omitted field is not preserved, it is reset.
std::string BuildConfigJson(HWND dlg) {
    std::string base_url   = dsime::Utf16ToUtf8(GetText(dlg, IDC_BASE_URL));
    std::string api_key    = dsime::Utf16ToUtf8(GetText(dlg, IDC_API_KEY));
    std::string model      = dsime::Utf16ToUtf8(GetText(dlg, IDC_MODEL));
    std::string prompt     = dsime::Utf16ToUtf8(GetText(dlg, IDC_SYSTEM_PROMPT));
    std::string temp       = dsime::Utf16ToUtf8(GetText(dlg, IDC_TEMPERATURE));
    std::string max_tokens = dsime::Utf16ToUtf8(GetText(dlg, IDC_MAX_TOKENS));
    std::string timeout    = dsime::Utf16ToUtf8(GetText(dlg, IDC_TIMEOUT_MS));
    // Both thinking knobs are strings and may be left empty, which tells the core
    // to omit the corresponding request field entirely (for endpoints that
    // reject them) — so they are never defaulted here.
    std::string reasoning  = dsime::Utf16ToUtf8(GetText(dlg, IDC_REASONING_EFFORT));
    std::string thinking   = dsime::Utf16ToUtf8(GetText(dlg, IDC_THINKING));

    std::string ctx_window = dsime::Utf16ToUtf8(GetText(dlg, IDC_CONTEXT_WINDOW_TOKENS));
    std::string ctx_recent = dsime::Utf16ToUtf8(GetText(dlg, IDC_CONTEXT_KEEP_RECENT));
    std::string queue_max  = dsime::Utf16ToUtf8(GetText(dlg, IDC_QUEUE_MAX_PENDING));

    // Default numeric fields if the user blanked them, so the JSON stays valid.
    //
    // These literals are copies of the core's `default_*()` functions, and that
    // duplication has teeth: saving here goes through `ds_engine_set_config_json`,
    // which writes the object straight out and never re-enters
    // `Config::load_or_create` — so the core's stock-value migrations do not run
    // on this path. A stale literal here silently writes the old default back
    // into a config that had just been migrated. Keep them in step by hand.
    if (temp.empty())       temp = "0.3";
    if (max_tokens.empty()) max_tokens = "1024";
    if (timeout.empty())    timeout = "15000";   // core: DEFAULT_TIMEOUT_MS
    if (ctx_window.empty()) ctx_window = "16384";
    if (ctx_recent.empty()) ctx_recent = "10";
    if (queue_max.empty())  queue_max = "8";

    std::string json;
    json += "{\n";
    json += "  \"base_url\": \""      + JsonEscape(base_url) + "\",\n";
    json += "  \"api_key\": \""       + JsonEscape(api_key)  + "\",\n";
    json += "  \"model\": \""         + JsonEscape(model)    + "\",\n";
    json += "  \"system_prompt\": \"" + JsonEscape(prompt)   + "\",\n";
    json += "  \"temperature\": "     + temp       + ",\n";
    json += "  \"max_tokens\": "      + max_tokens + ",\n";
    json += "  \"reasoning_effort\": \"" + JsonEscape(reasoning) + "\",\n";
    json += "  \"thinking\": \""      + JsonEscape(thinking) + "\",\n";
    json += "  \"timeout_ms\": "      + timeout    + ",\n";
    // std::string(...) rather than a bare ternary: a char* + char* is pointer
    // arithmetic, not concatenation.
    json += "  \"stream\": " + std::string(GetCheck(dlg, IDC_STREAM) ? "true" : "false") + ",\n";
    json += "  \"context_enabled\": " +
            std::string(GetCheck(dlg, IDC_CONTEXT_ENABLED) ? "true" : "false") + ",\n";
    json += "  \"context_window_tokens\": " + ctx_window + ",\n";
    json += "  \"context_keep_recent\": "   + ctx_recent + ",\n";

    // Carried through untouched from what the core last handed us. Emitted only
    // when non-empty: a *missing* field takes the core's default, whereas an
    // empty one would be taken literally (an empty compaction prompt, a ratio of
    // 0). Better to omit than to corrupt.
    if (!g_hidden.context_prompt.empty()) {
        json += "  \"context_prompt\": \"" + JsonEscape(g_hidden.context_prompt) + "\",\n";
    }
    if (!g_hidden.context_compact_ratio.empty()) {
        json += "  \"context_compact_ratio\": " + g_hidden.context_compact_ratio + ",\n";
    }
    if (!g_hidden.context_max_windows.empty()) {
        json += "  \"context_max_windows\": " + g_hidden.context_max_windows + ",\n";
    }

    // Last, and therefore the one line without a trailing comma.
    json += "  \"queue_max_pending\": " + queue_max + "\n";
    json += "}\n";
    return json;
}

// Build the JSON from the dialog fields and persist it through the core.
bool SaveFromDialog(HWND dlg, std::wstring* errOut) {
    std::string json = BuildConfigJson(dlg);
    int32_t rc = g_engine.SetConfigJson(json.c_str());
    if (rc != DS_OK) {
        if (errOut) *errOut = dsime::LastError();
        return false;
    }
    return true;
}

// ---- "Test" button: sample conversion against the on-screen settings -------
//
// Side-effect-free: we never touch g_engine or the saved config. Instead we
// write the current fields to a throwaway temp config file and spin up a
// disposable engine on it, convert a fixed sample ("nihao"), and report the
// result or the core's error. This lets the user verify base URL / key / model
// BEFORE committing them with Save.

struct TestState {
    HANDLE       done = nullptr;
    int32_t      status = DS_ERR_INTERNAL;
    std::wstring text;
};

// CORE WORKER THREAD: stash the result and wake the UI thread. Signature must
// match DsConvertCallback exactly (plain C calling convention).
void TestConvertCb(void* user_data, uint64_t /*request_id*/, int32_t status,
                   const char* text_utf8) {
    TestState* st = static_cast<TestState*>(user_data);
    st->status = status;
    // text_utf8 carries the converted sentence on success, and the core's error
    // detail (e.g. "network error: …", "auth error: …") on failure. Capture it
    // either way — ds_last_error() is thread-local to this worker thread and so
    // is unreadable from the UI thread.
    st->text = dsime::Utf8ToUtf16(text_utf8);
    ::SetEvent(st->done);
}

void TestConnection(HWND dlg) {
    SetText(dlg, IDC_STATUS, L"Testing... converting \"nihao\"");
    ::UpdateWindow(dlg);

    std::string json = BuildConfigJson(dlg);

    // Stage the fields in a temp config file (UTF-8, no BOM) for a throwaway
    // engine — keeps the test from overwriting the user's saved config.
    wchar_t tmpDir[MAX_PATH] = {};
    wchar_t tmpFile[MAX_PATH] = {};
    if (!::GetTempPathW(MAX_PATH, tmpDir) ||
        !::GetTempFileNameW(tmpDir, L"dsi", 0, tmpFile)) {
        SetText(dlg, IDC_STATUS, L"Test: could not create a temp file");
        return;
    }
    {
        HANDLE hf = ::CreateFileW(tmpFile, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            SetText(dlg, IDC_STATUS, L"Test: could not write temp config");
            ::DeleteFileW(tmpFile);
            return;
        }
        DWORD wrote = 0;
        ::WriteFile(hf, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
        ::CloseHandle(hf);
    }

    std::wstring resultMsg;
    {
        dsime::Engine testEngine;
        if (!testEngine.Create(dsime::Utf16ToUtf8(tmpFile).c_str())) {
            resultMsg = L"Test: invalid settings - " + dsime::LastError();
        } else {
            dsime::Session sess;
            if (!sess.Create(testEngine)) {
                resultMsg = L"Test: could not create a session";
            } else {
                sess.SetInput("nihao");
                TestState st;
                st.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                uint64_t id = sess.Convert(&TestConvertCb, &st);
                if (id == 0) {
                    resultMsg = L"Test: empty input";
                } else {
                    // The C ABI guarantees the callback fires exactly once, so we
                    // wait for it (pumping messages to stay responsive) and only
                    // then tear down `st`/sess. After a soft cap we nudge with
                    // Cancel; the (cancelled) callback still fires and ends this.
                    DWORD start = ::GetTickCount();
                    bool nudged = false;
                    for (;;) {
                        DWORD rc = ::MsgWaitForMultipleObjects(1, &st.done, FALSE,
                                                               200, QS_ALLINPUT);
                        if (rc == WAIT_OBJECT_0) break;
                        if (rc == WAIT_OBJECT_0 + 1) {
                            MSG m;
                            while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                                ::TranslateMessage(&m);
                                ::DispatchMessageW(&m);
                            }
                        }
                        if (!nudged && ::GetTickCount() - start > 20000) {
                            sess.Cancel();
                            nudged = true;
                        }
                    }
                    if (st.status == DS_OK) {
                        resultMsg = L"Test OK:  nihao -> " + st.text;
                    } else {
                        // Prefer the per-result detail carried in the callback;
                        // fall back to the (usually empty) thread-local error.
                        std::wstring e = st.text;
                        if (e.empty()) e = dsime::LastError();
                        if (e.empty()) e = L"conversion failed";
                        resultMsg = L"Test failed: " + e;
                    }
                }
                if (st.done) ::CloseHandle(st.done);
            }
        }
    }
    ::DeleteFileW(tmpFile);
    SetText(dlg, IDC_STATUS, resultMsg);
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Icon on the dialog.
            HICON hIcon = ::LoadIconW(::GetModuleHandleW(nullptr),
                                      MAKEINTRESOURCEW(IDI_DSIME));
            if (hIcon) {
                ::SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
                ::SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
            }
            LoadIntoDialog(dlg);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    std::wstring err;
                    if (SaveFromDialog(dlg, &err)) {
                        ::EndDialog(dlg, IDOK);
                    } else {
                        std::wstring msgText = L"Could not save settings.\n\n" + err;
                        ::MessageBoxW(dlg, msgText.c_str(), L"DS Input",
                                      MB_OK | MB_ICONERROR);
                    }
                    return TRUE;
                }
                case IDC_TEST: {
                    // Disable the buttons for the duration: TestConnection pumps
                    // messages while waiting, so without this the dialog could
                    // re-enter (a second Test, or Cancel destroying us mid-wait).
                    HWND bTest   = ::GetDlgItem(dlg, IDC_TEST);
                    HWND bSave   = ::GetDlgItem(dlg, IDOK);
                    HWND bCancel = ::GetDlgItem(dlg, IDCANCEL);
                    ::EnableWindow(bTest, FALSE);
                    ::EnableWindow(bSave, FALSE);
                    ::EnableWindow(bCancel, FALSE);
                    TestConnection(dlg);
                    ::EnableWindow(bTest, TRUE);
                    ::EnableWindow(bSave, TRUE);
                    ::EnableWindow(bCancel, TRUE);
                    return TRUE;
                }
                case IDC_CONTEXT_ENABLED:
                    SyncContextEnable(dlg);
                    return TRUE;
                case IDC_CLEAR_CONTEXT: {
                    // Immediate, not tied to Save: it is a deletion, and the user
                    // should see it happen when they ask for it.
                    const int answer = ::MessageBoxW(
                        dlg,
                        L"Forget everything DS Input has remembered?\n\n"
                        L"This deletes the record of what you have typed since "
                        L"this program started: the text that is sent with each "
                        L"conversion to give the model context. It cannot be undone.",
                        L"DS Input", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
                    if (answer != IDYES) return TRUE;
                    if (g_engine.ClearContexts() == DS_OK) {
                        SetText(dlg, IDC_STATUS, L"Stored context cleared.");
                    } else {
                        SetText(dlg, IDC_STATUS,
                                L"Could not clear context: " + dsime::LastError());
                    }
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            ::EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // COM is not strictly needed by the engine, but init it in case future core
    // changes use COM-backed paths; STA matches a UI process.
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);

    // One shared engine for the dialog lifetime; default per-user config path.
    if (!g_engine.Create(nullptr)) {
        std::wstring err = dsime::LastError();
        std::wstring msgText = L"Failed to load DS Input core engine.\n\n" + err;
        ::MessageBoxW(nullptr, msgText.c_str(), L"DS Input", MB_OK | MB_ICONERROR);
        ::CoUninitialize();
        return 1;
    }

    ::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
                      DlgProc, 0);

    g_engine.reset();
    ::CoUninitialize();
    return 0;
}

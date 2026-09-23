// Trace.cpp — see Trace.h for why this exists and how it is switched on.

#include "Trace.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <new>

namespace {

// Resolved once per process. The marker is not expected to appear or vanish
// while a host is running, and re-statting it on every keystroke would put a
// syscall in the input path.
bool TraceEnabled() {
    static const bool on = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = ::GetTempPathW(ARRAYSIZE(path), path);
        if (n == 0 || n >= ARRAYSIZE(path) - 16) return false;
        ::wcscat_s(path, L"dspinyinime-trace.on");
        return ::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }();
    return on;
}

}  // namespace

bool DsimeTraceEnabled() { return TraceEnabled(); }

void DsimeTrace(const wchar_t* fmt, ...) {
    if (!TraceEnabled()) return;

    constexpr size_t kMax = 2048;
    wchar_t line[kMax] = {};

    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    const int used = ::swprintf_s(line, kMax, L"%02u:%02u:%02u.%03u [t%lu] ",
                                  st.wHour, st.wMinute, st.wSecond,
                                  st.wMilliseconds, ::GetCurrentThreadId());
    if (used < 0) return;

    va_list ap;
    va_start(ap, fmt);
    const int n = ::_vsnwprintf_s(line + used, kMax - used, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) {
        line[kMax - 2] = L'\0';  // truncated: keep the prefix, lose the tail
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD pn = ::GetTempPathW(ARRAYSIZE(path), path);
    if (pn == 0 || pn >= ARRAYSIZE(path) - 16) return;
    ::wcscat_s(path, L"dspinyinime-trace.log");

    // UTF-8 appended as binary. Opening with "ccs=UTF-8" instead would re-emit a
    // BOM on every append, turning the log into a field of U+FEFF.
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0,
                                            nullptr, nullptr);
    if (bytes <= 1) return;
    char* utf8 = new (std::nothrow) char[bytes];
    if (utf8 == nullptr) return;
    ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, bytes, nullptr, nullptr);

    FILE* f = nullptr;
    if (::_wfopen_s(&f, path, L"ab") == 0 && f != nullptr) {
        ::fwrite(utf8, 1, static_cast<size_t>(bytes - 1), f);
        ::fwrite("\r\n", 1, 2, f);
        ::fclose(f);
    }
    delete[] utf8;
}

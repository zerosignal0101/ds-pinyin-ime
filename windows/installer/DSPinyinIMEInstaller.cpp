// DSPinyinIMEInstaller.cpp — a guided, macOS-style installer for the DS Pinyin IME
// TSF text service. One self-contained, elevated exe that embeds both the x64 and ARM64
// builds, installs the set matching the host, registers the text service, and
// adds it to the user's language list.
//
// Flow (single window): describe → Install → progress → done / failed.
//
//   1. Extract the host-arch payload (dsime_tsf.dll, dsime.dll,
//      DSPinyinIMESettings.exe) to %ProgramFiles%\DS Pinyin IME.
//   2. regsvr32 the TSF DLL (the native-arch regsvr32 in System32 matches the
//      native-arch DLL we install).
//   3. Add the DS Pinyin IME profile to the user's zh-Hans language list.
//   4. Tell the user to sign out / back in (Windows enrolls a freshly registered
//      text service at the next logon, like macOS does at login).

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>  // SHFileOperationW (WIN32_LEAN_AND_MEAN keeps it out of windows.h)
#include <shlobj.h>
#include <winver.h>
#include <cwchar>  // _wcsicmp, swprintf_s
#include <string>
#include <vector>

#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "version.lib")  // GetFileVersionInfo: the installed version

namespace {

constexpr wchar_t kInstallDirName[] = L"DS Pinyin IME";
// Copied into the install directory during install so that the UninstallString in
// Apps & features points somewhere stable — not at wherever this exe was
// downloaded to, which the user is free to delete.
constexpr wchar_t kInstallerExeName[] = L"DSPinyinIMEInstaller.exe";
// TSF profile id for the language list: "0804:{CLSID}{PROFILE}" — must match
// windows/Guids.h (c_clsidDsimeTextService / c_guidDsimeProfile) and LANGID 0804.
// Narrow, because its only consumer is the PowerShell source built below.
constexpr char kTipId[] =
    "0804:{6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}{A1B2C3D4-55E6-47F8-8901-23456789ABCD}";
// The Apps & features entry. Under HKLM because the install is machine-wide
// (Program Files plus an HKCR COM registration), so removing it needs the same
// elevation that writing it did.
constexpr wchar_t kUninstallKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DSPinyinIME";
// The per-user settings tree, as directories::ProjectDirs builds it in
// core/src/config.rs — %APPDATA%\DSPinyinIME\DSPinyinIME.
constexpr wchar_t kConfigDirName[] = L"DSPinyinIME";

constexpr UINT WM_APP_PROGRESS = WM_APP + 1;  // lParam = wchar_t* (proc frees)
constexpr UINT WM_APP_DONE     = WM_APP + 2;  // wParam = 1 ok / 0 failed; lParam = wchar_t*

// ---- arch detection --------------------------------------------------------

bool HostIsArm64() {
    USHORT processMachine = 0, nativeMachine = 0;
    if (::IsWow64Process2(::GetCurrentProcess(), &processMachine, &nativeMachine)) {
        return nativeMachine == IMAGE_FILE_MACHINE_ARM64;
    }
    return false;  // default to x64 if the API is unavailable
}

// ---- small helpers ---------------------------------------------------------

std::wstring ProgramFilesDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &p)) && p) {
        out = p;
    }
    if (p) ::CoTaskMemFree(p);
    if (out.empty()) {
        wchar_t buf[MAX_PATH];
        DWORD n = ::GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH);
        out.assign(buf, n);
    }
    return out;
}

std::wstring AppDataDir() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        out = p;
    }
    if (p) ::CoTaskMemFree(p);
    return out;
}

// Write an embedded RCDATA resource to a file. On a sharing violation (a prior
// install's DLL is loaded) rename the existing file aside first.
bool ExtractResource(int id, const std::wstring& path, std::wstring* err) {
    HRSRC hr = ::FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!hr) { if (err) *err = L"missing payload resource"; return false; }
    DWORD size = ::SizeofResource(nullptr, hr);
    HGLOBAL hg = ::LoadResource(nullptr, hr);
    if (!hg || size == 0) { if (err) *err = L"could not load payload"; return false; }
    const void* data = ::LockResource(hg);
    if (!data) { if (err) *err = L"could not lock payload"; return false; }

    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (attempt == 0 && ::GetLastError() == ERROR_SHARING_VIOLATION) {
                // Loaded by a running app — rename it aside (allowed on NTFS) and retry.
                std::wstring old = path + L".old";
                ::DeleteFileW(old.c_str());
                ::MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
                continue;
            }
            if (err) *err = L"could not write " + path;
            return false;
        }
        DWORD wrote = 0;
        BOOL ok = ::WriteFile(f, data, size, &wrote, nullptr) && wrote == size;
        ::CloseHandle(f);
        if (!ok) { if (err) *err = L"short write to " + path; return false; }
        return true;
    }
    if (err) *err = L"could not replace " + path + L" (in use)";
    return false;
}

// Run a process and wait; returns its exit code, or -1 on launch failure.
DWORD RunWait(const std::wstring& cmdline) {
    std::wstring mutableCmd = cmdline;  // CreateProcessW may modify the buffer
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return static_cast<DWORD>(-1);
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess);
    return code;
}

void PostProgress(HWND dlg, const wchar_t* text) {
    if (!dlg) return;  // silent mode: no UI to update
    ::PostMessageW(dlg, WM_APP_PROGRESS, 0,
                   reinterpret_cast<LPARAM>(::_wcsdup(text)));
}

// Write a UTF-8 file (used for the temp language-list script).
bool WriteAllBytes(const std::wstring& path, const std::string& bytes) {
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = ::WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr);
    ::CloseHandle(f);
    return ok && wrote == bytes.size();
}

// Add or remove the DS Pinyin IME profile in the current user's zh-Hans language
// list, via a short PowerShell script (the supported, forward-compatible API).
// Best-effort: if this fails the registration is still correct and only the
// language list is stale, which the user can put right by hand.
//
// Add and remove are deliberately not mirror images. Add touches the *first* zh
// list, creating zh-Hans-CN only if the user has no zh at all — the shape this
// has always had. Remove has to sweep *every* zh list, because leaving the tip
// on any of them is exactly the dead Win+Space entry that makes an uninstalled
// IME keep showing up. Note also that remove never creates a language; an
// uninstall that added one would be its own kind of bug.
void EditLanguageList(HWND dlg, bool add) {
    wchar_t tmp[MAX_PATH]; ::GetTempPathW(MAX_PATH, tmp);
    std::wstring ps = std::wstring(tmp) + L"dspinyinime-lang.ps1";
    // ASCII-only source: the TIP id is constant and nothing else is non-ASCII.
    std::string script =
        "Import-Module International -ErrorAction SilentlyContinue\r\n"
        "$tip = '" + std::string(kTipId) + "'\r\n"
        "$ll = Get-WinUserLanguageList\r\n"
        "$changed = $false\r\n";
    if (add) {
        script +=
            "$zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1\r\n"
            "if (-not $zh) {\r\n"
            "  $ll.Add('zh-Hans-CN')\r\n"
            "  $zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1\r\n"
            "  $changed = $true\r\n"
            "}\r\n"
            "if ($zh -and -not ($zh.InputMethodTips -contains $tip)) {\r\n"
            "  $zh.InputMethodTips.Add($tip); $changed = $true\r\n"
            "}\r\n";
    } else {
        script +=
            "foreach ($l in $ll) {\r\n"
            "  if ($l.LanguageTag -notlike 'zh*') { continue }\r\n"
            "  if (-not ($l.InputMethodTips -contains $tip)) { continue }\r\n"
            "  [void]$l.InputMethodTips.Remove($tip); $changed = $true\r\n"
            "}\r\n";
    }
    script += "if ($changed) { Set-WinUserLanguageList $ll -Force }\r\n";

    if (!WriteAllBytes(ps, script)) return;
    PostProgress(dlg, add ? L"Adding DS Pinyin IME to your language list…"
                          : L"Removing DS Pinyin IME from your language list…");
    RunWait(L"powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + ps + L"\"");
    ::DeleteFileW(ps.c_str());
}

// ---- uninstall -------------------------------------------------------------

// Is there something to uninstall? The Apps & features entry is the honest
// signal, but fall back to the DLL: an install that died between writing the
// files and recording itself still has to be removable.
bool IsInstalled() {
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        ::RegCloseKey(k);
        return true;
    }
    std::wstring dll = ProgramFilesDir() + L"\\" + kInstallDirName + L"\\dsime_tsf.dll";
    return ::GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The version of the DLL just written, read from its own version resource.
// Deliberately not a constant compiled into this exe: that would be a second
// copy of the version, free to disagree with the build the entry describes.
std::wstring FileVersionOf(const std::wstring& path) {
    DWORD dummy = 0;
    DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size);
    if (!::GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return L"";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!::VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || !ffi) return L"";
    wchar_t out[64];
    ::swprintf_s(out, L"%u.%u.%u.%u",
                 HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                 HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

enum class Removal { Gone, RenamedAside, Failed };

// Remove a file — or, if it is a loaded image, rename it aside instead. A running
// process holds its image open against deletion and overwriting, but NTFS does
// allow the *rename*, which is the same trick ExtractResource uses in the other
// direction. Renaming is what gets the file out of the directory now; the
// deletion is then ordered for the next boot, by which time nothing holds it.
Removal RemoveOrRenameAside(const std::wstring& path, std::wstring* asideName) {
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return Removal::Gone;
    if (::DeleteFileW(path.c_str())) return Removal::Gone;
    // A previous run's aside copy may still be sitting there, and still be
    // un-budgeable, so take a fresh name rather than collide with our own litter.
    for (int i = 0; i < 10; ++i) {
        std::wstring old = path + L".old" + (i ? std::to_wstring(i) : std::wstring());
        if (::GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (::MoveFileExW(path.c_str(), old.c_str(), 0)) {
            ::MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            if (asideName) *asideName = old;
            return Removal::RenamedAside;
        }
    }
    return Removal::Failed;
}

// Delete a directory and everything under it, without a progress dialog.
void DeleteTree(const std::wstring& path) {
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring from = path;
    from.push_back(L'\0');  // SHFileOperation wants a double-NUL-terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc  = FO_DELETE;
    op.pFrom  = from.c_str();
    op.fFlags = FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR;
    ::SHFileOperationW(&op);
}

// Does `s` name `dir` itself or something inside it? The Session Manager stores
// NT paths ("\??\C:\Program Files\DS Pinyin IME"), so this is a substring hunt —
// but anchored on a following separator, so a sibling like
// "C:\Program Files\DS Pinyin IME Extra" is not a false positive.
bool MentionsDir(const std::wstring& s, const std::wstring& dir) {
    if (dir.empty() || s.size() < dir.size()) return false;
    const int n = static_cast<int>(dir.size());
    for (size_t at = 0; at + dir.size() <= s.size(); ++at) {
        if (::CompareStringOrdinal(s.c_str() + at, n, dir.c_str(), n, TRUE) != CSTR_EQUAL) continue;
        const size_t end = at + dir.size();
        if (end == s.size() || s[end] == L'\\') return true;
    }
    return false;
}

// Withdraw any delayed delete a previous uninstall queued for this directory.
// ClearInstallDir falls back to MOVEFILE_DELAY_UNTIL_REBOOT when it cannot
// remove the directory — and it never can, because the running installer is
// inside it. That leaves the Session Manager holding a delete for a path a
// later install may well repopulate. Windows is documented to skip a delayed
// directory delete when the directory is not empty, but "uninstalled, then
// reinstalled, then rebooted" must not come down to a documented maybe: being
// wrong means a reboot empties a freshly installed Program Files directory.
void ClearPendingDeletes(const std::wstring& dst) {
    constexpr wchar_t kRunKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    constexpr wchar_t kValue[]  = L"PendingFileRenameOperations";

    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k)
        != ERROR_SUCCESS) {
        return;  // not elevated, or nothing has ever been scheduled
    }
    DWORD type = 0, bytes = 0;
    if (::RegQueryValueExW(k, kValue, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS
        || (type != REG_MULTI_SZ && type != REG_SZ) || bytes < sizeof(wchar_t)) {
        ::RegCloseKey(k);
        return;
    }
    // Ask for the raw bytes; the buffer keeps the terminating NULs, so leave a
    // spare pair of characters for the walk below to run into.
    std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (::RegQueryValueExW(k, kValue, nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &bytes)
        != ERROR_SUCCESS) {
        ::RegCloseKey(k);
        return;
    }

    // A flat list of NUL-terminated strings in (from, to) pairs. A delete is a
    // pair whose `to` is the empty string — which is still one string on the
    // wire, so it costs one separator to step over.
    std::vector<std::wstring> keep;
    bool dropped = false;
    for (const wchar_t* p = buf.data(); *p;) {
        std::wstring from = p;
        p += from.size() + 1;
        std::wstring to;
        if (*p) {
            to = p;
            p += to.size() + 1;
        } else {
            ++p;
        }
        if (MentionsDir(from, dst) || MentionsDir(to, dst)) {
            dropped = true;
            continue;
        }
        keep.push_back(std::move(from));
        keep.push_back(std::move(to));
    }
    if (!dropped) {
        ::RegCloseKey(k);
        return;
    }
    if (keep.empty()) {
        ::RegDeleteValueW(k, kValue);
    } else {
        std::vector<wchar_t> out;
        for (const std::wstring& s : keep) {
            out.insert(out.end(), s.begin(), s.end());
            out.push_back(L'\0');
        }
        out.push_back(L'\0');  // terminate the list
        ::RegSetValueExW(k, kValue, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(out.data()),
                         static_cast<DWORD>(out.size() * sizeof(wchar_t)));
    }
    ::RegCloseKey(k);
}

// Delete the "<name>.old*" files an earlier cycle renamed aside — either a
// previous uninstall, or a previous install replacing a DLL that was still
// loaded. Left alone they are litter that grows by a file per cycle (the legacy
// DSInput directory accumulated ~50), and a single survivor is enough to defeat
// the RemoveDirectoryW that finishes an uninstall. Best-effort throughout: one
// still held open by a running process simply stays.
void SweepRenamedAside(const std::wstring& dst) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW((dst + L"\\*.old*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ::DeleteFileW((dst + L"\\" + fd.cFileName).c_str());
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// Clear the four files the install owns. Returns a human-readable report of what
// happened to the awkward ones; an empty string means everything just went.
std::wstring ClearInstallDir(const std::wstring& dst) {
    const wchar_t* kFiles[] = {
        L"dsime.dll", L"dsime_tsf.dll", L"DSPinyinIMESettings.exe", kInstallerExeName,
    };
    std::wstring aside, stuck;
    for (const wchar_t* f : kFiles) {
        std::wstring name;
        switch (RemoveOrRenameAside(dst + L"\\" + f, &name)) {
            case Removal::RenamedAside:
                if (!aside.empty()) aside += L", ";
                aside += f;
                break;
            case Removal::Failed:
                if (!stuck.empty()) stuck += L", ";
                stuck += f;
                break;
            case Removal::Gone:
                break;
        }
    }
    std::wstring report;
    if (!aside.empty()) {
        report += aside + L" were in use by running apps and have been renamed aside; "
                          L"close those apps to let them go.\r\n";
    }
    if (!stuck.empty()) {
        report += L"Could not remove " + stuck + L" — they will go once the apps holding "
                  L"them are closed.\r\n";
    }
    if (!::RemoveDirectoryW(dst.c_str())) {
        // Nearly always our own exe, still mapped because we are running from it.
        // It is renamed aside above; ask Windows to take the directory at the same
        // point it takes that file, when nothing is holding anything in it.
        ::MoveFileExW(dst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        report += L"The install directory is still there because something in it could "
                  L"not be removed while it was running: " + dst + L"\r\n";
    }
    return report;
}

// Put this exe in the install directory, so the UninstallString in Apps &
// features has something to run that is not wherever the user happened to
// download it from. The payloads are embedded resources, so the copy is the
// whole installer and `--uninstall` works later with no original download.
bool CopySelfInto(const std::wstring& dst, std::wstring* err) {
    wchar_t self[MAX_PATH];
    if (::GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        if (err) *err = L"could not locate the running installer";
        return false;
    }
    std::wstring target = dst + L"\\" + kInstallerExeName;
    if (::_wcsicmp(self, target.c_str()) == 0) return true;  // already running from there
    if (::CopyFileW(self, target.c_str(), FALSE)) return true;
    // In use from a previous install: clear it out of the way and try once more.
    RemoveOrRenameAside(target, nullptr);
    if (::CopyFileW(self, target.c_str(), FALSE)) return true;
    if (err) *err = L"could not copy the installer into " + dst;
    return false;
}

// Record the install so Windows offers to undo it. Best-effort by design: the
// input method works without this, it just has no entry to be found by.
bool WriteUninstallEntry(const std::wstring& dst, std::wstring* err) {
    HKEY k = nullptr;
    LONG rc = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) {
        if (err) *err = L"could not write the Apps & features entry (error "
                        + std::to_wstring(rc) + L")";
        return false;
    }
    std::wstring exe = dst + L"\\" + kInstallerExeName;
    auto setStr = [&](const wchar_t* name, const std::wstring& v) {
        ::RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                         static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    setStr(L"DisplayName", L"DS Pinyin IME");
    setStr(L"DisplayVersion", FileVersionOf(dst + L"\\dsime_tsf.dll"));
    setStr(L"Publisher", L"DS Pinyin IME");
    setStr(L"InstallLocation", dst);
    setStr(L"DisplayIcon", exe);
    setStr(L"UninstallString", L"\"" + exe + L"\" --uninstall");
    setStr(L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --silent");
    // There is nothing here to change or repair, and offering both would only
    // give the user two buttons that both do nothing.
    DWORD one = 1;
    ::RegSetValueExW(k, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    ::RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    ::RegCloseKey(k);
    return true;
}

// ---- the uninstall job ------------------------------------------------------

bool RunUninstall(HWND dlg, bool removeConfig, std::wstring& outMsg) {
    std::wstring dst = ProgramFilesDir() + L"\\" + kInstallDirName;
    std::wstring cfg = AppDataDir() + L"\\" + kConfigDirName;

    // Registration first, files second — the reverse of the install, and for the
    // same reason. Taking the tip out of the language list is what stops Windows
    // trying to activate a text service that is about to stop existing.
    EditLanguageList(dlg, false);

    std::wstring dll = dst + L"\\dsime_tsf.dll";
    if (::GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
        PostProgress(dlg, L"Unregistering the text service…");
        // regsvr32 loads the DLL to reach DllUnregisterServer, so this has to run
        // before the files go. A non-zero code is reported and not treated as
        // fatal: the registration may already be gone (an older run, or an
        // install that never finished) and the files still have to.
        RunWait(L"regsvr32 /s /u \"" + dll + L"\"");
    }

    PostProgress(dlg, L"Removing the program files…");
    // A delete for this directory is about to be queued below; drop the stale one
    // first, so running uninstall twice cannot leave the Session Manager holding
    // two entries for the same path. Then sweep what earlier cycles renamed
    // aside — ClearInstallDir knows only the four names it owns, and one leftover
    // file is enough to defeat the RemoveDirectoryW that finishes the job.
    ClearPendingDeletes(dst);
    SweepRenamedAside(dst);
    std::wstring report = ClearInstallDir(dst);

    ::RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallKey);  // no-op when absent

    if (removeConfig) {
        PostProgress(dlg, L"Deleting your settings…");
        DeleteTree(cfg);
        outMsg = L"DS Pinyin IME is uninstalled, and your settings and API key are gone.\r\n\r\n";
    } else {
        outMsg = L"DS Pinyin IME is uninstalled.\r\n\r\nYour settings and API key are still at "
                 + cfg + L".\r\nReinstalling picks them straight back up; delete that folder to "
                 L"be rid of them.\r\n\r\n";
    }
    outMsg += report;
    outMsg += L"Sign out and back in to clear it from the language bar — until then the keyboard "
              L"can still be listed, with nothing behind it.";
    return true;
}

// ---- the install job (worker thread) ---------------------------------------

// Core install steps. Posts progress to `dlg` (nullptr => silent). Returns
// success and the final user-facing message in `outMsg`.
bool RunInstall(HWND dlg, std::wstring& outMsg) {
    const bool arm64 = HostIsArm64();
    const int rTsf      = arm64 ? IDR_ARM64_TSF      : IDR_X64_TSF;
    const int rCore     = arm64 ? IDR_ARM64_CORE     : IDR_X64_CORE;
    const int rSettings = arm64 ? IDR_ARM64_SETTINGS : IDR_X64_SETTINGS;

    std::wstring dst = ProgramFilesDir() + L"\\" + kInstallDirName;
    std::wstring err;

    PostProgress(dlg, (std::wstring(L"Installing the ") + (arm64 ? L"ARM64" : L"x64")
                       + L" build to " + dst + L"…").c_str());
    ::SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);

    // Installing over an install that was uninstalled first lands on a directory
    // the Session Manager still has a delayed delete queued for, and that is full
    // of the *.old files that uninstall renamed aside. Clear both before writing
    // into it: the files are litter, and that queued delete names the directory
    // we are about to repopulate.
    ClearPendingDeletes(dst);
    SweepRenamedAside(dst);

    bool ok =
        ExtractResource(rCore,     dst + L"\\dsime.dll",          &err) &&
        ExtractResource(rTsf,      dst + L"\\dsime_tsf.dll",      &err) &&
        ExtractResource(rSettings, dst + L"\\DSPinyinIMESettings.exe", &err);

    if (ok) {
        PostProgress(dlg, L"Registering the text service…");
        std::wstring dll = dst + L"\\dsime_tsf.dll";
        DWORD rc = RunWait(L"regsvr32 /s \"" + dll + L"\"");
        if (rc != 0) { ok = false; err = L"regsvr32 failed (code " + std::to_wstring(rc) + L")"; }
    }

    if (ok) {
        EditLanguageList(dlg, true);  // best-effort
        // Recording the uninstaller is best-effort too: the input method works
        // without it, but then nothing in Windows would offer to remove it, which
        // is the state this release exists to fix.
        std::wstring note;
        if (!CopySelfInto(dst, &note) || !WriteUninstallEntry(dst, &note)) {
            note = L"\r\n\r\nNote: the uninstaller could not be recorded (" + note +
                   L"). To remove DS Pinyin IME by hand: regsvr32 /u \"" + dst +
                   L"\\dsime_tsf.dll\", take it out of your Chinese keyboards, and delete "
                   + dst + L".";
        }
        outMsg =
            L"DS Pinyin IME is installed.\r\n\r\nSign out and back in to finish — Windows "
            L"enables a newly registered input method at the next logon. Then switch to "
            L"it with Win+Space and set your API key in DS Pinyin IME Settings." + note;
        return true;
    }
    outMsg = L"Installation failed: " + err;
    return false;
}

struct Job {
    HWND dlg;
    bool uninstall;
    bool removeConfig;
};

DWORD WINAPI WorkerThread(LPVOID param) {
    Job* job = static_cast<Job*>(param);
    HWND dlg = job->dlg;
    const bool uninstall = job->uninstall;
    const bool removeConfig = job->removeConfig;
    delete job;
    std::wstring msg;
    const bool ok = uninstall ? RunUninstall(dlg, removeConfig, msg) : RunInstall(dlg, msg);
    ::PostMessageW(dlg, WM_APP_DONE, ok ? 1 : 0,
                   reinterpret_cast<LPARAM>(::_wcsdup(msg.c_str())));
    return 0;
}

// ---- dialog ----------------------------------------------------------------

void SetBodyWelcome(HWND dlg, bool installed) {
    const bool arm64 = HostIsArm64();
    std::wstring body;
    if (installed) {
        body =
            L"DS Pinyin IME is already installed on this PC.\r\n\r\n"
            L"Uninstall removes the program files, unregisters the text service and takes it "
            L"out of your Chinese (Simplified) keyboards. Your settings and API key are kept "
            L"unless you tick the box above.\r\n\r\n"
            L"Install replaces the program files with the ";
        body += arm64 ? L"ARM64" : L"x64";
        body += L" build, and leaves your settings alone.";
    } else {
        body =
            L"This installs DS Pinyin IME for this PC. You type toneless pinyin and an LLM "
            L"converts the whole sentence to Chinese inline — there is no candidate window.\r\n\r\n"
            L"The installer will:\r\n"
            L"  1.  Copy the ";
        body += arm64 ? L"ARM64" : L"x64";
        body +=
            L" build into Program Files.\r\n"
            L"  2.  Register the text service with Windows.\r\n"
            L"  3.  Add DS Pinyin IME to your Chinese (Simplified) keyboards.\r\n\r\n"
            L"After it finishes, sign out and back in to activate it, then set your API key "
            L"in DS Pinyin IME Settings.";
    }
    ::SetDlgItemTextW(dlg, IDC_BODY, body.c_str());
}

// Lock the dialog, start the marquee, and hand the work to a worker thread: this
// is a filesystem-and-registry job and must not block the message pump.
void StartWorker(HWND dlg, bool uninstall) {
    ::EnableWindow(::GetDlgItem(dlg, IDOK), FALSE);
    ::EnableWindow(::GetDlgItem(dlg, IDC_UNINSTALL), FALSE);
    ::EnableWindow(::GetDlgItem(dlg, IDCANCEL), FALSE);
    ::EnableWindow(::GetDlgItem(dlg, IDC_REMOVE_CONFIG), FALSE);
    HWND pr = ::GetDlgItem(dlg, IDC_PROGRESS);
    ::ShowWindow(pr, SW_SHOW);
    ::SendMessageW(pr, PBM_SETMARQUEE, TRUE, 30);
    const bool removeConfig = ::IsDlgButtonChecked(dlg, IDC_REMOVE_CONFIG) == BST_CHECKED;
    Job* job = new Job{ dlg, uninstall, removeConfig };
    HANDLE t = ::CreateThread(nullptr, 0, WorkerThread, job, 0, nullptr);
    if (t) ::CloseHandle(t);
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            HICON ic = ::LoadIconW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_INSTALLER));
            if (ic) {
                ::SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(ic));
                ::SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(ic));
            }
            // Offer only what is actually possible: Uninstall and the "delete my
            // settings" box mean nothing before there is an install to undo.
            const bool installed = IsInstalled();
            ::ShowWindow(::GetDlgItem(dlg, IDC_UNINSTALL), installed ? SW_SHOW : SW_HIDE);
            ::ShowWindow(::GetDlgItem(dlg, IDC_REMOVE_CONFIG), installed ? SW_SHOW : SW_HIDE);
            if (installed) ::SetWindowTextW(dlg, L"Uninstall DS Pinyin IME");
            SetBodyWelcome(dlg, installed);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    StartWorker(dlg, false);
                    return TRUE;
                case IDC_UNINSTALL:
                    StartWorker(dlg, true);
                    return TRUE;
                case IDCANCEL:
                    ::EndDialog(dlg, 0);
                    return TRUE;
            }
            break;

        case WM_APP_PROGRESS: {
            wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
            if (text) { ::SetDlgItemTextW(dlg, IDC_STATUS, text); ::free(text); }
            return TRUE;
        }

        case WM_APP_DONE: {
            wchar_t* text = reinterpret_cast<wchar_t*>(lParam);
            HWND pr = ::GetDlgItem(dlg, IDC_PROGRESS);
            ::SendMessageW(pr, PBM_SETMARQUEE, FALSE, 0);
            ::ShowWindow(pr, SW_HIDE);
            if (text) { ::SetDlgItemTextW(dlg, IDC_STATUS, text); ::free(text); }
            // Done: every action is spent (installing and uninstalling are not
            // things to do twice in one run), so leave only Close.
            ::ShowWindow(::GetDlgItem(dlg, IDOK), SW_HIDE);
            ::ShowWindow(::GetDlgItem(dlg, IDC_UNINSTALL), SW_HIDE);
            ::ShowWindow(::GetDlgItem(dlg, IDC_REMOVE_CONFIG), SW_HIDE);
            ::EnableWindow(::GetDlgItem(dlg, IDCANCEL), TRUE);
            ::SetDlgItemTextW(dlg, IDCANCEL, L"Close");
            return TRUE;
        }

        case WM_CLOSE:
            ::EndDialog(dlg, 0);
            return TRUE;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int) {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Flags. The UninstallString recorded in Apps & features is
    // `"<install dir>\DSPinyinIMEInstaller.exe" --uninstall`; its quiet twin adds
    // --silent. Matching is case-insensitive and the old /S and /silent spellings
    // still work.
    //   --uninstall / -u     remove instead of install
    //   --silent    / -s     no UI; 0 on success, 1 on failure
    //   --remove-config      uninstall also deletes the settings and API key
    std::wstring cmd = lpCmdLine ? lpCmdLine : L"";
    for (wchar_t& c : cmd) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    auto has = [&cmd](const wchar_t* flag) { return cmd.find(flag) != std::wstring::npos; };
    const bool uninstall    = has(L"--uninstall") || has(L"/u");
    const bool silent       = has(L"--silent") || has(L"/s");
    const bool removeConfig = has(L"--remove-config") || has(L"/removeconfig");

    if (silent) {
        std::wstring msg;
        const bool ok = uninstall ? RunUninstall(nullptr, removeConfig, msg) : RunInstall(nullptr, msg);
        ::CoUninitialize();
        return ok ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);
    ::DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_INSTALLER), nullptr, DlgProc, 0);
    ::CoUninitialize();
    return 0;
}

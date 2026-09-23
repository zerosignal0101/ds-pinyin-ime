# uninstall-dspinyinime.ps1 — elevated removal of the DS Pinyin IME TSF text service.
#
# The reverse of install-dspinyinime.ps1, run in the opposite order for the same
# reason the install runs it forwards: registration first, files second.
# regsvr32 /u has to load the DLL to reach DllUnregisterServer, so the DLL must
# still be there when it runs — and Windows must have stopped trying to activate
# the text service before either, which is what taking the TIP id out of the
# language list does.
#
#   1. Remove the TIP id from the user's zh language list.
#   2. regsvr32 /u: DllUnregisterServer drops HKCR\CLSID InprocServer32, the TSF
#      profile and its categories.
#   3. Delete the install directory. The DLLs are loaded by every running app
#      that has the IME active, so the installer's rename-aside applies in
#      reverse: a loaded image can be *renamed* on NTFS but never deleted or
#      overwritten while its process lives.
#   4. Drop the Apps & features entry.
#
# Your settings are KEPT unless -RemoveConfig is passed. They hold the API key
# and everything the Settings window edits, and "uninstall" is not the same
# request as "forget what I typed" — the Settings window has its own button for
# that. A reinstall picks the file straight back up.
#
# Must run ELEVATED. Run:  ./windows/uninstall-dspinyinime.ps1
#                   or:   ./windows/uninstall-dspinyinime.ps1 -RemoveConfig

[CmdletBinding()]
param(
    # Where install-dspinyinime.ps1 put the trio.
    [string]$InstallDir = (Join-Path $env:ProgramFiles "DS Pinyin IME"),
    # Also delete %APPDATA%\DSPinyinIME — settings and API key included. Off by
    # default; see the header for why.
    [switch]$RemoveConfig
)

$ErrorActionPreference = "Stop"

$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) { throw "This script must be run elevated (as Administrator)." }

$log = Join-Path $env:TEMP "dspinyinime-uninstall.log"
"== DS Pinyin IME uninstall log ==" | Set-Content -Path $log
function Log($m) { $m | Tee-Object -FilePath $log -Append | Out-Null; Write-Host $m }

# ── 1. Take the TIP out of the language list ────────────────────────────────
# "0804:{CLSID}{PROFILE}" — must match windows/Guids.h. Leaving this behind is
# what makes a removed IME keep showing up in Win+Space as a dead entry.
$tip = '0804:{6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}{A1B2C3D4-55E6-47F8-8901-23456789ABCD}'
Log "== Removing DS Pinyin IME from the Chinese language list"
$ll = Get-WinUserLanguageList
$changed = $false
foreach ($lang in ($ll | Where-Object { $_.LanguageTag -like 'zh*' })) {
    if ($lang.InputMethodTips -contains $tip) {
        [void]$lang.InputMethodTips.Remove($tip)
        Log "   removed from '$($lang.LanguageTag)'"
        $changed = $true
    }
}
if ($changed) {
    Set-WinUserLanguageList $ll -Force
} else {
    Log "   not present in any zh language list"
}

# ── 2. Unregister the COM/TSF in-proc server ────────────────────────────────
$dll = Join-Path $InstallDir "dsime_tsf.dll"
if (Test-Path -LiteralPath $dll) {
    Log "== Unregistering the text service"
    # 64-bit regsvr32 (System32) to match the 64-bit DLL; /s keeps it quiet.
    $regsvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
    $p = Start-Process $regsvr -ArgumentList "/s", "/u", "`"$dll`"" -Wait -PassThru
    Log "   regsvr32 /u exit code: $($p.ExitCode)"
    if ($p.ExitCode -ne 0) {
        # Not fatal: the registration may already be gone (an older run, or an
        # install that never completed) and the files still have to go.
        Log "   NOTE: regsvr32 reported $($p.ExitCode) — continuing anyway"
    }
} else {
    Log "== No dsime_tsf.dll at $dll — nothing to unregister"
}

# ── 3. Delete the install directory ─────────────────────────────────────────
Log "== Removing $InstallDir"
# The installer self-copies, so it is in here too and is the one file that is
# always running while we try to delete it.
$files = @("dsime.dll", "dsime_tsf.dll", "DSPinyinIMESettings.exe", "DSPinyinIMEInstaller.exe")
$aside = @()
foreach ($f in $files) {
    $target = Join-Path $InstallDir $f
    if (-not (Test-Path -LiteralPath $target)) { continue }
    try {
        Remove-Item -LiteralPath $target -Force -ErrorAction Stop
        Log "   removed $f"
    } catch {
        # Loaded by a running process. Rename it aside instead — the process
        # keeps its handle and keeps working from the renamed file, exactly as
        # the installer does when it replaces a live DLL. A previous run's
        # aside-copy may still be sitting there and still be un-budgeable, so
        # take a fresh name rather than failing on a collision with it.
        $old = "$target.old"
        Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $old) {
            $old = "$target.old." + [guid]::NewGuid().ToString("N").Substring(0, 8)
        }
        try {
            Move-Item -LiteralPath $target -Destination $old -Force
            $aside += (Split-Path -Leaf $old)
            Log "   $f is in use — renamed aside to $(Split-Path -Leaf $old)"
        } catch {
            Log "   could not remove or rename $f — it will go at the next reboot"
        }
    }
}
if ($aside.Count -gt 0) {
    Log "   NOTE: $($aside -join ', ') can be deleted once the apps holding"
    Log "         them are closed; they are unreachable until then."
}

if (Test-Path -LiteralPath $InstallDir) {
    try {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force -ErrorAction Stop
        Log "   removed the install directory"
    } catch {
        Log "   install directory kept — it still holds:"
        Get-ChildItem -LiteralPath $InstallDir -ErrorAction SilentlyContinue |
            ForEach-Object { Log "     $($_.Name)" }
    }
}

# ── 4. Drop the Apps & features entry ───────────────────────────────────────
$uninstallKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DSPinyinIME"
if (Test-Path $uninstallKey) {
    Remove-Item $uninstallKey -Recurse -Force
    Log "== Removed the Apps & features entry"
}

# ── 5. Settings, only when asked ────────────────────────────────────────────
$configRoot = Join-Path $env:APPDATA "DSPinyinIME"
if ($RemoveConfig) {
    if (Test-Path -LiteralPath $configRoot) {
        Remove-Item -LiteralPath $configRoot -Recurse -Force
        Log "== Deleted $configRoot (settings and API key)"
    } else {
        Log "== No settings at $configRoot"
    }
} elseif (Test-Path -LiteralPath $configRoot) {
    Log "== Kept your settings at $configRoot"
    Log "   (pass -RemoveConfig to delete them as well)"
}

Log ""
Log "DONE — sign out and back in to finish; until then the keyboard can still"
Log "       be listed even though it no longer has anything behind it."

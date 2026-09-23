# install-dspinyinime.ps1 — elevated install of the DS Pinyin IME TSF text service (x64).
#
# Mirrors what windows/installer/DSPinyinIMEInstaller.cpp does at runtime:
#   1. Copy the trio (dsime.dll, dsime_tsf.dll, DSPinyinIMESettings.exe) into
#      %ProgramFiles%\DS Pinyin IME — all three must co-locate (the DLLs load dsime.dll
#      at runtime), and regsvr32 records the exact path it registers from.
#   2. regsvr32 the text service: DllRegisterServer writes HKCR\CLSID
#      InprocServer32 + the TSF profile/categories for zh-Hans (0x0804).
#   3. Add the TIP id to the user's zh language list so it is selectable with
#      Win+Space after the next sign-in.
#
# Must run ELEVATED. Run:  ./windows/install-dspinyinime.ps1

[CmdletBinding()]
param(
    # Source of the staged build (build.ps1 output).
    [string]$Src = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "dist\x64"),
    [string]$Dst = (Join-Path $env:ProgramFiles "DS Pinyin IME")
)

$ErrorActionPreference = "Stop"

$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) { throw "This script must be run elevated (as Administrator)." }

$log = Join-Path $env:TEMP "dspinyinime-install.log"
"== DS Pinyin IME install log ==" | Set-Content -Path $log
function Log($m) { $m | Tee-Object -FilePath $log -Append | Out-Null; Write-Host $m }

# ── Leftovers from an earlier install ───────────────────────────────────────
# Mirrors ClearPendingDeletes/SweepRenamedAside in installer/DSPinyinIMEInstaller.cpp,
# and for the same reasons. A file that was loaded while being replaced gets
# renamed aside (*.old*); a directory that cannot be deleted — because the exe
# doing the deleting is inside it — goes to the Session Manager as a delayed
# delete. So reinstalling before the next reboot lands on a directory the OS
# still has a delete queued for, and full of *.old files, any one of which is
# enough to defeat the directory removal that finishes an uninstall. The litter
# compounds a file per cycle (the legacy DSInput directory reached ~50).
function Test-MentionsDir {
    param([string]$S, [string]$Dir)
    if (-not $S -or -not $Dir) { return $false }
    foreach ($m in [regex]::Matches($S, [regex]::Escape($Dir), 'IgnoreCase')) {
        # Anchored on a following separator, so a sibling directory whose name
        # merely begins with ours ("DS Pinyin IME Extra") is not a false hit.
        $end = $m.Index + $m.Length
        if ($end -eq $S.Length -or $S[$end] -eq '\') { return $true }
    }
    return $false
}

# Drop every reboot-delete naming $Dir. Returns how many pairs went.
function Clear-PendingDeletes {
    param([string]$Dir)
    $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager"
    $p = (Get-ItemProperty $key -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    if (-not $p) { return 0 }
    # A flat list of (name, new-name) pairs; a delete is a pair whose second
    # string is empty. Pairs unrelated to $Dir are left exactly as they were.
    $keep = [System.Collections.Generic.List[string]]::new()
    $dropped = 0
    for ($i = 0; $i -lt $p.Count; $i += 2) {
        $from = $p[$i]
        $to = if ($i + 1 -lt $p.Count) { $p[$i + 1] } else { "" }
        if ((Test-MentionsDir $from $Dir) -or (Test-MentionsDir $to $Dir)) { $dropped++; continue }
        $keep.Add($from); $keep.Add($to)
    }
    if ($dropped -eq 0) { return 0 }
    if ($keep.Count -eq 0) {
        Remove-ItemProperty -Path $key -Name PendingFileRenameOperations -ErrorAction SilentlyContinue
    } else {
        Set-ItemProperty -Path $key -Name PendingFileRenameOperations -Value ([string[]]$keep) -Type MultiString
    }
    return $dropped
}

# Delete the *.old* files earlier cycles renamed aside. Best-effort: one still
# held open by a running process simply stays. Returns how many went.
function Remove-StaleAside {
    param([string]$Dir)
    $n = 0
    Get-ChildItem -LiteralPath $Dir -Filter "*.old*" -Force -File -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction Stop; $n++ } catch { }
    }
    return $n
}

$files = @("dsime.dll", "dsime_tsf.dll", "DSPinyinIMESettings.exe")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $Src $f))) { throw "missing $f under $Src — run build.ps1 first." }
}

# ── 1. Copy the trio ────────────────────────────────────────────────────────
Log "== Installing to $Dst"
New-Item -ItemType Directory -Force -Path $Dst | Out-Null
# Installing over an install that was uninstalled first lands on a directory the
# Session Manager still has a delayed delete queued for, and that is full of the
# *.old files uninstall renamed aside. Clear both before writing into it: the
# files are litter, and that queued delete names the directory we are about to
# repopulate.
$dropped = Clear-PendingDeletes $Dst
if ($dropped -gt 0) { Log "   withdrew $dropped stale reboot-delete entry/entries" }
$swept = Remove-StaleAside $Dst
if ($swept -gt 0) { Log "   swept $swept leftover *.old file(s)" }
$swapped = @()
foreach ($f in $files) {
    $source = Join-Path $Src $f
    $target = Join-Path $Dst $f
    try {
        Copy-Item $source $target -Force -ErrorAction Stop
    } catch {
        # The DLLs are already loaded by every running app that has the IME
        # active (explorer, browsers, editors, …), so overwriting them fails
        # while those processes live. On NTFS the loaded image can be renamed
        # aside — the running process keeps its handle and keeps working from the
        # renamed file — which is exactly how the guided installer handles this.
        Log "   $f is in use — renaming the loaded copy aside"
        # A loaded image can be *renamed* but never deleted or overwritten, so a
        # previous run's aside-copy is still sitting there and still cannot be
        # budged while its process lives — the name stays taken until reboot.
        # Try to reclaim it, and if we can't, take a fresh name instead of
        # failing the whole install on a collision with our own leftovers.
        $old = "$target.old"
        Remove-Item $old -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $old) {
            $old = "$target.old." + [guid]::NewGuid().ToString("N").Substring(0, 8)
        }
        Move-Item -LiteralPath $target -Destination $old -Force
        Copy-Item $source $target -Force
        $swapped += $f
    }
    Log "   copied $f"
}
if ($swapped.Count -gt 0) {
    Log "   NOTE: $($swapped -join ', ') were in use; apps holding them keep the"
    Log "         OLD build until they are restarted (or you sign out)."
    Log "         The renamed originals (*.old*) can be deleted once those"
    Log "         processes are gone — they are unreachable before that."
}

# ── 2. Register the COM/TSF in-proc server ─────────────────────────────────
# 64-bit regsvr32 (System32) for the x64 DLL; /s keeps it non-interactive.
Log "== Registering the text service"
$regsvr = Join-Path $env:SystemRoot "System32\regsvr32.exe"
$p = Start-Process $regsvr -ArgumentList "/s", "`"$Dst\dsime_tsf.dll`"" -Wait -PassThru
Log "   regsvr32 exit code: $($p.ExitCode)"
if ($p.ExitCode -ne 0) { throw "regsvr32 failed (exit $($p.ExitCode))" }

# ── 3. Add the TIP to the zh language list ──────────────────────────────────
# "0804:{CLSID}{PROFILE}" — must match windows/Guids.h.
$tip = '0804:{6F3D9A21-7C44-4E1B-9C2A-1B2C3D4E5F60}{A1B2C3D4-55E6-47F8-8901-23456789ABCD}'
Log "== Adding DS Pinyin IME to the Chinese language list"
$ll = Get-WinUserLanguageList
$zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1
if (-not $zh) {
    $ll.Add('zh-Hans-CN')
    $zh = $ll | Where-Object { $_.LanguageTag -like 'zh*' } | Select-Object -First 1
}
if ($zh.InputMethodTips -notcontains $tip) {
    $zh.InputMethodTips.Add($tip)
    Set-WinUserLanguageList $ll -Force
    Log "   added to '$($zh.LanguageTag)'"
} else {
    Log "   already present on '$($zh.LanguageTag)'"
}

Log "DONE — sign out and back in to activate, then pick DS Pinyin IME with Win+Space."

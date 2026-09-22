# install-dsinput.ps1 — elevated install of the DS Input TSF text service (x64).
#
# Mirrors what windows/installer/DSInputInstaller.cpp does at runtime:
#   1. Copy the trio (dsime.dll, dsime_tsf.dll, DSInputSettings.exe) into
#      %ProgramFiles%\DSInput — all three must co-locate (the DLLs load dsime.dll
#      at runtime), and regsvr32 records the exact path it registers from.
#   2. regsvr32 the text service: DllRegisterServer writes HKCR\CLSID
#      InprocServer32 + the TSF profile/categories for zh-Hans (0x0804).
#   3. Add the TIP id to the user's zh language list so it is selectable with
#      Win+Space after the next sign-in.
#
# Must run ELEVATED. Run:  ./windows/install-dsinput.ps1

[CmdletBinding()]
param(
    # Source of the staged build (build.ps1 output).
    [string]$Src = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "dist\x64"),
    [string]$Dst = (Join-Path $env:ProgramFiles "DSInput")
)

$ErrorActionPreference = "Stop"

$elevated = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) { throw "This script must be run elevated (as Administrator)." }

$log = Join-Path $env:TEMP "dsinput-install.log"
"== DS Input install log ==" | Set-Content -Path $log
function Log($m) { $m | Tee-Object -FilePath $log -Append | Out-Null; Write-Host $m }

$files = @("dsime.dll", "dsime_tsf.dll", "DSInputSettings.exe")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $Src $f))) { throw "missing $f under $Src — run build.ps1 first." }
}

# ── 1. Copy the trio ────────────────────────────────────────────────────────
Log "== Installing to $Dst"
New-Item -ItemType Directory -Force -Path $Dst | Out-Null
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
        $old = "$target.old"
        Log "   $f is in use — renaming the loaded copy aside"
        Remove-Item $old -Force -ErrorAction SilentlyContinue
        Move-Item $target $old -Force
        Copy-Item $source $target -Force
        $swapped += $f
    }
    Log "   copied $f"
}
if ($swapped.Count -gt 0) {
    Log "   NOTE: $($swapped -join ', ') were in use; apps holding them keep the"
    Log "         OLD build until they are restarted (or you sign out)."
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
Log "== Adding DS Input to the Chinese language list"
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

Log "DONE — sign out and back in to activate, then pick DS Input with Win+Space."

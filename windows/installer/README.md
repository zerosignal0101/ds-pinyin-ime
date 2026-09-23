# DS Pinyin IME — Windows installer

A single, self-contained, elevated **universal installer** (`DSPinyinIMEInstaller.exe`)
for the DS Pinyin IME TSF text service — the Windows counterpart to the macOS guided installer.

It **embeds both the x64 and ARM64 builds**, detects the host architecture at run
time, installs the matching set, registers the text service, and adds DS Pinyin IME to
the user's Chinese (Simplified) keyboards.

> Windows has no fat/universal binaries: a TSF text service is an in-proc COM DLL
> loaded into each text-host process and must match that process's architecture.
> This installer ships per-arch DLLs and installs the one matching the host OS, so
> one download works on both x64 and ARM64 PCs. (Serving x64-emulated apps on
> ARM64, or 32-bit apps on x64, would need the extra-arch DLL / an ARM64X hybrid —
> a future enhancement.)

## What it does

1. Extracts the host-arch payload (`dsime_tsf.dll`, `dsime.dll`,
   `DSPinyinIMESettings.exe`) to `%ProgramFiles%\DS Pinyin IME`.
2. `regsvr32` the TSF DLL (the native-arch `regsvr32` matches the native-arch DLL).
3. Adds the DS Pinyin IME profile to the user's `zh-Hans` language list.
4. Copies *itself* into the install directory and records an
   `HKLM\…\Uninstall\DSPinyinIME` entry, so the app can be removed from
   **Settings ▸ Apps** rather than only by hand. Steps 3 and 4 are best-effort —
   the input method works without them, it just has no entry to be found by.
5. Tells the user to sign out / back in — Windows enrolls a freshly registered
   text service at the next logon.

Run when DS Pinyin IME is already installed, the same window offers **Uninstall**
as well — see *Run*.

## Build

From a VS 2022 environment with both MSVC toolsets + the Rust MSVC targets:

```powershell
cd windows
./installer/build-installer.ps1            # runs build.ps1 -Arch all, then packages
# -> installer/build/Release/DSPinyinIMEInstaller.exe
```

`build-installer.ps1 -SkipBuild` repackages whatever is already staged in
`windows/dist/<arch>/` (handy when iterating on the installer itself).

## Run

Double-click `DSPinyinIMEInstaller.exe` (it prompts for administrator). If DS
Pinyin IME is already installed the window offers **Uninstall** too, with a box
for deleting your settings along with it.

For automation or headless verification:

```powershell
DSPinyinIMEInstaller.exe /S        # install, no UI; exit 0 = success, 1 = failure
DSPinyinIMEInstaller.exe --uninstall                            # the same window, uninstall mode
DSPinyinIMEInstaller.exe --uninstall --silent                   # uninstall, no UI
DSPinyinIMEInstaller.exe --uninstall --silent --remove-config   # …and delete the settings
```

`--uninstall` is what the Apps & features entry records as its `UninstallString`,
pointing at the copy of this exe that the install leaves in the install
directory. Flags are case-insensitive, and the older `/S` and `/silent`
spellings still work.

## Files

| File | Role |
|------|------|
| `DSPinyinIMEInstaller.cpp` | Wizard UI + install/uninstall logic (extract, register, language list, self-copy, Apps & features entry); silent and `--uninstall` modes. |
| `DSPinyinIMEInstaller.rc` | Dialog template, icon, embedded manifest, and the embedded per-arch payloads. |
| `installer.manifest` | `requireAdministrator` + common-controls + DPI awareness. |
| `CMakeLists.txt` | Builds the installer exe (x64) with `/MANIFEST:NO` (manifest comes from the .rc). |
| `build-installer.ps1` | Builds both arches then packages the installer. |

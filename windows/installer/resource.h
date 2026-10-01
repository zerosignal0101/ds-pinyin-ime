// resource.h — ids for the DS Pinyin IME universal installer.
#pragma once

#define IDI_INSTALLER       1

#define IDD_INSTALLER       100
#define IDC_TITLE           101
#define IDC_BODY            102
#define IDC_STATUS          103
#define IDC_PROGRESS        104
#define IDC_UNINSTALL       105  // shown only when something is installed
#define IDC_REMOVE_CONFIG   106  // "also delete my settings" — uninstall only
// The action button is IDOK ("Install" → "Close"); IDCANCEL is the close box.

// Embedded payloads (RCDATA). One trio per architecture; the installer extracts
// only the set matching the host. A missing arch is simply absent at runtime.
#define IDR_X64_TSF         300
#define IDR_X64_CORE        301
#define IDR_X64_SETTINGS    302
#define IDR_ARM64_TSF       310
#define IDR_ARM64_CORE      311
#define IDR_ARM64_SETTINGS  312

// The compiled pinyin dictionary (dsime.lex). ONE copy, not one per arch: it is a
// table of UTF-8 words, identical for every machine, and it is 22 MB — embedding
// it twice would put 22 MB on every install for nothing.
//
// It is also not optional. Without it the IME still runs, and runs exactly as it
// did before dictionary support existed: no segmentation, no candidate row, and the
// digits 2-9 back to being ordinary characters. Nothing reports that, which is
// why a release built without it looks fine and is in fact an IME with the main
// feature silently missing.
#define IDR_DICT            320

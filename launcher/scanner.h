/* scanner.h — System diagnostics and auto-download for Sprout Launcher V2.
 * A background thread probes each requirement and posts messages to the notify window.
 *
 * Messages posted:
 *   WM_SCAN_UPDATE  : wParam = ScanId (SCAN_COUNT = all done)
 *   WM_DL_PROGRESS  : wParam = ScanId, lParam = percent 0-100
 *   WM_DL_DONE      : wParam = ScanId, lParam = 1 success / 0 fail
 */
#pragma once
#ifndef SPROUT_SCANNER_H
#define SPROUT_SCANNER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ScanStatus_e {
    SCAN_CHECKING   = 0,
    SCAN_OK         = 1,
    SCAN_WARN       = 2,
    SCAN_MISSING    = 3,
    SCAN_DOWNLOAD   = 4,
    SCAN_DONE_OK    = 5,
    SCAN_DONE_FAIL  = 6,
} ScanStatus;

typedef enum ScanId_e {
    SCAN_OPENGL = 0,
    SCAN_MESA3D = 1,
    SCAN_VCRT   = 2,
    SCAN_RAM    = 3,
    SCAN_DISK   = 4,
    SCAN_OS     = 5,
    SCAN_SIMD   = 6,
    SCAN_COUNT  = 7,
} ScanId;

typedef struct ScanResult_s {
    ScanId     id;
    ScanStatus status;
    wchar_t    label[64];
    wchar_t    detail[256];
    int        can_download;
    int        download_pct;
    BOOL       recommends_software;
} ScanResult;

#define WM_SCAN_UPDATE  (WM_APP + 1)
#define WM_DL_PROGRESS  (WM_APP + 2)
#define WM_DL_DONE      (WM_APP + 3)

void scan_start(HWND hwnd, const char *base_dir);
void scan_get(ScanResult out[SCAN_COUNT]);
int  scan_is_done(void);
const char *scan_recommended_renderer(void);
void scan_download_mesa(HWND hwnd, const char *base_dir);
void scan_download_vcrt(HWND hwnd, const char *base_dir);
BOOL scan_import_mesa(HWND hwnd, const char *base_dir, const wchar_t *src_path);

#ifdef __cplusplus
}
#endif

#endif /* SPROUT_SCANNER_H */

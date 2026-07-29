/* scanner.cpp — background diagnostics + WinHTTP auto-download for Sprout Launcher V2.
 * Low-end-PC safe: pure GDI painting, minimal allocations, all Win32 API only.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "scanner.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "opengl32.lib")

/* ---------- internal state ------------------------------------------ */

static CRITICAL_SECTION g_cs;
static ScanResult        g_results[SCAN_COUNT];
static volatile LONG     g_scan_started  = 0;
static volatile LONG     g_scan_done     = 0;
static volatile LONG     g_dl_mesa_busy  = 0;
static volatile LONG     g_dl_vcrt_busy  = 0;
static char              g_base[512];
static HWND              g_notify        = NULL;

/* ---------- helpers -------------------------------------------------- */

static void result_init(void) {
    static const wchar_t *names[SCAN_COUNT] = {
        L"OpenGL Driver", L"Mesa3D (Software Renderer)", L"VC++ Runtime",
        L"Memory (RAM)", L"Disk Space", L"Operating System", L"CPU Extensions"
    };
    for (int i = 0; i < SCAN_COUNT; i++) {
        g_results[i].id = (ScanId)i;
        g_results[i].status = SCAN_CHECKING;
        wcsncpy_s(g_results[i].label, 64, names[i], _TRUNCATE);
        wcsncpy_s(g_results[i].detail, 256, L"Checking...", _TRUNCATE);
        g_results[i].can_download = 0;
        g_results[i].download_pct = 0;
        g_results[i].recommends_software = FALSE;
    }
}

static void result_set(ScanId id, ScanStatus st, const wchar_t *detail, int can_dl) {
    EnterCriticalSection(&g_cs);
    g_results[id].status = st;
    wcsncpy_s(g_results[id].detail, 256, detail, _TRUNCATE);
    g_results[id].can_download = can_dl;
    LeaveCriticalSection(&g_cs);
    if (g_notify) PostMessage(g_notify, WM_SCAN_UPDATE, (WPARAM)id, 0);
}

static int file_exists_a(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* ---------- OpenGL probe (creates invisible dummy window+context) ---- */

static int probe_opengl_version(void) {
    WNDCLASSA wc = {0};
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "SproutGLProbeWnd";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("SproutGLProbeWnd", "", WS_OVERLAPPEDWINDOW,
                              0, 0, 16, 16, NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { UnregisterClassA("SproutGLProbeWnd", GetModuleHandleA(NULL)); return 0; }

    HDC hdc = GetDC(hwnd);
    int version = 0;

    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pf = ChoosePixelFormat(hdc, &pfd);
    if (pf && SetPixelFormat(hdc, pf, &pfd)) {
        HGLRC ctx = wglCreateContext(hdc);
        if (ctx) {
            if (wglMakeCurrent(hdc, ctx)) {
                typedef const unsigned char *(WINAPI *glGetStringFn)(unsigned int);
                HMODULE gl = GetModuleHandleA("opengl32.dll");
                if (!gl) gl = LoadLibraryA("opengl32.dll");
                if (gl) {
                    glGetStringFn fn = (glGetStringFn)GetProcAddress(gl, "glGetString");
                    if (fn) {
                        const char *ver_str = (const char *)fn(0x1F02 /* GL_VERSION */);
                        const char *ren_str = (const char *)fn(0x1F01 /* GL_RENDERER */);
                        if (ver_str && ren_str) {
                            /* Check if Microsoft GDI Generic software OpenGL 1.1 */
                            if (strstr(ren_str, "GDI Generic") != NULL || strstr(ren_str, "Microsoft") != NULL) {
                                version = 1;
                            } else if (ver_str[0] >= '2' && ver_str[0] <= '9') {
                                version = ver_str[0] - '0';
                            } else {
                                version = 1;
                            }
                        }
                    }
                }
            }
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(ctx);
        }
    }
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    UnregisterClassA("SproutGLProbeWnd", GetModuleHandleA(NULL));
    return version;
}

/* ---------- SIMD / CPU ------------------------------------------- */

static int cpu_has_avx2(void) {
    int info[4] = {0};
    __cpuid(info, 0);
    if (info[0] < 7) return 0;
    __cpuidex(info, 7, 0);
    return (info[1] >> 5) & 1;
}

static int cpu_has_sse41(void) {
    int info[4] = {0};
    __cpuid(info, 1);
    return (info[2] >> 19) & 1;
}

/* ---------- OS version ------------------------------------------- */

static int win_major_version(void) {
    typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOEXW *);
    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    int major = 6;
    if (ntdll) {
        RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            OSVERSIONINFOEXW inf = {sizeof(inf)};
            if (fn(&inf) == 0) major = (int)inf.dwMajorVersion;
        }
        FreeLibrary(ntdll);
    }
    return major;
}

/* ---------- Scanner thread --------------------------------------- */

static DWORD WINAPI scanner_thread(LPVOID unused) {
    wchar_t det[300];

    /* 1. OpenGL */
    int glver = probe_opengl_version();
    EnterCriticalSection(&g_cs);
    g_results[SCAN_OPENGL].recommends_software = (glver < 2) ? TRUE : FALSE;
    LeaveCriticalSection(&g_cs);
    if (glver >= 2) {
        swprintf_s(det, 300, L"OpenGL %d detected - GPU acceleration available.", glver);
        result_set(SCAN_OPENGL, SCAN_OK, det, 0);
    } else if (glver == 1) {
        result_set(SCAN_OPENGL, SCAN_WARN,
            L"OpenGL 1.x / Generic driver - GPU too old. Software renderer recommended.", 0);
    } else {
        result_set(SCAN_OPENGL, SCAN_MISSING,
            L"No hardware OpenGL found - Software renderer (Mesa3D) is required.", 0);
    }

    /* 2. Mesa3D DLLs */
    char mesa_dll[600], mesa_lip[600], mesa_llvm[600];
    _snprintf_s(mesa_dll,  sizeof(mesa_dll),  _TRUNCATE, "%s\\mesa\\opengl32.dll",            g_base);
    _snprintf_s(mesa_lip,  sizeof(mesa_lip),  _TRUNCATE, "%s\\mesa\\mesa_driver_llvmpipe.dll", g_base);
    _snprintf_s(mesa_llvm, sizeof(mesa_llvm), _TRUNCATE, "%s\\mesa\\LLVM-C.dll",               g_base);

    int has_dll  = file_exists_a(mesa_dll);
    int has_lip  = file_exists_a(mesa_lip);
    int has_llvm = file_exists_a(mesa_llvm);

    if (has_dll && has_lip && has_llvm) {
        result_set(SCAN_MESA3D, SCAN_OK, L"All Mesa3D DLLs present - Software renderer ready.", 0);
    } else {
        wchar_t missing[256] = L"Missing:";
        if (!has_dll)  wcscat_s(missing, 256, L" opengl32.dll");
        if (!has_lip)  wcscat_s(missing, 256, L" mesa_driver_llvmpipe.dll");
        if (!has_llvm) wcscat_s(missing, 256, L" LLVM-C.dll");
        result_set(SCAN_MESA3D, SCAN_MISSING, missing, 1);
    }

    /* 3. VC++ Runtime */
    {
        HMODULE vcrt = LoadLibraryA("VCRUNTIME140.dll");
        if (vcrt) {
            FreeLibrary(vcrt);
            result_set(SCAN_VCRT, SCAN_OK,
                L"Visual C++ 2019/2022 Runtime installed.", 0);
        } else {
            result_set(SCAN_VCRT, SCAN_MISSING,
                L"VCRUNTIME140.dll not found - required for sprout.exe to run.", 1);
        }
    }

    /* 4. RAM */
    {
        MEMORYSTATUSEX ms = {sizeof(ms)};
        GlobalMemoryStatusEx(&ms);
        ULONGLONG total_mb = ms.ullTotalPhys  / (1024*1024);
        ULONGLONG avail_mb = ms.ullAvailPhys  / (1024*1024);
        swprintf_s(det, 300, L"%llu MB total, %llu MB available", total_mb, avail_mb);
        ScanStatus st = total_mb >= 2048 ? SCAN_OK : (total_mb >= 768 ? SCAN_WARN : SCAN_MISSING);
        if (total_mb < 768)  wcscat_s(det, 300, L" - Very low RAM, game will likely crash.");
        else if (total_mb < 2048) wcscat_s(det, 300, L" - Low RAM, may cause slowdowns.");
        result_set(SCAN_RAM, st, det, 0);
    }

    /* 5. Disk space */
    {
        char drive[4] = { g_base[0], ':', '\\', 0 };
        ULARGE_INTEGER free_b;
        if (GetDiskFreeSpaceExA(drive, &free_b, NULL, NULL)) {
            ULONGLONG free_mb = free_b.QuadPart / (1024*1024);
            if (free_mb >= 1024)
                swprintf_s(det, 300, L"%.1f GB free on %C:", free_mb / 1024.0, drive[0]);
            else
                swprintf_s(det, 300, L"%llu MB free on %C:", free_mb, drive[0]);
            ScanStatus st = free_mb >= 2048 ? SCAN_OK : (free_mb >= 500 ? SCAN_WARN : SCAN_MISSING);
            if (free_mb < 500) wcscat_s(det, 300, L" - Critical: insufficient disk space.");
            result_set(SCAN_DISK, st, det, 0);
        } else {
            result_set(SCAN_DISK, SCAN_WARN, L"Could not determine free disk space.", 0);
        }
    }

    /* 6. OS version */
    {
        int major = win_major_version();
        swprintf_s(det, 300, L"Windows %d", major);
        ScanStatus st;
        if (major >= 10)      { wcscat_s(det, 300, L" - Fully supported.");               st = SCAN_OK;      }
        else if (major >= 6)  { wcscat_s(det, 300, L" - Works, but Win10+ recommended."); st = SCAN_WARN;    }
        else                  { wcscat_s(det, 300, L" - Windows 7 or newer required.");   st = SCAN_MISSING; }
        result_set(SCAN_OS, st, det, 0);
    }

    /* 7. SIMD */
    {
        int avx2  = cpu_has_avx2();
        int sse41 = cpu_has_sse41();
        if (avx2) {
            result_set(SCAN_SIMD, SCAN_OK,
                L"AVX2 + SSE4.1 - Mesa3D LLVMpipe will run at full speed.", 0);
        } else if (sse41) {
            result_set(SCAN_SIMD, SCAN_WARN,
                L"SSE4.1 only (no AVX2) - Mesa3D will work, may be slower on complex scenes.", 0);
        } else {
            result_set(SCAN_SIMD, SCAN_WARN,
                L"No SSE4.1/AVX2 detected - Mesa3D may not function on this CPU.", 0);
        }
    }

    InterlockedExchange(&g_scan_done, 1);
    if (g_notify) PostMessage(g_notify, WM_SCAN_UPDATE, SCAN_COUNT, 0);
    return 0;
}

/* ---------- WinHTTP download helper ------------------------------ */

typedef struct DlCtx_s {
    HWND     hwnd;
    ScanId   scan_id;
    wchar_t  server[200];
    wchar_t  path[512];
    wchar_t  local_file[512];
    char     base_dir[512];
    int      run_after;
} DlCtx;

static BOOL winhttp_download(const wchar_t *server, const wchar_t *url_path,
                             const wchar_t *out_file,
                             ScanId scan_id, HWND hwnd) {
    HINTERNET sess = NULL, conn = NULL, req = NULL;
    HANDLE    fh   = INVALID_HANDLE_VALUE;
    BOOL      ok   = FALSE;

    /* Working copies for redirect loop */
    wchar_t cur_server[512], cur_path[1024];
    wcsncpy_s(cur_server, 512,  server,   _TRUNCATE);
    wcsncpy_s(cur_path,   1024, url_path, _TRUNCATE);

    sess = WinHttpOpen(L"SproutLauncher/2.0",
                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) goto done;

    /* Redirect loop — GitHub redirects to githubusercontent CDN on a different host */
    for (int redirect = 0; redirect < 10; redirect++) {
        if (conn) { WinHttpCloseHandle(conn); conn = NULL; }
        if (req)  { WinHttpCloseHandle(req);  req  = NULL; }

        conn = WinHttpConnect(sess, cur_server, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn) goto done;

        /* Disable auto-redirect so we can handle cross-host redirects ourselves */
        DWORD no_redir = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        req = WinHttpOpenRequest(conn, L"GET", cur_path, NULL,
                                 WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
        if (!req) goto done;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &no_redir, sizeof(no_redir));

        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
        if (!WinHttpReceiveResponse(req, NULL)) goto done;

        /* Check HTTP status code */
        DWORD status = 0, sz_status = sizeof(DWORD);
        WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status, &sz_status, WINHTTP_NO_HEADER_INDEX);

        /* Follow 301/302/307/308 redirects manually */
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            wchar_t location[2048] = {0};
            DWORD   loc_sz = sizeof(location) - sizeof(wchar_t);
            if (!WinHttpQueryHeaders(req,
                    WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    location, &loc_sz, WINHTTP_NO_HEADER_INDEX)) {
                goto done; /* no Location header — can't follow */
            }
            /* Parse the Location URL: https://host/path or /path */
            if (wcsncmp(location, L"https://", 8) == 0) {
                const wchar_t *after  = location + 8;
                const wchar_t *slash  = wcschr(after, L'/');
                if (!slash) goto done;
                size_t host_len = slash - after;
                if (host_len >= 512) goto done;
                wcsncpy_s(cur_server, 512,  after, host_len);
                cur_server[host_len] = L'\0';
                wcsncpy_s(cur_path,   1024, slash, _TRUNCATE);
            } else if (wcsncmp(location, L"http://", 7) == 0) {
                /* Upgrade http → https anyway */
                const wchar_t *after = location + 7;
                const wchar_t *slash = wcschr(after, L'/');
                if (!slash) goto done;
                size_t host_len = slash - after;
                if (host_len >= 512) goto done;
                wcsncpy_s(cur_server, 512,  after, host_len);
                cur_server[host_len] = L'\0';
                wcsncpy_s(cur_path,   1024, slash, _TRUNCATE);
            } else {
                /* Relative redirect */
                wcsncpy_s(cur_path, 1024, location, _TRUNCATE);
            }
            continue; /* next iteration with new host/path */
        }

        /* Not a redirect — proceed to download */
        DWORD content_len = 0, sz = sizeof(DWORD);
        WinHttpQueryHeaders(req,
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &content_len, &sz, WINHTTP_NO_HEADER_INDEX);

        fh = CreateFileW(out_file, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) goto done;

        DWORD total = 0, bytes_read = 0;
        int   last_pct = -1;
        char  buf[65536];
        while (WinHttpReadData(req, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
            DWORD written;
            WriteFile(fh, buf, bytes_read, &written, NULL);
            total += bytes_read;
            int pct = content_len > 0 ? (int)((ULONGLONG)total * 100 / content_len) : 50;
            if (pct != last_pct) {
                last_pct = pct;
                EnterCriticalSection(&g_cs);
                g_results[scan_id].download_pct = pct;
                LeaveCriticalSection(&g_cs);
                PostMessage(hwnd, WM_DL_PROGRESS, (WPARAM)scan_id, (LPARAM)pct);
            }
        }
        ok = (total > 0);
        break; /* download finished */
    }

done:
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (req)  WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (sess) WinHttpCloseHandle(sess);
    return ok;
}


/* ---------- Universal Extraction Helper (7zr / tar / PowerShell) - */

static BOOL extract_archive(const char *archive_file, const char *dest_dir) {
    char cmd[1400];
    char tool_7zr[512];
    _snprintf_s(tool_7zr, sizeof(tool_7zr), _TRUNCATE, "%s\\7zr.exe", dest_dir);

    BOOL ran = FALSE;

    /* Try 1: 7zr.exe if present */
    if (file_exists_a(tool_7zr)) {
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
            "\"%s\" e \"%s\" -o\"%s\" *.dll -aoa",
            tool_7zr, archive_file, dest_dir);
    }
    /* Try 2: Windows 10/11 native tar.exe (supports .7z, .zip, .tar out of the box) */
    else if (file_exists_a("C:\\Windows\\System32\\tar.exe")) {
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
            "C:\\Windows\\System32\\tar.exe -xf \"%s\" -C \"%s\"",
            archive_file, dest_dir);
    }
    /* Try 3: PowerShell Expand-Archive (for .zip archives) */
    else {
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
            "powershell -Command \"Expand-Archive -Path '%s' -DestinationPath '%s' -Force\"",
            archive_file, dest_dir);
    }

    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 120000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ran = TRUE;
    }

    /* Move any DLLs extracted in subfolders (e.g. x64\ or mesa-...\) up to dest_dir */
    char sub_dll_search[1024];
    _snprintf_s(sub_dll_search, sizeof(sub_dll_search), _TRUNCATE, "%s\\*\\*.dll", dest_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(sub_dll_search, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char sub_folder_pattern[1024];
            _snprintf_s(sub_folder_pattern, sizeof(sub_folder_pattern), _TRUNCATE, "%s\\*", dest_dir);
            WIN32_FIND_DATAA fd2;
            HANDLE hf2 = FindFirstFileA(sub_folder_pattern, &fd2);
            if (hf2 != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        strcmp(fd2.cFileName, ".") != 0 && strcmp(fd2.cFileName, "..") != 0) {
                        char sub_dlls[1024];
                        _snprintf_s(sub_dlls, sizeof(sub_dlls), _TRUNCATE, "%s\\%s\\*.dll", dest_dir, fd2.cFileName);
                        WIN32_FIND_DATAA fd3;
                        HANDLE hf3 = FindFirstFileA(sub_dlls, &fd3);
                        if (hf3 != INVALID_HANDLE_VALUE) {
                            do {
                                char src_path[1024], dst_path[1024];
                                _snprintf_s(src_path, sizeof(src_path), _TRUNCATE, "%s\\%s\\%s", dest_dir, fd2.cFileName, fd3.cFileName);
                                _snprintf_s(dst_path, sizeof(dst_path), _TRUNCATE, "%s\\%s", dest_dir, fd3.cFileName);
                                MoveFileExA(src_path, dst_path, MOVEFILE_REPLACE_EXISTING);
                            } while (FindNextFileA(hf3, &fd3));
                            FindClose(hf3);
                        }
                    }
                } while (FindNextFileA(hf2, &fd2));
                FindClose(hf2);
            }
        } while (0);
        FindClose(hFind);
    }

    return ran;
}

/* ---------- Mesa3D download thread -------------------------------- */

static DWORD WINAPI dl_mesa_thread(LPVOID arg) {
    DlCtx *ctx = (DlCtx *)arg;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_cs);
    g_results[SCAN_MESA3D].status = SCAN_DOWNLOAD;
    g_results[SCAN_MESA3D].download_pct = 0;
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_MESA3D, 0);

    /* Ensure mesa folder exists */
    char mesa_dir[512];
    _snprintf_s(mesa_dir, sizeof(mesa_dir), _TRUNCATE, "%s\\mesa", ctx->base_dir);
    CreateDirectoryA(mesa_dir, NULL);

    /* Step 1: download 7zr.exe extractor helper if needed */
    wchar_t path_7zr[512];
    swprintf_s(path_7zr, 512, L"%hs\\mesa\\7zr.exe", ctx->base_dir);

    char path_7zr_a[512];
    _snprintf_s(path_7zr_a, sizeof(path_7zr_a), _TRUNCATE, "%s\\mesa\\7zr.exe", ctx->base_dir);
    if (!file_exists_a(path_7zr_a)) {
        EnterCriticalSection(&g_cs);
        wcsncpy_s(g_results[SCAN_MESA3D].detail, 256, L"Downloading extractor (7zr.exe)...", _TRUNCATE);
        LeaveCriticalSection(&g_cs);
        PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_MESA3D, 0);

        winhttp_download(L"www.7-zip.org", L"/a/7zr.exe", path_7zr, SCAN_MESA3D, ctx->hwnd);
    }

    /* Step 2: download Mesa3D archive (~70 MB) */
    wchar_t path_7z[512];
    swprintf_s(path_7z, 512, L"%hs\\mesa\\mesa3d.7z", ctx->base_dir);

    char path_7z_a[512];
    _snprintf_s(path_7z_a, sizeof(path_7z_a), _TRUNCATE, "%s\\mesa\\mesa3d.7z", ctx->base_dir);

    EnterCriticalSection(&g_cs);
    wcsncpy_s(g_results[SCAN_MESA3D].detail, 256, L"Downloading Mesa3D package (~70 MB)...", _TRUNCATE);
    g_results[SCAN_MESA3D].download_pct = 0;
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_MESA3D, 0);

    ok = winhttp_download(
        L"github.com",
        L"/pal1000/mesa-dist-win/releases/download/26.1.3/mesa3d-26.1.3-release-msvc.7z",
        path_7z, SCAN_MESA3D, ctx->hwnd);
    if (!ok) goto fail;

    /* Step 3: Extract archive */
    EnterCriticalSection(&g_cs);
    wcsncpy_s(g_results[SCAN_MESA3D].detail, 256, L"Extracting Mesa3D DLLs...", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_MESA3D, 0);

    extract_archive(path_7z_a, mesa_dir);

    /* Step 4: Verify DLL presence */
    {
        char dll[512];
        _snprintf_s(dll, sizeof(dll), _TRUNCATE, "%s\\mesa\\opengl32.dll", ctx->base_dir);
        ok = file_exists_a(dll);
    }
    if (!ok) goto fail;

    /* Cleanup downloaded archive */
    DeleteFileA(path_7z_a);

    EnterCriticalSection(&g_cs);
    g_results[SCAN_MESA3D].status      = SCAN_DONE_OK;
    g_results[SCAN_MESA3D].can_download = 0;
    wcsncpy_s(g_results[SCAN_MESA3D].detail, 256,
              L"Mesa3D installed successfully - Software renderer ready.", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_DL_DONE, SCAN_MESA3D, 1);
    goto cleanup;

fail:
    EnterCriticalSection(&g_cs);
    g_results[SCAN_MESA3D].status = SCAN_DONE_FAIL;
    wcsncpy_s(g_results[SCAN_MESA3D].detail, 256,
              L"Download failed. Check your connection and try again.", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_DL_DONE, SCAN_MESA3D, 0);

cleanup:
    InterlockedExchange(&g_dl_mesa_busy, 0);
    HeapFree(GetProcessHeap(), 0, ctx);
    return 0;
}

/* ---------- VC++ Runtime download thread -------------------------- */

static DWORD WINAPI dl_vcrt_thread(LPVOID arg) {
    DlCtx *ctx = (DlCtx *)arg;
    BOOL ok = FALSE;

    EnterCriticalSection(&g_cs);
    g_results[SCAN_VCRT].status = SCAN_DOWNLOAD;
    g_results[SCAN_VCRT].download_pct = 0;
    wcsncpy_s(g_results[SCAN_VCRT].detail, 256, L"Downloading VC++ Redistributable...", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_VCRT, 0);

    wchar_t out_file[512];
    swprintf_s(out_file, 512, L"%hs\\vc_redist.x64.exe", ctx->base_dir);

    ok = winhttp_download(L"aka.ms",
                          L"/vs/17/release/vc_redist.x64.exe",
                          out_file, SCAN_VCRT, ctx->hwnd);
    if (!ok) goto fail;

    /* Run installer silently */
    {
        char cmd[600];
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                    "\"%s\\vc_redist.x64.exe\" /install /quiet /norestart",
                    ctx->base_dir);

        EnterCriticalSection(&g_cs);
        wcsncpy_s(g_results[SCAN_VCRT].detail, 256, L"Installing VC++ Runtime...", _TRUNCATE);
        LeaveCriticalSection(&g_cs);
        PostMessage(ctx->hwnd, WM_SCAN_UPDATE, SCAN_VCRT, 0);

        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {0};
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    /* Verify */
    {
        HMODULE h = LoadLibraryA("VCRUNTIME140.dll");
        ok = (h != NULL);
        if (h) FreeLibrary(h);
    }
    if (!ok) goto fail;

    /* Cleanup installer */
    {
        char tmp[512];
        _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s\\vc_redist.x64.exe", ctx->base_dir);
        DeleteFileA(tmp);
    }

    EnterCriticalSection(&g_cs);
    g_results[SCAN_VCRT].status = SCAN_DONE_OK;
    g_results[SCAN_VCRT].can_download = 0;
    wcsncpy_s(g_results[SCAN_VCRT].detail, 256,
              L"VC++ Runtime installed successfully.", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_DL_DONE, SCAN_VCRT, 1);
    goto cleanup;

fail:
    EnterCriticalSection(&g_cs);
    g_results[SCAN_VCRT].status = SCAN_DONE_FAIL;
    wcsncpy_s(g_results[SCAN_VCRT].detail, 256,
              L"Download failed. Check your connection and try again.", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    PostMessage(ctx->hwnd, WM_DL_DONE, SCAN_VCRT, 0);

cleanup:
    InterlockedExchange(&g_dl_vcrt_busy, 0);
    HeapFree(GetProcessHeap(), 0, ctx);
    return 0;
}

/* ---------- Public API ------------------------------------------- */

void scan_start(HWND hwnd, const char *base_dir) {
    if (InterlockedCompareExchange(&g_scan_started, 1, 0) != 0) return;
    InitializeCriticalSection(&g_cs);
    g_notify = hwnd;
    strncpy_s(g_base, sizeof(g_base), base_dir, _TRUNCATE);
    result_init();
    HANDLE t = CreateThread(NULL, 0, scanner_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

void scan_get(ScanResult out[SCAN_COUNT]) {
    if (!g_scan_started) {
        result_init();
        memcpy(out, g_results, sizeof(g_results));
        return;
    }
    EnterCriticalSection(&g_cs);
    memcpy(out, g_results, sizeof(g_results));
    LeaveCriticalSection(&g_cs);
}

int scan_is_done(void) { return (int)g_scan_done; }

const char *scan_recommended_renderer(void) {
    if (!g_scan_started) return "hardware";
    EnterCriticalSection(&g_cs);
    BOOL sw = g_results[SCAN_OPENGL].recommends_software;
    LeaveCriticalSection(&g_cs);
    return sw ? "software" : "hardware";
}

void scan_download_mesa(HWND hwnd, const char *base_dir) {
    if (InterlockedCompareExchange(&g_dl_mesa_busy, 1, 0) != 0) return;
    DlCtx *ctx = (DlCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DlCtx));
    if (!ctx) { InterlockedExchange(&g_dl_mesa_busy, 0); return; }
    ctx->hwnd    = hwnd;
    ctx->scan_id = SCAN_MESA3D;
    strncpy_s(ctx->base_dir, sizeof(ctx->base_dir), base_dir, _TRUNCATE);
    HANDLE t = CreateThread(NULL, 0, dl_mesa_thread, ctx, 0, NULL);
    if (!t) { HeapFree(GetProcessHeap(), 0, ctx); InterlockedExchange(&g_dl_mesa_busy, 0); }
    else CloseHandle(t);
}

void scan_download_vcrt(HWND hwnd, const char *base_dir) {
    if (InterlockedCompareExchange(&g_dl_vcrt_busy, 1, 0) != 0) return;
    DlCtx *ctx = (DlCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DlCtx));
    if (!ctx) { InterlockedExchange(&g_dl_vcrt_busy, 0); return; }
    ctx->hwnd    = hwnd;
    ctx->scan_id = SCAN_VCRT;
    strncpy_s(ctx->base_dir, sizeof(ctx->base_dir), base_dir, _TRUNCATE);
    HANDLE t = CreateThread(NULL, 0, dl_vcrt_thread, ctx, 0, NULL);
    if (!t) { HeapFree(GetProcessHeap(), 0, ctx); InterlockedExchange(&g_dl_vcrt_busy, 0); }
    else CloseHandle(t);
}

static void copy_dlls_from_dir(const wchar_t *src_dir, const char *dest_mesa_dir) {
    wchar_t search_pattern[1024];
    swprintf_s(search_pattern, 1024, L"%s\\*.dll", src_dir);
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            wchar_t s_file[1024], d_file[1024];
            swprintf_s(s_file, 1024, L"%s\\%s", src_dir, fd.cFileName);
            swprintf_s(d_file, 1024, L"%hs\\%s", dest_mesa_dir, fd.cFileName);
            CopyFileW(s_file, d_file, FALSE);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

BOOL scan_import_mesa(HWND hwnd, const char *base_dir, const wchar_t *src_path) {
    if (!src_path || !src_path[0]) return FALSE;
    char mesa_dir[512];
    _snprintf_s(mesa_dir, sizeof(mesa_dir), _TRUNCATE, "%s\\mesa", base_dir);
    CreateDirectoryA(mesa_dir, NULL);

    DWORD attr = GetFileAttributesW(src_path);
    if (attr == INVALID_FILE_ATTRIBUTES) return FALSE;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        copy_dlls_from_dir(src_path, mesa_dir);
        wchar_t sub_pattern[1024];
        swprintf_s(sub_pattern, 1024, L"%s\\*", src_path);
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(sub_pattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                    wchar_t sub_dir[1024];
                    swprintf_s(sub_dir, 1024, L"%s\\%s", src_path, fd.cFileName);
                    copy_dlls_from_dir(sub_dir, mesa_dir);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    } else {
        const wchar_t *ext = wcsrchr(src_path, L'.');
        if (ext && (_wcsicmp(ext, L".7z") == 0 || _wcsicmp(ext, L".zip") == 0)) {
            char archive_a[512];
            WideCharToMultiByte(CP_ACP, 0, src_path, -1, archive_a, sizeof(archive_a), NULL, NULL);
            extract_archive(archive_a, mesa_dir);
        } else {
            wchar_t dst_file[1024];
            const wchar_t *fname = wcsrchr(src_path, L'\\');
            if (!fname) fname = wcsrchr(src_path, L'/');
            fname = fname ? fname + 1 : src_path;
            swprintf_s(dst_file, 1024, L"%hs\\%s", mesa_dir, fname);
            CopyFileW(src_path, dst_file, FALSE);

            wchar_t src_dir[1024];
            wcscpy_s(src_dir, 1024, src_path);
            wchar_t *p = wcsrchr(src_dir, L'\\');
            if (!p) p = wcsrchr(src_dir, L'/');
            if (p) {
                *p = L'\0';
                copy_dlls_from_dir(src_dir, mesa_dir);
            }
        }
    }

    char dll_check[512];
    _snprintf_s(dll_check, sizeof(dll_check), _TRUNCATE, "%s\\mesa\\opengl32.dll", base_dir);
    BOOL ok = file_exists_a(dll_check);

    InterlockedExchange(&g_scan_started, 0);
    scan_start(hwnd, base_dir);
    return ok;
}

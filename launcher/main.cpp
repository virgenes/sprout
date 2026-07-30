/* main.cpp — Sprout Launcher V2 (Flicker-free Double-Buffered GDI Win32 Launcher)
 * Fixes: Double-buffering, fixed child window positioning, legibility of controls,
 * combobox dropdown expansion, robust dark-mode theming, and renderer state saving.
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "config.h"
#include "scanner.h"
#include "controls_dialog.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* ================================================================
 *  LAYOUT CONSTANTS
 * ================================================================ */
#define WW2      660        /* window client width  */
#define WH2      710        /* window client height */
#define HDR_H    72         /* header height        */
#define NAV_H    40         /* nav-tab bar height   */
#define STA_H    30         /* status bar height    */
#define CON_Y    (HDR_H + NAV_H)   /* content top  */
#define CON_H    (WH2 - CON_Y - STA_H)
#define PAD      20         /* inner content padding */

/* ================================================================
 *  COLOUR PALETTE
 * ================================================================ */
#define C_BG       RGB(0x0E,0x0E,0x1A)
#define C_HDR      RGB(0x12,0x12,0x22)
#define C_CARD     RGB(0x16,0x16,0x2A)
#define C_CARD2    RGB(0x1C,0x1C,0x34)
#define C_BORDER   RGB(0x26,0x26,0x44)
#define C_ACCENT   RGB(0x4A,0xDE,0x80)
#define C_ACCH     RGB(0x3B,0xC7,0x6C)
#define C_ACCP     RGB(0x2D,0xA0,0x55)
#define C_WARN     RGB(0xF5,0x9E,0x0B)
#define C_ERR      RGB(0xEF,0x44,0x44)
#define C_TEXT     RGB(0xF0,0xF0,0xF5)
#define C_DIM      RGB(0x90,0x90,0xA8)
#define C_MUTE     RGB(0x52,0x52,0x7A)
#define C_BTNTXT   RGB(0x08,0x08,0x10)
#define C_NAV_ACT  RGB(0x1E,0x1E,0x38)
#define C_NAV_HVR  RGB(0x18,0x18,0x2C)

/* ================================================================
 *  PAGES & CONTROL IDs
 * ================================================================ */
typedef enum { PAGE_HOME=0, PAGE_SYSTEM, PAGE_OPTIONS, PAGE_ENGINE, PAGE_COUNT } Page;

/* Nav buttons */
#define ID_NAV_BASE  3000
#define ID_BTN_PLAY  3010
#define ID_BTN_CTRL  3011
/* System page */
#define ID_BTN_RESCAN    3100
#define ID_BTN_DL_MESA   3101
#define ID_BTN_DL_VCRT   3102
#define ID_BTN_LOCATE_MESA 3103
/* Options page combos/checks */
#define ID_CMB_VMODE     3200
#define ID_CMB_FPS       3201
#define ID_CMB_QUAL      3202
#define ID_CMB_SHADOW    3203
#define ID_CMB_SCALE     3204
#define ID_CMB_LANG      3205
#define ID_CHK_VSYNC     3206
#define ID_CHK_IAP       3207
#define ID_CHK_SAVES     3208
#define ID_CHK_CON       3209
/* Engine page */
#define ID_CMB_REND      3300
#define ID_CMB_MSAA      3301
#define ID_BTN_GETMESA   3302

#define TMR_ANIM   1

/* ================================================================
 *  GLOBAL STATE
 * ================================================================ */
static HWND   g_hwnd        = NULL;
static Page   g_page        = PAGE_HOME;
static int    g_anim_tick   = 0;
static BOOL   g_scan_done   = FALSE;

/* GDI Resources */
static HFONT  g_fTitle, g_fHeader, g_fBody, g_fSmall, g_fBtn, g_fNav, g_fEmoji;
static HBRUSH g_brBg, g_brCard, g_brCard2, g_brAccent, g_brWarn, g_brErr, g_brHdr;

/* Control Handles */
static HWND g_hNav[PAGE_COUNT];
static HWND g_hPlay, g_hCtrl;
static HWND g_hRescan, g_hDlMesa, g_hDlVcrt, g_hLocateMesa;
static HWND g_hVMode, g_hFps, g_hQual, g_hShadow, g_hScale, g_hLang;
static HWND g_hVsync, g_hIap, g_hSaves, g_hCon;
static HWND g_hRend, g_hMsaa, g_hGetMesa, g_hLocateMesaEng;

/* ---------- Forward declarations ---------- */
static void backup_saves(HWND hwnd);

/* ================================================================
 *  HELPERS — FONTS & BRUSHES
 * ================================================================ */
static HFONT make_font(int pt, int weight, const wchar_t *face) {
    HDC dc = GetDC(NULL);
    int height = -MulDiv(pt, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ReleaseDC(NULL, dc);
    return CreateFontW(height, 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, face);
}

static void create_fonts(void) {
    g_fTitle  = make_font(22, FW_BOLD,     L"Segoe UI");
    g_fHeader = make_font(11, FW_SEMIBOLD, L"Segoe UI");
    g_fBody   = make_font(10, FW_NORMAL,   L"Segoe UI");
    g_fSmall  = make_font( 9, FW_NORMAL,   L"Segoe UI");
    g_fBtn    = make_font(11, FW_SEMIBOLD, L"Segoe UI");
    g_fNav    = make_font(10, FW_SEMIBOLD, L"Segoe UI");
    g_fEmoji  = make_font(14, FW_NORMAL,   L"Segoe UI Emoji");
}

static void delete_fonts(void) {
    DeleteObject(g_fTitle);  DeleteObject(g_fHeader);
    DeleteObject(g_fBody);   DeleteObject(g_fSmall);
    DeleteObject(g_fBtn);    DeleteObject(g_fNav);
    DeleteObject(g_fEmoji);
}

static void create_brushes(void) {
    g_brBg     = CreateSolidBrush(C_BG);
    g_brCard   = CreateSolidBrush(C_CARD);
    g_brCard2  = CreateSolidBrush(C_CARD2);
    g_brAccent = CreateSolidBrush(C_ACCENT);
    g_brWarn   = CreateSolidBrush(C_WARN);
    g_brErr    = CreateSolidBrush(C_ERR);
    g_brHdr    = CreateSolidBrush(C_HDR);
}

static void delete_brushes(void) {
    DeleteObject(g_brBg);    DeleteObject(g_brCard);  DeleteObject(g_brCard2);
    DeleteObject(g_brAccent);DeleteObject(g_brWarn);
    DeleteObject(g_brErr);   DeleteObject(g_brHdr);
}

/* ================================================================
 *  HELPERS — DRAWING PRIMITIVES
 * ================================================================ */

static void draw_card(HDC dc, int x, int y, int w, int h, COLORREF bg) {
    HBRUSH br  = CreateSolidBrush(bg);
    HPEN   pen = CreatePen(PS_SOLID, 1, C_BORDER);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, x, y, x+w, y+h, 12, 12);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(br); DeleteObject(pen);
}

static void draw_circle(HDC dc, int cx, int cy, int r, COLORREF col) {
    HBRUSH br  = CreateSolidBrush(col);
    HPEN   pen = CreatePen(PS_NULL, 0, 0);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, br);
    Ellipse(dc, cx-r, cy-r, cx+r+1, cy+r+1);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(br); DeleteObject(pen);
}

static void draw_progress(HDC dc, int x, int y, int w, int h, int pct, COLORREF track, COLORREF fill) {
    RECT rtrack = {x, y, x+w, y+h};
    HBRUSH bt = CreateSolidBrush(track);
    FillRect(dc, &rtrack, bt);
    DeleteObject(bt);
    if (pct > 0) {
        int fw = w * pct / 100;
        if (fw > 0) {
            RECT rfill = {x, y, x+fw, y+h};
            HBRUSH bf = CreateSolidBrush(fill);
            FillRect(dc, &rfill, bf);
            DeleteObject(bf);
        }
    }
}

static void draw_text(HDC dc, const wchar_t *s, HFONT f, COLORREF col,
                      int x, int y, int w, int h, UINT fmt) {
    HFONT of = f ? (HFONT)SelectObject(dc, f) : NULL;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    RECT r = {x, y, x+w, y+h};
    DrawTextW(dc, s, -1, &r, fmt);
    if (of) SelectObject(dc, of);
}

static const wchar_t *dot_anim(int tick) {
    switch (tick % 4) {
        case 0: return L"";
        case 1: return L".";
        case 2: return L"..";
        default: return L"...";
    }
}

static COLORREF status_color(ScanStatus st) {
    switch (st) {
        case SCAN_OK:       return C_ACCENT;
        case SCAN_WARN:     return C_WARN;
        case SCAN_MISSING:  return C_ERR;
        case SCAN_DONE_OK:  return C_ACCENT;
        case SCAN_DONE_FAIL:return C_ERR;
        default:            return C_MUTE;
    }
}

static void paint_ow_button(DRAWITEMSTRUCT *di, COLORREF bg_normal, COLORREF bg_press,
                             COLORREF text_col, BOOL is_accent) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    BOOL pressed = (di->itemState & ODS_SELECTED) != 0;
    COLORREF bg = pressed ? bg_press : bg_normal;

    HBRUSH br  = CreateSolidBrush(bg);
    HPEN   pen = CreatePen(PS_SOLID, 1, is_accent ? C_ACCP : C_BORDER);
    HPEN   op  = (HPEN)SelectObject(dc, pen);
    HBRUSH ob  = (HBRUSH)SelectObject(dc, br);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 8, 8);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(br); DeleteObject(pen);

    wchar_t t[64]; GetWindowTextW(di->hwndItem, t, 64);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text_col);
    SelectObject(dc, g_fBtn);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* ================================================================
 *  PAGE VISIBILITY & POSITIONING
 * ================================================================ */
static void set_page(Page p) {
    g_page = p;

    /* Home controls */
    int show_home = (p == PAGE_HOME);
    ShowWindow(g_hPlay, show_home ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hCtrl, show_home ? SW_SHOW : SW_HIDE);

    /* System controls */
    int show_sys = (p == PAGE_SYSTEM);
    ShowWindow(g_hRescan, show_sys ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLocateMesa, show_sys ? SW_SHOW : SW_HIDE);

    ScanResult res[SCAN_COUNT];
    scan_get(res);

    BOOL dl_mesa_vis = show_sys && res[SCAN_MESA3D].can_download &&
        (res[SCAN_MESA3D].status == SCAN_MISSING || res[SCAN_MESA3D].status == SCAN_DONE_FAIL);
    BOOL dl_vcrt_vis = show_sys && res[SCAN_VCRT].can_download &&
        (res[SCAN_VCRT].status == SCAN_MISSING || res[SCAN_VCRT].status == SCAN_DONE_FAIL);

    ShowWindow(g_hDlMesa, dl_mesa_vis ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hDlVcrt, dl_vcrt_vis ? SW_SHOW : SW_HIDE);

    /* Options controls */
    int show_opt = (p == PAGE_OPTIONS);
    ShowWindow(g_hVMode,  show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hFps,    show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hQual,   show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hShadow, show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hScale,  show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLang,   show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hVsync,  show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hIap,    show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSaves,  show_opt ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hCon,    show_opt ? SW_SHOW : SW_HIDE);

    /* Engine controls */
    int show_eng = (p == PAGE_ENGINE);
    ShowWindow(g_hRend,          show_eng ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hMsaa,          show_eng ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hGetMesa,       show_eng ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLocateMesaEng, show_eng ? SW_SHOW : SW_HIDE);

    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ================================================================
 *  CONTROL CREATION HELPERS
 * ================================================================ */
static HWND make_combo(HWND p, int x, int y, int w, int h_dropdown, int id, HFONT f) {
    HWND hc = CreateWindowW(L"COMBOBOX", NULL,
        WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL|CBS_HASSTRINGS|WS_TABSTOP,
        x, y, w, h_dropdown, p, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f) SendMessage(hc, WM_SETFONT, (WPARAM)f, FALSE);
    return hc;
}

static HWND make_check(HWND p, const wchar_t *t, int x, int y, int w, int h, int id, HFONT f, int chk) {
    HWND hc = CreateWindowW(L"BUTTON", t, WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|WS_TABSTOP,
                            x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f) SendMessage(hc, WM_SETFONT, (WPARAM)f, FALSE);
    SendMessage(hc, BM_SETCHECK, chk ? BST_CHECKED : BST_UNCHECKED, 0);
    return hc;
}

static HWND make_ow_btn(HWND p, const wchar_t *t, int x, int y, int w, int h, int id, HFONT f) {
    HWND hb = CreateWindowW(L"BUTTON", t,
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_OWNERDRAW|WS_TABSTOP,
        x, y, w, h, p, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f) SendMessage(hb, WM_SETFONT, (WPARAM)f, FALSE);
    return hb;
}

/* ================================================================
 *  DRAW PAGES (PAINTED ONTO OFFSCREEN MEMORY DC)
 * ================================================================ */

static void paint_home(HDC dc) {
    ScanResult res[SCAN_COUNT];
    scan_get(res);

    int issues = 0, crit = 0;
    for (int i = 0; i < SCAN_COUNT && g_scan_done; i++) {
        if (res[i].status == SCAN_WARN || res[i].status == SCAN_MISSING) issues++;
        if (res[i].status == SCAN_MISSING) crit++;
    }

    int crd_y = CON_Y + 20;
    COLORREF crd_col = (crit > 0) ? RGB(0x25,0x10,0x12) :
                       (issues > 0) ? RGB(0x24,0x1C,0x08) : RGB(0x10,0x22,0x14);
    draw_card(dc, PAD, crd_y, WW2 - PAD*2, 80, crd_col);

    if (!g_scan_done) {
        wchar_t scanning[64];
        swprintf_s(scanning, 64, L"Scanning system%s", dot_anim(g_anim_tick));
        draw_text(dc, scanning, g_fHeader, C_DIM, PAD+18, crd_y+12, WW2-PAD*2-20, 24,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_text(dc, L"Please wait while we check your system...",
                  g_fBody, C_MUTE, PAD+18, crd_y+38, WW2-PAD*2-20, 24, DT_LEFT);
    } else if (crit > 0) {
        wchar_t s[80];
        swprintf_s(s, 80, L"%d critical issue%s found", crit, crit>1?L"s":L"");
        draw_circle(dc, PAD+26, crd_y+40, 8, C_ERR);
        draw_text(dc, s, g_fHeader, C_ERR, PAD+42, crd_y+12, WW2-PAD*2-50, 24,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_text(dc, L"Go to the System tab to fix issues before launching.",
                  g_fBody, C_DIM, PAD+42, crd_y+38, WW2-PAD*2-50, 24, DT_LEFT);
    } else if (issues > 0) {
        wchar_t s[80];
        swprintf_s(s, 80, L"%d warning%s - game should still work", issues, issues>1?L"s":L"");
        draw_circle(dc, PAD+26, crd_y+40, 8, C_WARN);
        draw_text(dc, s, g_fHeader, C_WARN, PAD+42, crd_y+12, WW2-PAD*2-50, 24,
                  DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_text(dc, L"Check the System tab for details.",
                  g_fBody, C_DIM, PAD+42, crd_y+38, WW2-PAD*2-50, 24, DT_LEFT);
    } else if (g_scan_done) {
        draw_circle(dc, PAD+26, crd_y+40, 8, C_ACCENT);
        draw_text(dc, L"All systems ready - good to go!", g_fHeader, C_ACCENT,
                  PAD+42, crd_y+12, WW2-PAD*2-50, 24, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        draw_text(dc, L"Your PC meets all requirements. Press Play to launch.",
                  g_fBody, C_DIM, PAD+42, crd_y+38, WW2-PAD*2-50, 24, DT_LEFT);
    }

    if (g_scan_done) {
        const char *rec = scan_recommended_renderer();
        BOOL is_sw = (strcmp(rec, "software") == 0);
        int inf_y = crd_y + 100;
        draw_card(dc, PAD, inf_y, WW2-PAD*2, 50, C_CARD);
        wchar_t rtext[128];
        if (is_sw) {
            swprintf_s(rtext, 128, L"Renderer: Software (Mesa3D)  -  engine will use CPU rendering");
        } else {
            swprintf_s(rtext, 128, L"Renderer: Hardware (OpenGL)  -  engine will use GPU acceleration");
        }
        draw_text(dc, L"Detected Configuration", g_fSmall, C_MUTE, PAD+14, inf_y+8, WW2-PAD*3, 16, DT_LEFT);
        draw_text(dc, rtext, g_fBody, C_TEXT, PAD+14, inf_y+26, WW2-PAD*3, 18, DT_LEFT);
    }

    draw_text(dc, L"Plants vs. Zombies 2 - Native Windows Port", g_fSmall, C_MUTE,
              PAD, WH2 - STA_H - 50, WW2-PAD*2, 20, DT_CENTER);
}

static void paint_system(HDC dc) {
    ScanResult res[SCAN_COUNT];
    scan_get(res);

    draw_text(dc, L"SYSTEM DIAGNOSTICS", g_fHeader, C_DIM,
              PAD, CON_Y+10, WW2-PAD*2, 20, DT_LEFT);

    int row_h = 56;
    int row_y = CON_Y + 34;

    for (int i = 0; i < SCAN_COUNT; i++) {
        COLORREF row_bg = (i % 2 == 0) ? C_CARD : C_CARD2;
        HBRUSH rb  = CreateSolidBrush(row_bg);
        HPEN   rp  = CreatePen(PS_SOLID, 1, C_BORDER);
        HPEN   rop = (HPEN)SelectObject(dc, rp);
        HBRUSH rob = (HBRUSH)SelectObject(dc, rb);
        RoundRect(dc, PAD, row_y, WW2-PAD, row_y+row_h-2, 8, 8);
        SelectObject(dc, rop); SelectObject(dc, rob);
        DeleteObject(rb); DeleteObject(rp);

        COLORREF dot_col;
        if (res[i].status == SCAN_CHECKING || res[i].status == SCAN_DOWNLOAD) {
            int pulse = (g_anim_tick + i) % 3;
            dot_col = pulse == 0 ? C_MUTE : (pulse == 1 ? C_DIM : C_MUTE);
        } else {
            dot_col = status_color(res[i].status);
        }
        draw_circle(dc, PAD+18, row_y + row_h/2 - 1, 6, dot_col);

        draw_text(dc, res[i].label, g_fBtn, C_TEXT,
                  PAD+34, row_y+7, 180, 18, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        const wchar_t *tag = NULL;
        COLORREF tag_col = C_MUTE;
        switch (res[i].status) {
            case SCAN_CHECKING:  tag = L"Checking..."; tag_col = C_MUTE;   break;
            case SCAN_OK:        tag = L"OK";          tag_col = C_ACCENT;  break;
            case SCAN_WARN:      tag = L"Warning";     tag_col = C_WARN;    break;
            case SCAN_MISSING:   tag = L"Missing";     tag_col = C_ERR;     break;
            case SCAN_DOWNLOAD:  {
                wchar_t pbar[32];
                swprintf_s(pbar, 32, L"Downloading %d%%", res[i].download_pct);
                draw_text(dc, pbar, g_fSmall, C_ACCENT, WW2-200, row_y+7, 120, 18,
                          DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
                draw_progress(dc, PAD+34, row_y+row_h-16, WW2-PAD-100,
                              6, res[i].download_pct, C_CARD2, C_ACCENT);
                tag = NULL;
                break;
            }
            case SCAN_DONE_OK:   tag = L"\u2713 Done";   tag_col = C_ACCENT;  break;
            case SCAN_DONE_FAIL: tag = L"Failed";     tag_col = C_ERR;     break;
        }
        if (tag)
            draw_text(dc, tag, g_fSmall, tag_col, WW2-200, row_y+7, 110, 18,
                      DT_RIGHT|DT_VCENTER|DT_SINGLELINE);

        draw_text(dc, res[i].detail, g_fSmall, C_DIM,
                  PAD+34, row_y+26, WW2-PAD-200, 18, DT_LEFT|DT_END_ELLIPSIS|DT_SINGLELINE);

        row_y += row_h;
    }
}

static void paint_options(HDC dc) {
    draw_text(dc, L"GAME OPTIONS", g_fHeader, C_DIM, PAD, CON_Y+10, WW2-PAD*2, 20, DT_LEFT);

    int lx = PAD+14, lw = 160;
    int row_h = 38;
    int y = CON_Y + 34;

    struct { const wchar_t *lbl; } rows[] = {
        {L"Window Mode"}, {L"FPS Limit"}, {L"Quality"},
        {L"Shadows"}, {L"Render Scale"}, {L"Language"},
    };

    draw_card(dc, PAD, y-8, WW2-PAD*2, 6 * row_h + 4, C_CARD);

    for (int i = 0; i < 6; i++) {
        draw_text(dc, rows[i].lbl, g_fBody, C_DIM,
                  lx, y + row_h*i + 8, lw, 22, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }

    int cy2 = y + 6*row_h + 18;
    draw_card(dc, PAD, cy2-8, WW2-PAD*2, 2*row_h+8, C_CARD);
    draw_text(dc, L"FEATURES", g_fHeader, C_MUTE, PAD+14, cy2-2, 200, 14, DT_LEFT);
}

static void paint_engine(HDC dc) {
    draw_text(dc, L"RENDERING ENGINE", g_fHeader, C_DIM, PAD, CON_Y+10, WW2-PAD*2, 20, DT_LEFT);

    int y = CON_Y + 38;

    /* Renderer card */
    draw_card(dc, PAD, y, WW2-PAD*2, 80, C_CARD);
    draw_text(dc, L"Rendering Backend", g_fBtn, C_TEXT, PAD+16, y+10, 260, 20, DT_LEFT);
    draw_text(dc,
        L"Hardware: uses your GPU (OpenGL).  Software: CPU-based Mesa3D LLVMpipe.",
        g_fSmall, C_DIM, PAD+16, y+32, WW2-PAD*2-220, 36, DT_LEFT|DT_WORDBREAK);

    /* MSAA card */
    y += 96;
    draw_card(dc, PAD, y, WW2-PAD*2, 70, C_CARD);
    draw_text(dc, L"Anti-Aliasing (MSAA)", g_fBtn, C_TEXT, PAD+16, y+10, 260, 20, DT_LEFT);
    draw_text(dc, L"Higher values reduce jagged edges but cost more CPU/GPU power.",
        g_fSmall, C_DIM, PAD+16, y+32, WW2-PAD*2-220, 32, DT_LEFT|DT_WORDBREAK);

    /* Mesa3D info card */
    y += 86;
    BOOL sw = (_stricmp(g_renderer, "software") == 0);
    draw_card(dc, PAD, y, WW2-PAD*2, 100, sw ? RGB(0x0F,0x22,0x18) : C_CARD);
    draw_text(dc, sw ? L"Mesa3D Software Active" : L"Mesa3D Software Available",
        g_fBtn, sw ? C_ACCENT : C_DIM, PAD+16, y+8, 280, 20, DT_LEFT);
    draw_text(dc,
        L"Mesa3D is a software renderer using your CPU. Ideal for PCs\nwithout a dedicated GPU or with outdated drivers.",
        g_fSmall, C_DIM, PAD+16, y+30, WW2-PAD*2-32, 36, DT_LEFT|DT_WORDBREAK);
}

/* ================================================================
 *  WINDOW PROC
 * ================================================================ */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    /* ---- WM_CREATE ------------------------------------------- */
    case WM_CREATE: {
        g_hwnd = hwnd;
        create_fonts();
        create_brushes();

        /* DWM dark title bar (Win10 & Win11 compatible) */
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));

        /* Nav buttons */
        const wchar_t *nav_labels[PAGE_COUNT] = { L"Home", L"System", L"Options", L"Engine" };
        int nw = WW2 / PAGE_COUNT;
        for (int i = 0; i < PAGE_COUNT; i++) {
            g_hNav[i] = make_ow_btn(hwnd, nav_labels[i], i*nw, HDR_H, nw, NAV_H,
                                    ID_NAV_BASE+i, g_fNav);
        }

        /* Home page controls */
        g_hPlay = make_ow_btn(hwnd, L"\u25B6  Launch Game",
                              (WW2-240)/2, CON_Y + 250, 240, 54, ID_BTN_PLAY, g_fTitle);
        g_hCtrl = make_ow_btn(hwnd, L"Edit Controls",
                              (WW2-140)/2, CON_Y + 326, 140, 32, ID_BTN_CTRL, g_fBody);

        /* System page controls */
        g_hRescan     = make_ow_btn(hwnd, L"Re-Scan", WW2-PAD-90, CON_Y+8, 90, 28,
                                    ID_BTN_RESCAN, g_fSmall);
        g_hLocateMesa = make_ow_btn(hwnd, L"\U0001F4C1", WW2-PAD-140, CON_Y+34+56+6, 34, 28,
                                    ID_BTN_LOCATE_MESA, g_fEmoji);
        g_hDlMesa     = make_ow_btn(hwnd, L"Download", WW2-PAD-100, CON_Y+34+56+6, 95, 28,
                                    ID_BTN_DL_MESA, g_fSmall);
        g_hDlVcrt     = make_ow_btn(hwnd, L"Download", WW2-PAD-100, CON_Y+34+56*2+6, 100, 28,
                                    ID_BTN_DL_VCRT, g_fSmall);

        /* Options page combos & checks */
        int y = CON_Y + 34;
        int row_h = 38;
        int cx = PAD + 180, cw = 230, combo_h = 180;

        g_hVMode  = make_combo(hwnd, cx, y+5,         cw, combo_h, ID_CMB_VMODE,  g_fBody);
        g_hFps    = make_combo(hwnd, cx, y+row_h+5,   cw, combo_h, ID_CMB_FPS,    g_fBody);
        g_hQual   = make_combo(hwnd, cx, y+row_h*2+5, cw, combo_h, ID_CMB_QUAL,   g_fBody);
        g_hShadow = make_combo(hwnd, cx, y+row_h*3+5, cw, combo_h, ID_CMB_SHADOW, g_fBody);
        g_hScale  = make_combo(hwnd, cx, y+row_h*4+5, cw, combo_h, ID_CMB_SCALE,  g_fBody);
        g_hLang   = make_combo(hwnd, cx, y+row_h*5+5, cw, combo_h, ID_CMB_LANG,   g_fBody);

        for (int i=0;i<kNumVideoMode;i++) SendMessageW(g_hVMode,  CB_ADDSTRING,0,(LPARAM)kVideoMode[i].label);
        for (int i=0;i<kNumFpsLimit; i++) SendMessageW(g_hFps,    CB_ADDSTRING,0,(LPARAM)kFpsLimit[i].label);
        for (int i=0;i<kNumQuality;  i++) SendMessageW(g_hQual,   CB_ADDSTRING,0,(LPARAM)kQuality[i].label);
        for (int i=0;i<kNumShadows;  i++) SendMessageW(g_hShadow, CB_ADDSTRING,0,(LPARAM)kShadows[i].label);
        for (int i=0;i<kNumRenderScale;i++) SendMessageW(g_hScale,CB_ADDSTRING,0,(LPARAM)kRenderScale[i].label);
        for (int i=0;i<kNumLangs;    i++) SendMessageW(g_hLang,   CB_ADDSTRING,0,(LPARAM)kLangs[i].label);

        for (int i=0;i<kNumVideoMode;i++) if(kVideoMode[i].val==(_stricmp(g_videoMode,"fullscreen")==0?1:0)){SendMessage(g_hVMode,CB_SETCURSEL,i,0);break;}
        for (int i=0;i<kNumFpsLimit;i++) if(kFpsLimit[i].val==g_fpsLimit){SendMessage(g_hFps,CB_SETCURSEL,i,0);break;}
        SendMessage(g_hQual,CB_SETCURSEL,0,0);
        SendMessage(g_hShadow,CB_SETCURSEL,0,0);
        SendMessage(g_hScale,CB_SETCURSEL,2,0); /* 100% */
        SendMessage(g_hLang,CB_SETCURSEL,lang_idx(g_locale),0);

        int cy2 = y + 6*row_h + 18;
        g_hVsync = make_check(hwnd, L"V-Sync",          PAD+14,     cy2+16, 160, 24, ID_CHK_VSYNC, g_fBody, g_vsync);
        g_hIap   = make_check(hwnd, L"Emulate IAP",     PAD+14+170, cy2+16, 160, 24, ID_CHK_IAP,   g_fBody, g_emulateIap);
        g_hSaves = make_check(hwnd, L"Persist Saves",   PAD+14,     cy2+46, 160, 24, ID_CHK_SAVES, g_fBody, g_persistSaves);
        g_hCon   = make_check(hwnd, L"Show Console",    PAD+14+170, cy2+46, 160, 24, ID_CHK_CON,   g_fBody, g_showConsole);

        /* Engine page controls */
        int ey = CON_Y + 38;
        g_hRend         = make_combo(hwnd, WW2-PAD-190, ey+26, 190, combo_h, ID_CMB_REND, g_fBody);
        g_hMsaa         = make_combo(hwnd, WW2-PAD-190, ey+96+22, 190, combo_h, ID_CMB_MSAA, g_fBody);
        g_hGetMesa      = make_ow_btn(hwnd, L"Get Mesa3D",      PAD+16,    ey+96+86+65, 130, 26, ID_BTN_GETMESA,    g_fSmall);
        g_hLocateMesaEng= make_ow_btn(hwnd, L"\U0001F4C1",      PAD+16+134, ey+96+86+65, 34, 26, ID_BTN_LOCATE_MESA, g_fEmoji);

        for (int i=0;i<kNumRenderer;i++) SendMessageW(g_hRend,CB_ADDSTRING,0,(LPARAM)kRenderer[i].label);
        SendMessage(g_hRend,CB_SETCURSEL,(_stricmp(g_renderer,"software")==0)?1:0,0);

        const wchar_t *msaa_items[] = {L"Off (x1)", L"x2", L"x4", L"x8"};
        for (int i=0;i<4;i++) SendMessageW(g_hMsaa,CB_ADDSTRING,0,(LPARAM)msaa_items[i]);
        int msaa_sel = (g_gl_msaa==2)?1 : (g_gl_msaa==4)?2 : (g_gl_msaa==8)?3 : 0;
        SendMessage(g_hMsaa,CB_SETCURSEL,msaa_sel,0);

        /* Start background scanner */
        scan_start(hwnd, g_exeDir);

        /* Initial Page */
        set_page(PAGE_HOME);

        /* Animation timer (200ms) */
        SetTimer(hwnd, TMR_ANIM, 200, NULL);
        return 0;
    }

    /* ---- WM_TIMER -------------------------------------------- */
    case WM_TIMER:
        if (wp == TMR_ANIM) {
            g_anim_tick++;
            BOOL was_done = g_scan_done;
            g_scan_done = scan_is_done();
            if (!g_scan_done) {
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (!was_done) {
                set_page(g_page);
            }
        }
        return 0;

    /* ---- Scan / download messages ---------------------------- */
    case WM_SCAN_UPDATE:
    case WM_DL_PROGRESS:
        g_scan_done = scan_is_done();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_DL_DONE:
        g_scan_done = scan_is_done();
        set_page(g_page);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    /* ---- WM_ERASEBKGND (Prevent background erase flickering) -- */
    case WM_ERASEBKGND:
        return 1;

    /* ---- WM_PAINT (Double-Buffered Flicker-Free Rendering) ---- */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdcWin = BeginPaint(hwnd, &ps);

        RECT clientRect;
        GetClientRect(hwnd, &clientRect);
        int w = clientRect.right - clientRect.left;
        int h = clientRect.bottom - clientRect.top;

        /* Create Memory Offscreen DC */
        HDC dc = CreateCompatibleDC(hdcWin);
        HBITMAP hbm = CreateCompatibleBitmap(hdcWin, w, h);
        HBITMAP hbmOld = (HBITMAP)SelectObject(dc, hbm);

        /* Paint background */
        FillRect(dc, &clientRect, g_brBg);

        /* ---- Header ---- */
        RECT hdr = {0, 0, w, HDR_H};
        FillRect(dc, &hdr, g_brHdr);

        RECT bar = {0, 0, w, 3};
        FillRect(dc, &bar, g_brAccent);

        draw_text(dc, L"Sprout", g_fTitle, C_ACCENT, PAD, 12, 200, 30, DT_LEFT);
        draw_text(dc, L"Plants vs. Zombies 2 - Native Windows Port",
                  g_fSmall, C_DIM, PAD, 44, w - PAD*2 - 120, 18, DT_LEFT);
        draw_text(dc, L"v2.0", g_fSmall, C_MUTE, w - PAD - 50, 28, 50, 18, DT_RIGHT);

        RECT hbdr = {0, HDR_H - 1, w, HDR_H};
        HBRUSH hbr = CreateSolidBrush(C_BORDER);
        FillRect(dc, &hbdr, hbr);
        DeleteObject(hbr);

        /* ---- Nav Bar ---- */
        RECT nav = {0, HDR_H, w, HDR_H + NAV_H};
        HBRUSH navbr = CreateSolidBrush(C_HDR);
        FillRect(dc, &nav, navbr);
        DeleteObject(navbr);

        RECT nbdr = {0, HDR_H + NAV_H - 1, w, HDR_H + NAV_H};
        HBRUSH nbr = CreateSolidBrush(C_BORDER);
        FillRect(dc, &nbdr, nbr);
        DeleteObject(nbr);

        /* Active tab underline */
        int nw = w / PAGE_COUNT;
        RECT ul = {(int)g_page * nw, HDR_H + NAV_H - 3, (int)g_page * nw + nw, HDR_H + NAV_H};
        FillRect(dc, &ul, g_brAccent);

        /* ---- Status Bar ---- */
        RECT sta = {0, h - STA_H, w, h};
        HBRUSH sbr = CreateSolidBrush(C_HDR);
        FillRect(dc, &sta, sbr);
        DeleteObject(sbr);

        RECT stbdr = {0, h - STA_H, w, h - STA_H + 1};
        HBRUSH stbb = CreateSolidBrush(C_BORDER);
        FillRect(dc, &stbdr, stbb);
        DeleteObject(stbb);

        const wchar_t *sta_txt = g_scan_done ?
            L"System scan complete  |  All checks finished" :
            L"Scanning system...";
        draw_text(dc, sta_txt, g_fSmall, C_MUTE, PAD, h - STA_H + 7, w - PAD*2 - 100, 18, DT_LEFT);

        /* ---- Page Contents ---- */
        switch (g_page) {
        case PAGE_HOME:    paint_home(dc);    break;
        case PAGE_SYSTEM:  paint_system(dc);  break;
        case PAGE_OPTIONS: paint_options(dc); break;
        case PAGE_ENGINE:  paint_engine(dc);  break;
        default: break;
        }

        /* BitBlt atomic transfer */
        BitBlt(hdcWin, 0, 0, w, h, dc, 0, 0, SRCCOPY);

        SelectObject(dc, hbmOld);
        DeleteObject(hbm);
        DeleteDC(dc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    /* ---- Owner-draw buttons ---------------------------------- */
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        int id = di->CtlID;

        /* Nav buttons */
        if (id >= ID_NAV_BASE && id < ID_NAV_BASE + PAGE_COUNT) {
            int idx = id - ID_NAV_BASE;
            BOOL active = (idx == (int)g_page);
            BOOL hov    = (di->itemState & ODS_HOTLIGHT) != 0;
            COLORREF bg = active ? C_NAV_ACT : (hov ? C_NAV_HVR : C_HDR);
            COLORREF tc = active ? C_ACCENT : C_DIM;
            HDC dc = di->hDC; RECT r = di->rcItem;
            HBRUSH br = CreateSolidBrush(bg);
            FillRect(dc, &r, br); DeleteObject(br);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, tc);
            SelectObject(dc, g_fNav);
            DrawTextW(dc, id == ID_NAV_BASE + PAGE_HOME ? L"Home" :
                          id == ID_NAV_BASE + PAGE_SYSTEM  ? L"System" :
                          id == ID_NAV_BASE + PAGE_OPTIONS ? L"Options" : L"Engine",
                      -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            return TRUE;
        }

        /* PLAY button */
        if (id == ID_BTN_PLAY) {
            BOOL pressed = (di->itemState & ODS_SELECTED) != 0;
            paint_ow_button(di, pressed ? C_ACCP : C_ACCENT, C_ACCP, C_BTNTXT, TRUE);
            return TRUE;
        }

        if (id == ID_BTN_CTRL || id == ID_BTN_RESCAN || id == ID_BTN_GETMESA || id == ID_BTN_LOCATE_MESA) {
            paint_ow_button(di, C_CARD2, C_CARD, C_TEXT, FALSE);
            return TRUE;
        }

        if (id == ID_BTN_DL_MESA || id == ID_BTN_DL_VCRT) {
            paint_ow_button(di, C_ACCP, RGB(0x20,0x80,0x45), C_TEXT, FALSE);
            return TRUE;
        }

        return DefWindowProc(hwnd, msg, wp, lp);
    }

    /* ---- WM_CTLCOLOR — Dark theming for controls & dropdowns -- */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brCard;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSCROLLBAR: {
        HDC hdc = (HDC)wp;
        SetBkColor(hdc, C_CARD2);
        SetTextColor(hdc, C_TEXT);
        return (LRESULT)g_brCard2;
    }

    /* ---- WM_COMMAND ------------------------------------------ */
    case WM_COMMAND: {
        int cid = LOWORD(wp);

        /* Nav tabs */
        if (cid >= ID_NAV_BASE && cid < ID_NAV_BASE + PAGE_COUNT) {
            set_page((Page)(cid - ID_NAV_BASE));
            return 0;
        }

        /* Launch game */
        if (cid == ID_BTN_PLAY) {
            write_config();
            char exe[1024];
            _snprintf_s(exe, sizeof(exe), _TRUNCATE, "%s\\sprout.exe", g_exeDir);
            if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(hwnd, L"sprout.exe not found next to launcher.", L"Sprout", MB_ICONERROR);
                return 0;
            }

            /* Backup saves before launch */
            if (g_persistSaves) {
                backup_saves(hwnd);
            }

            wchar_t wexe[1024], wdir[1024];
            _snwprintf_s(wexe, sizeof(wexe)/sizeof(wchar_t), _TRUNCATE, L"\"%hs\"", exe);
            _snwprintf_s(wdir, sizeof(wdir)/sizeof(wchar_t), _TRUNCATE, L"%hs", g_exeDir);
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            DWORD fl = CREATE_DEFAULT_ERROR_MODE;
            if (!g_showConsole) fl |= DETACHED_PROCESS;
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOW;
            if (!CreateProcessW(NULL, wexe, NULL, NULL, FALSE, fl, NULL, wdir, &si, &pi)) {
                wchar_t ebuf[256];
                _snwprintf_s(ebuf, sizeof(ebuf)/sizeof(wchar_t), _TRUNCATE,
                    L"Launch failed (error %lu).", GetLastError());
                MessageBoxW(hwnd, ebuf, L"Sprout", MB_ICONERROR);
                return 0;
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            /* Write PID so crash handler can locate launcher window */
            char pid_path[1024];
            _snprintf_s(pid_path, sizeof(pid_path), _TRUNCATE, "%s\\sprout.pid", g_exeDir);
            FILE *pf = NULL;
            if (fopen_s(&pf, pid_path, "w") == 0 && pf) {
                fprintf(pf, "%lu", pi.dwProcessId);
                fclose(pf);
            }
            return 0;
        }

        /* Controls dialog */
        if (cid == ID_BTN_CTRL) {
            open_controls_dialog(hwnd);
            return 0;
        }

        /* Re-scan */
        if (cid == ID_BTN_RESCAN) {
            scan_start(hwnd, g_exeDir);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Download Mesa3D */
        if (cid == ID_BTN_DL_MESA || cid == ID_BTN_GETMESA) {
            scan_download_mesa(hwnd, g_exeDir);
            ShowWindow(g_hDlMesa, SW_HIDE);
            return 0;
        }

        /* Locate/Import Mesa3D */
        if (cid == ID_BTN_LOCATE_MESA) {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Mesa3D Files (*.dll;*.7z;*.zip)\0*.dll;*.7z;*.zip\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrTitle = L"Select Mesa3D opengl32.dll or package (.7z/.zip)";

            if (GetOpenFileNameW(&ofn)) {
                if (scan_import_mesa(hwnd, g_exeDir, file)) {
                    MessageBoxW(hwnd, L"Mesa3D successfully imported! Software rendering is ready.", L"Sprout Launcher", MB_ICONINFORMATION);
                    set_page(g_page);
                } else {
                    MessageBoxW(hwnd, L"Could not import Mesa3D. Please select a valid opengl32.dll or Mesa3D package.", L"Sprout Launcher", MB_ICONERROR);
                }
            }
            return 0;
        }

        /* Download VC++ Runtime */
        if (cid == ID_BTN_DL_VCRT) {
            scan_download_vcrt(hwnd, g_exeDir);
            ShowWindow(g_hDlVcrt, SW_HIDE);
            return 0;
        }

        /* Combobox selection change */
        if (HIWORD(wp) == CBN_SELCHANGE) {
            if (cid == ID_CMB_VMODE) {
                int sel = (int)SendMessage(g_hVMode, CB_GETCURSEL, 0, 0);
                strncpy_s(g_videoMode, sizeof(g_videoMode),
                          sel == 1 ? "fullscreen" : "window", _TRUNCATE);
            } else if (cid == ID_CMB_FPS) {
                int sel = (int)SendMessage(g_hFps, CB_GETCURSEL, 0, 0);
                g_fpsLimit = (sel >= 0 && sel < kNumFpsLimit) ? kFpsLimit[sel].val : 60;
            } else if (cid == ID_CMB_LANG) {
                int sel = (int)SendMessage(g_hLang, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < kNumLangs)
                    strncpy_s(g_locale, sizeof(g_locale), kLangs[sel].code, _TRUNCATE);
            } else if (cid == ID_CMB_REND) {
                int sel = (int)SendMessage(g_hRend, CB_GETCURSEL, 0, 0);
                strncpy_s(g_renderer, sizeof(g_renderer),
                          sel == 1 ? "software" : "hardware", _TRUNCATE);
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (cid == ID_CMB_MSAA) {
                int sel = (int)SendMessage(g_hMsaa, CB_GETCURSEL, 0, 0);
                int msaa_vals[] = {0, 2, 4, 8};
                if (sel >= 0 && sel < 4) g_gl_msaa = msaa_vals[sel];
            }
            write_config();
        }

        /* Checkbox click */
        if (HIWORD(wp) == BN_CLICKED) {
            if (cid == ID_CHK_VSYNC) g_vsync       = (int)(SendMessage(g_hVsync,BM_GETCHECK,0,0)==BST_CHECKED);
            if (cid == ID_CHK_IAP)   g_emulateIap   = (int)(SendMessage(g_hIap,  BM_GETCHECK,0,0)==BST_CHECKED);
            if (cid == ID_CHK_SAVES) g_persistSaves  = (int)(SendMessage(g_hSaves,BM_GETCHECK,0,0)==BST_CHECKED);
            if (cid == ID_CHK_CON)   g_showConsole   = (int)(SendMessage(g_hCon,  BM_GETCHECK,0,0)==BST_CHECKED);
            write_config();
        }

        return 0;
    }

    /* ---- WM_CLOSE / WM_DESTROY ------------------------------- */
    case WM_CLOSE:
        write_config();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TMR_ANIM);
        delete_fonts();
        delete_brushes();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ================================================================
 *  backup_saves — backs up save directory before launching
 * ================================================================ */
static void backup_saves(HWND hwnd) {
    char save_dir[1024], bak_dir[1024];
    _snprintf_s(save_dir, sizeof(save_dir), _TRUNCATE, "%s\\save", g_exeDir);
    DWORD attr = GetFileAttributesA(save_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return; /* no save dir yet */

    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(bak_dir, sizeof(bak_dir), _TRUNCATE,
        "%s\\save_backup_%04d-%02d-%02d_%02d%02d%02d",
        g_exeDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    /* Use SHFileOperation for recursive copy (no extra deps) */
    SHFILEOPSTRUCTA op = {0};
    char from[1024], to[1024];
    _snprintf_s(from, sizeof(from), _TRUNCATE, "%s\0", save_dir);
    _snprintf_s(to, sizeof(to), _TRUNCATE, "%s\0", bak_dir);
    op.hwnd   = hwnd;
    op.wFunc  = FO_COPY;
    op.pFrom  = from;
    op.pTo    = to;
    op.fFlags = FOF_NO_UI;
    SHFileOperationA(&op);
}

/* ================================================================
 *  WinMain
 * ================================================================ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    InitCommonControls();
    get_paths();
    init_binds();
    read_config();
    read_controls();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"SproutLauncherV2";
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    if (!RegisterClassW(&wc)) return 1;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    RECT wr = {(sw-WW2)/2, (sh-WH2)/2, (sw-WW2)/2+WW2, (sh-WH2)/2+WH2};
    AdjustWindowRect(&wr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX, FALSE);

    HWND hwnd = CreateWindowW(
        L"SproutLauncherV2", L"Sprout Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        wr.left, wr.top, wr.right-wr.left, wr.bottom-wr.top,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

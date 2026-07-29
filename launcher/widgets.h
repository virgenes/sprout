/* Widget creation helpers for the launcher's Win32 GUI.
 * All return the created HWND. */

#ifndef SPROUT_WIDGETS_H
#define SPROUT_WIDGETS_H

#include <windows.h>
#include <commctrl.h>

typedef struct { int val; const wchar_t *label; } ComboItem;
typedef struct { const char *code; const wchar_t *label; } LangItem;

static inline HWND w_btn(HWND parent, const wchar_t *text, int x, int y, int w, int h, int id, HFONT font, DWORD extra) {
    return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | extra,
                         x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
}

static inline HWND w_btn_ow(HWND parent, const wchar_t *text, int x, int y, int w, int h, int id, HFONT font) {
    HWND hw = w_btn(parent, text, x, y, w, h, id, font, BS_PUSHBUTTON | BS_OWNERDRAW);
    if (font) SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static inline HWND w_chk(HWND parent, const wchar_t *text, int x, int y, int w, int h, int id, HFONT font, int checked) {
    HWND hw = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (font) SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessage(hw, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return hw;
}

static inline HWND w_lbl(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                            x, y, w, h, parent, NULL, NULL, NULL);
    if (font) SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static inline HWND w_lbl_c(HWND parent, const wchar_t *text, int x, int y, int w, int h, HFONT font) {
    HWND hw = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER,
                            x, y, w, h, parent, NULL, NULL, NULL);
    if (font) SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static inline HWND w_cmb(HWND parent, int x, int y, int w, int h, int id, HFONT font) {
    HWND hw = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_HASSTRINGS | WS_TABSTOP,
                            x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (font) SendMessageW(hw, WM_SETFONT, (WPARAM)font, TRUE);
    return hw;
}

static inline void w_cmb_add(HWND h, const ComboItem *items, int n) {
    for (int a = 0; a < n; a++) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)items[a].label);
}

static inline void w_cmb_set(HWND h, const ComboItem *items, int n, int val) {
    for (int a = 0; a < n; a++) if (items[a].val == val) { SendMessage(h, CB_SETCURSEL, a, 0); break; }
}

static inline int w_cmb_val(HWND h, const ComboItem *items) {
    int s = (int)SendMessage(h, CB_GETCURSEL, 0, 0);
    return s >= 0 ? items[s].val : 0;
}

#endif

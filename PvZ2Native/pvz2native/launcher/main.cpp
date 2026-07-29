#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'\"")

#include <sprout/actions.h>
#include "gamepad_icon.h"
#include "controls_dialog.h"
#include "config.h"

static HBITMAP load_bmp_from_mem(const unsigned char *data, int sz) {
    if (!data || sz < 54) return NULL;
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER*)(data + 14);
    int w = bi->biWidth, h = abs(bi->biHeight);
    int row = ((w * bi->biBitCount + 31) / 32) * 4;
    void *bits = NULL;
    HDC dc = GetDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, (BITMAPINFO*)bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && bits && data + 54 + h * row <= data + sz) {
        for (int y = 0; y < h; y++)
            memcpy((char*)bits + y * row, data + 54 + y * row, row);
    }
    ReleaseDC(NULL, dc);
    return bmp;
}

static HINSTANCE g_hInst = NULL;
static HWND g_hWnd = NULL, g_hPlayBtn = NULL, g_hStatus = NULL, g_hCtrlBtn = NULL;
/* Options tab controls */
static HWND g_hFpsLimit = NULL, g_hLang = NULL, g_hConsole = NULL;
static HWND g_hEmulateIap = NULL, g_hVideoMode = NULL;
static HWND g_hPersistSaves = NULL;
static HWND g_hQuality = NULL, g_hShadows = NULL, g_hVsync = NULL, g_hRenderScale = NULL;
/* Engine tab controls */
static HWND g_hRenderer = NULL;
/* Tab buttons */
static HWND g_hTabOpt = NULL, g_hTabEng = NULL;

/* All option controls for show/hide */
static HWND g_optCtls[12]; static int g_nOpt;
/* All engine controls for show/hide */
static HWND g_engCtls[4]; static int g_nEng;

static HBITMAP g_hGamepadIcon = NULL;

static void set_dark_title(HWND h){BOOL d=TRUE;DwmSetWindowAttribute(h,20,&d,sizeof(d));}

static void show_tab(int tab) {
    g_engineTab = tab;
    for (int i = 0; i < g_nOpt; i++) ShowWindow(g_optCtls[i], tab == 0 ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < g_nEng; i++) ShowWindow(g_engCtls[i], tab == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hWnd, NULL, TRUE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    static HFONT hF=0,hB=0,hT=0,hS=0,hTabF=0;
    static HBRUSH hBg=0;
    switch(msg){
        case WM_CREATE:{
            set_dark_title(hwnd);
            NONCLIENTMETRICSW n={sizeof(n)}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(n),&n,0);
            LOGFONTW lf=n.lfMessageFont;
            lf.lfHeight=-15; wcscpy_s(lf.lfFaceName,LF_FACESIZE,L"Segoe UI"); hF=CreateFontIndirectW(&lf);
            lf.lfHeight=-14; lf.lfWeight=FW_NORMAL; hS=CreateFontIndirectW(&lf);
            lf.lfHeight=-18; lf.lfWeight=FW_SEMIBOLD; hB=CreateFontIndirectW(&lf);
            lf.lfHeight=-28; lf.lfWeight=FW_BOLD; hT=CreateFontIndirectW(&lf);
            lf.lfHeight=-13; lf.lfWeight=FW_NORMAL; hTabF=CreateFontIndirectW(&lf);
            hBg=CreateSolidBrush(COL_BG);

            /* Tab buttons at top of card */
            #define TAB_Y 104
            #define TAB_W 120
            #define TAB_H2 26
            int tab_off = (CARD_W - TAB_W * 2) / 2;
            g_hTabOpt = w_btn_ow(hwnd, L"Options", CARD_L + tab_off, TAB_Y, TAB_W, TAB_H2, 2001, hTabF);
            g_hTabEng = w_btn_ow(hwnd, L"Engine", CARD_L + tab_off + TAB_W, TAB_Y, TAB_W, TAB_H2, 2002, hTabF);

            /* Options tab controls (shifted down by TAB_H from originals) */
            #define T TAB_H
            #define R1 (145+TAB_H)   /* Video mode */
            #define R2 (185+TAB_H)   /* FPS limit */
            #define R3 (248+TAB_H)   /* Quality */
            #define R4 (288+TAB_H)   /* Shadows */
            #define R5 (328+TAB_H)   /* Render scale */
            #define R6 (398+TAB_H)   /* Language */
            #define R7 (440+TAB_H)   /* Emulate IAP */
            #define R8 (480+TAB_H)   /* Persist saves */
            #define R9 (520+TAB_H)   /* Console */

            g_hVideoMode = w_cmb(hwnd, CTRL_X, R1, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hVideoMode, kVideoMode, kNumVideoMode);
            SendMessage(g_hVideoMode, CB_SETCURSEL, _stricmp(g_videoMode,"native")==0 ? 1 : 0, 0);

            g_hFpsLimit = w_cmb(hwnd, CTRL_X, R2, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hFpsLimit, kFpsLimit, kNumFpsLimit);
            w_cmb_set(g_hFpsLimit, kFpsLimit, kNumFpsLimit, g_fpsLimit);

            g_hVsync = w_chk(hwnd, L"V-Sync", CTRL_X + CTRL_W + 10, R2, 80, 24, 0, hF, g_vsync);

            g_hQuality = w_cmb(hwnd, CTRL_X, R3, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hQuality, kQuality, kNumQuality);
            {int idx=0; if(_stricmp(g_quality,"medium")==0)idx=1; else if(_stricmp(g_quality,"low")==0)idx=2; SendMessage(g_hQuality,CB_SETCURSEL,idx,0);}

            g_hShadows = w_cmb(hwnd, CTRL_X, R4, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hShadows, kShadows, kNumShadows);
            {int idx=0; if(_stricmp(g_shadows,"medium")==0)idx=1; else if(_stricmp(g_shadows,"low")==0)idx=2; SendMessage(g_hShadows,CB_SETCURSEL,idx,0);}

            g_hRenderScale = w_cmb(hwnd, CTRL_X, R5, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hRenderScale, kRenderScale, kNumRenderScale);
            {int idx=2; if(strcmp(g_renderScale,"0.5")==0)idx=0; else if(strcmp(g_renderScale,"0.75")==0)idx=1; else if(strcmp(g_renderScale,"1.25")==0)idx=3; else if(strcmp(g_renderScale,"1.5")==0)idx=4; SendMessage(g_hRenderScale,CB_SETCURSEL,idx,0);}

            g_hLang = w_cmb(hwnd, CTRL_X, R6, CTRL_W, 200, 0, hF);
            for(int i=0;i<kNumLangs;i++)SendMessageW(g_hLang,CB_ADDSTRING,0,(LPARAM)kLangs[i].label);
            SendMessage(g_hLang,CB_SETCURSEL,lang_idx(g_locale),0);

            g_hEmulateIap = w_chk(hwnd, L"Emulate in-app purchases (free shop)", CTRL_X, R7, CTRL_W+80, 24, 0, hF, g_emulateIap);
            g_hPersistSaves = w_chk(hwnd, L"Persist saved settings", CTRL_X, R8, CTRL_W+60, 24, 0, hF, g_persistSaves);
            g_hConsole = w_chk(hwnd, L"Show console window", CTRL_X, R9, CTRL_W+60, 24, 0, hF, g_showConsole);

            /* Register option controls for show/hide */
            g_nOpt = 0;
            g_optCtls[g_nOpt++] = g_hVideoMode;
            g_optCtls[g_nOpt++] = g_hFpsLimit;
            g_optCtls[g_nOpt++] = g_hVsync;
            g_optCtls[g_nOpt++] = g_hQuality;
            g_optCtls[g_nOpt++] = g_hShadows;
            g_optCtls[g_nOpt++] = g_hRenderScale;
            g_optCtls[g_nOpt++] = g_hLang;
            g_optCtls[g_nOpt++] = g_hEmulateIap;
            g_optCtls[g_nOpt++] = g_hPersistSaves;
            g_optCtls[g_nOpt++] = g_hConsole;

            /* ── Engine tab controls ── */
            #define ER_Y (170+TAB_H)
            int er_lbl_y = ER_Y + 4;
            g_hRenderer = w_cmb(hwnd, CTRL_X, ER_Y, CTRL_W, 200, 0, hF);
            w_cmb_add(g_hRenderer, kRenderer, kNumRenderer);
            w_cmb_set(g_hRenderer, kRenderer, kNumRenderer, _stricmp(g_renderer,"software")==0 ? 1 : 0);

            g_nEng = 0;
            g_engCtls[g_nEng++] = g_hRenderer;

            /* ── Buttons (always visible) ── */
            g_hGamepadIcon = load_bmp_from_mem(kGamepadIcon, GAMEPAD_ICON_SIZE);
            g_hCtrlBtn = w_btn_ow(hwnd, L"", (WW-280)/2-8-48, BTN_Y+2, 48, 48, 1002, hS);

            g_hPlayBtn = w_btn_ow(hwnd, L"\u25B6  PLAY", (WW-280)/2, BTN_Y, 280, 52, 1001, hB);
            g_hStatus = w_lbl_c(hwnd, L"Ready", MARGIN, WH-44, WW-MARGIN*2, 20, hS);
            break;
        }

        case WM_DESTROY:{
            if(g_hGamepadIcon)DeleteObject(g_hGamepadIcon);
            if(hF)DeleteObject(hF); if(hB)DeleteObject(hB);
            if(hT)DeleteObject(hT); if(hS)DeleteObject(hS); if(hTabF)DeleteObject(hTabF);
            if(hBg)DeleteObject(hBg);
            PostQuitMessage(0); return 0;
        }

        case WM_CTLCOLORSTATIC:{
            HDC dc=(HDC)wp;
            SetBkColor(dc,COL_BG);
            SetTextColor(dc,(HWND)lp==g_hStatus?COL_DIM:COL_TEXT);
            return(LRESULT)hBg;
        }

        case WM_CTLCOLORBTN: return(LRESULT)GetStockObject(DC_BRUSH);

        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSCROLLBAR:{
            HDC dc=(HDC)wp;
            SetBkColor(dc,COL_CARD); SetTextColor(dc,COL_TEXT);
            return(LRESULT)GetStockObject(DC_BRUSH);
        }

        case WM_DRAWITEM:{
            DRAWITEMSTRUCT*di=(DRAWITEMSTRUCT*)lp;
            BOOL p=di->itemState&ODS_SELECTED;
            HDC dc=di->hDC; RECT r=di->rcItem;
            /* Tab buttons */
            if(di->hwndItem==g_hTabOpt || di->hwndItem==g_hTabEng){
                int active = (di->hwndItem==g_hTabOpt && g_engineTab==0) ||
                             (di->hwndItem==g_hTabEng && g_engineTab==1);
                COLORREF bg = p ? COL_ACCP : active ? COL_ACCENT : COL_CARD;
                COLORREF fg = active ? COL_BTNTXT : COL_DIM;
                HBRUSH br=CreateSolidBrush(bg); FillRect(dc,&r,br); DeleteObject(br);
                SetBkMode(dc,TRANSPARENT); SetTextColor(dc,fg);
                wchar_t t[32]; GetWindowTextW(di->hwndItem,t,32);
                DrawTextW(dc,t,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                return TRUE;
            }
            if(di->hwndItem==g_hPlayBtn){
                COLORREF bg=p?COL_ACCP:COL_ACCENT;
                InflateRect(&r,2,2);
                HBRUSH br=CreateSolidBrush(bg); FillRect(dc,&r,br); DeleteObject(br);
                SetBkMode(dc,TRANSPARENT);
                SetTextColor(dc,COL_BTNTXT);
                wchar_t t[32]; GetWindowTextW(g_hPlayBtn,t,32);
                DrawTextW(dc,t,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                return TRUE;
            }
            if(di->CtlID==1002){
                HBRUSH br=CreateSolidBrush((di->itemState&ODS_SELECTED)?COL_ACCP:COL_CARD);
                FillRect(dc,&r,br); DeleteObject(br);
                if(g_hGamepadIcon){
                    HDC mem=CreateCompatibleDC(dc);
                    HBITMAP ob=(HBITMAP)SelectObject(mem,g_hGamepadIcon);
                    BITMAP bm; GetObject(g_hGamepadIcon,sizeof(bm),&bm);
                    int iw=bm.bmWidth, ih=bm.bmHeight;
                    int x=r.left+(r.right-r.left-iw)/2;
                    int y=r.top+(r.bottom-r.top-ih)/2;
                    BLENDFUNCTION bf={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
                    AlphaBlend(dc,x,y,iw,ih,mem,0,0,iw,ih,bf);
                    SelectObject(mem,ob); DeleteDC(mem);
                }
                return TRUE;
            }
            return DefWindowProc(hwnd,msg,wp,lp);
        }

        case WM_COMMAND:{
            /* Tab switching */
            if(LOWORD(wp)==2001){ show_tab(0); return 0; }
            if(LOWORD(wp)==2002){ show_tab(1); return 0; }
            if(LOWORD(wp)==1002){open_controls_dialog(hwnd); return 0;}
            if(LOWORD(wp)==1001){
                int vm = (int)SendMessage(g_hVideoMode,CB_GETCURSEL,0,0);
                strncpy_s(g_videoMode,sizeof(g_videoMode),vm==1?"native":"auto",_TRUNCATE);
                g_fpsLimit=w_cmb_val(g_hFpsLimit,kFpsLimit);
                int ls=(int)SendMessage(g_hLang,CB_GETCURSEL,0,0);
                if(ls>=0&&ls<kNumLangs)strncpy_s(g_locale,sizeof(g_locale),kLangs[ls].code,_TRUNCATE);
                g_emulateIap=(SendMessage(g_hEmulateIap,BM_GETCHECK,0,0)==BST_CHECKED);
                g_persistSaves=(SendMessage(g_hPersistSaves,BM_GETCHECK,0,0)==BST_CHECKED);
                g_showConsole=(SendMessage(g_hConsole,BM_GETCHECK,0,0)==BST_CHECKED);
                g_vsync=(SendMessage(g_hVsync,BM_GETCHECK,0,0)==BST_CHECKED);
                {int q=(int)SendMessage(g_hQuality,CB_GETCURSEL,0,0); strncpy_s(g_quality,sizeof(g_quality),q==2?"low":q==1?"medium":"high",_TRUNCATE);}
                {int s=(int)SendMessage(g_hShadows,CB_GETCURSEL,0,0); strncpy_s(g_shadows,sizeof(g_shadows),s==2?"low":s==1?"medium":"high",_TRUNCATE);}
                {int rs=(int)SendMessage(g_hRenderScale,CB_GETCURSEL,0,0); strncpy_s(g_renderScale,sizeof(g_renderScale),rs==0?"0.5":rs==1?"0.75":rs==3?"1.25":rs==4?"1.5":"1.0",_TRUNCATE);}
                /* Read renderer from engine tab */
                {int ri=(int)SendMessage(g_hRenderer,CB_GETCURSEL,0,0); strncpy_s(g_renderer,sizeof(g_renderer),ri==1?"software":"hardware",_TRUNCATE);}
                write_config();
                SetWindowTextW(g_hStatus,L"Launching...");
                EnableWindow(g_hPlayBtn,FALSE);
                wchar_t cmd[1024]; _snwprintf_s(cmd,sizeof(cmd)/sizeof(wchar_t),_TRUNCATE,L"\"%hs\"",g_gamePath);
                STARTUPINFOW si={sizeof(si)}; PROCESS_INFORMATION pi;
                DWORD fl=CREATE_DEFAULT_ERROR_MODE;
                if(!g_showConsole)fl|=CREATE_NO_WINDOW;
                else{si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_SHOW;}
                if(CreateProcessW(NULL,cmd,NULL,NULL,FALSE,fl,NULL,NULL,&si,&pi)){
                    CloseHandle(pi.hThread); WaitForSingleObject(pi.hProcess,INFINITE);
                    CloseHandle(pi.hProcess);
                    SetWindowTextW(g_hStatus,L"Game closed. Press PLAY to launch again.");
                }else{
                    wchar_t e[256]; _snwprintf_s(e,sizeof(e)/sizeof(wchar_t),_TRUNCATE,L"Error %lu launching:\n%hs",GetLastError(),g_gamePath);
                    MessageBoxW(hwnd,e,L"Launch Error",MB_ICONERROR);
                    SetWindowTextW(g_hStatus,L"Launch failed.");
                }
                EnableWindow(g_hPlayBtn,TRUE); SetForegroundWindow(g_hWnd); return 0;
            }
            break;
        }

        case WM_PAINT:{
            PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
            RECT cr; GetClientRect(hwnd,&cr); FillRect(dc,&cr,hBg);
            HBRUSH ab=CreateSolidBrush(COL_ACCENT);
            RECT bar={0,0,WW,3}; FillRect(dc,&bar,ab); DeleteObject(ab);
            SetBkMode(dc,TRANSPARENT);
            SelectObject(dc,hT); SetTextColor(dc,COL_TEXT);
            RECT tr={MARGIN,22,WW-MARGIN,60};
            DrawTextW(dc,L"Sprout",-1,&tr,DT_LEFT|DT_SINGLELINE);
            SelectObject(dc,hS); SetTextColor(dc,COL_MUTE);
            RECT sr={MARGIN,54,WW-MARGIN,76};
            DrawTextW(dc,L"Plants vs. Zombies 2 \u2014 Native Windows Port",-1,&sr,DT_LEFT|DT_SINGLELINE);
            RECT card={CARD_L,100, CARD_R, BTN_Y-10};
            HBRUSH cb=CreateSolidBrush(COL_CARD);
            FillRect(dc,&card,cb); DeleteObject(cb);
            HPEN dp=CreatePen(PS_SOLID,1,RGB(0x32,0x32,0x46));
            HPEN op=(HPEN)SelectObject(dc,dp);
            if(g_engineTab==0){
                MoveToEx(dc,CARD_L+16,215+TAB_H,NULL); LineTo(dc,CARD_R-16,215+TAB_H);
                MoveToEx(dc,CARD_L+16,370+TAB_H,NULL); LineTo(dc,CARD_R-16,370+TAB_H);
            } else {
                MoveToEx(dc,CARD_L+16,145+TAB_H,NULL); LineTo(dc,CARD_R-16,145+TAB_H);
            }
            SelectObject(dc,op); DeleteObject(dp);
            SelectObject(dc,hS); SetTextColor(dc,COL_SECTION);
            auto section=[&](int y,const wchar_t*t){
                RECT r={LABEL_X,y,WW-LABEL_X,y+20};
                DrawTextW(dc,t,-1,&r,DT_LEFT);
            };
            if(g_engineTab==0){
                section(116+TAB_H,L"VIDEO");
                section(225+TAB_H,L"GRAPHICS");
                section(380+TAB_H,L"GAME");
            } else {
                section(116+TAB_H,L"RENDERER");
            }
            SelectObject(dc,hF); SetTextColor(dc,COL_DIM);
            auto label=[&](int y,const wchar_t*t){
                RECT r={LABEL_X,y,CTRL_X-8,y+28};
                DrawTextW(dc,t,-1,&r,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
            };
            if(g_engineTab==0){
                label(145+TAB_H,L"Window mode");
                label(185+TAB_H,L"FPS limit");
                label(248+TAB_H,L"Quality");
                label(288+TAB_H,L"Shadows");
                label(328+TAB_H,L"Render scale");
                label(398+TAB_H,L"Language");
            } else {
                label(170+TAB_H,L"Renderer");
                /* Info text */
                SetTextColor(dc,COL_MUTE);
                RECT ir={LABEL_X,200+TAB_H,CTRL_X+CTRL_W,280+TAB_H};
                DrawTextW(dc,L"Hardware: uses GPU acceleration.\nSoftware: renders on CPU via\nMesa3D (for old GPUs).",-1,&ir,DT_LEFT|DT_WORDBREAK);
            }
            EndPaint(hwnd,&ps);
            return 0;
        }

        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst,HINSTANCE,LPSTR,int){
    g_hInst=hInst; get_paths(); init_binds(); read_config(); read_controls();
    if(GetFileAttributesA(g_gamePath)==INVALID_FILE_ATTRIBUTES){
        wchar_t m[512]; _snwprintf_s(m,sizeof(m)/sizeof(wchar_t),_TRUNCATE,L"Sprout not installed.\nExpected: %hs",g_gamePath);
        MessageBoxW(NULL,m,L"Error",MB_ICONERROR); return 1;
    }
    INITCOMMONCONTROLSEX ic={sizeof(ic),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc={}; wc.lpfnWndProc=wnd_proc; wc.hInstance=hInst;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=NULL;
    wc.lpszClassName=L"SproutLauncher";
    RegisterClassW(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    RECT wr={(sw-WW)/2,(sh-WH)/2,(sw-WW)/2+WW,(sh-WH)/2+WH};
    AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    g_hWnd=CreateWindowW(L"SproutLauncher",L"Sprout",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        wr.left,wr.top,wr.right-wr.left,wr.bottom-wr.top,
        NULL,NULL,hInst,NULL);
    if(!g_hWnd)return 1;
    /* Show correct tab on startup */
    show_tab(g_engineTab);
    ShowWindow(g_hWnd,SW_SHOW); UpdateWindow(g_hWnd);
    MSG msg; while(GetMessage(&msg,NULL,0,0)){TranslateMessage(&msg);DispatchMessage(&msg);}
    return 0;
}

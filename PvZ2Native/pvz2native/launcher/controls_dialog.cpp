#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "controls_dialog.h"
#include "widgets.h"

#pragma comment(lib, "dwmapi.lib")

static void set_dark_title(HWND h){BOOL d=TRUE;DwmSetWindowAttribute(h,20,&d,sizeof(d));}

static HWND g_hCd = NULL;
static HWND g_hRowBtn[NUM_BINDS], g_hRowLbl[NUM_BINDS];
static HWND g_hTabKb, g_hTabGp, g_hBtnOk, g_hBtnCancel, g_hBtnReset;
static int g_orig_kb[NUM_BINDS], g_orig_gp[NUM_BINDS];

static LRESULT CALLBACK controls_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    static HFONT hF=0,hB=0,hS=0;
    static HBRUSH hBg=0;
    switch(msg){
        case WM_CREATE:{
            g_hCd=hwnd;
            for(int i=0;i<NUM_BINDS;i++){g_orig_kb[i]=g_binds[i].kb; g_orig_gp[i]=g_binds[i].gp;}
            set_dark_title(hwnd);

            NONCLIENTMETRICSW n={sizeof(n)}; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(n),&n,0);
            LOGFONTW lf=n.lfMessageFont;
            lf.lfHeight=-14; wcscpy_s(lf.lfFaceName,LF_FACESIZE,L"Segoe UI"); hF=CreateFontIndirectW(&lf);
            lf.lfHeight=-20; lf.lfWeight=FW_SEMIBOLD; hB=CreateFontIndirectW(&lf);
            lf.lfHeight=-13; lf.lfWeight=FW_NORMAL; hS=CreateFontIndirectW(&lf);
            lf.lfHeight=-14; lf.lfWeight=FW_SEMIBOLD; HFONT hTab=CreateFontIndirectW(&lf);
            hBg=CreateSolidBrush(COL_BG);

            int cx=16;

            g_hTabKb = w_btn_ow(hwnd, L"Keyboard", 16, 12, 120, 30, 2001, hTab);
            g_hTabGp = w_btn_ow(hwnd, L"Gamepad", 140, 12, 120, 30, 2002, hTab);

            for(int i=0;i<NUM_BINDS;i++){
                int y=CD_TOP+i*CD_ROW_H;
                g_hRowLbl[i] = w_lbl(hwnd, g_binds[i].label, cx-4, y+4, 200, 24, hF);
                g_hRowBtn[i] = w_btn_ow(hwnd, L"", cx+210, y+2, 110, 28, 2100+i, hB);
            }

            g_hBtnOk = w_btn_ow(hwnd, L"Apply && Close", CD_W-370, CD_H-48, 140, 32, 2200, hF);
            g_hBtnCancel = w_btn_ow(hwnd, L"Cancel", CD_W-220, CD_H-48, 90, 32, 2201, hF);
            g_hBtnReset = w_btn_ow(hwnd, L"Reset", CD_W-120, CD_H-48, 90, 32, 2202, hF);

            g_capturing=-1;
            break;
        }
        case WM_DESTROY:{
            g_hCd=NULL;
            if(hF)DeleteObject(hF); if(hB)DeleteObject(hB); if(hS)DeleteObject(hS); if(hBg)DeleteObject(hBg);
            return 0;
        }
        case WM_CTLCOLORSTATIC:{
            HDC dc=(HDC)wp; SetBkColor(dc,COL_BG); SetTextColor(dc,COL_TEXT);
            return(LRESULT)hBg;
        }
        case WM_CTLCOLORBTN: return(LRESULT)GetStockObject(DC_BRUSH);
        case WM_ERASEBKGND: return 1;

        case WM_PAINT:{
            PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
            RECT cr; GetClientRect(hwnd,&cr); FillRect(dc,&cr,hBg);
            HBRUSH ab=CreateSolidBrush(COL_ACCENT);
            RECT bar={0,0,CD_W,3}; FillRect(dc,&bar,ab); DeleteObject(ab);
            HPEN dp=CreatePen(PS_SOLID,1,RGB(0x32,0x32,0x46));
            HPEN op=(HPEN)SelectObject(dc,dp);
            MoveToEx(dc,16,g_bind_tab?42:62,NULL);
            LineTo(dc,CD_W-16,g_bind_tab?42:62);
            SelectObject(dc,op); DeleteObject(dp);
            EndPaint(hwnd,&ps);
            return 0;
        }
        case WM_DRAWITEM:{
            DRAWITEMSTRUCT*di=(DRAWITEMSTRUCT*)lp;
            HDC dc=di->hDC; RECT r=di->rcItem;
            int id=di->CtlID;

            if(id==2001||id==2002){
                int sel=(id==2001&&g_bind_tab==0)||(id==2002&&g_bind_tab==1);
                COLORREF bg=sel?COL_ACCENT:COL_CARD;
                HBRUSH br=CreateSolidBrush(bg); FillRect(dc,&r,br); DeleteObject(br);
                SetBkMode(dc,TRANSPARENT); SetTextColor(dc,sel?COL_BTNTXT:COL_TEXT);
                wchar_t t[32]; GetWindowTextW((HWND)di->hwndItem,t,32);
                DrawTextW(dc,t,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                return TRUE;
            }

            if(id>=2100&&id<2100+NUM_BINDS){
                int idx=id-2100;
                BOOL pressed=di->itemState&ODS_SELECTED;
                BOOL capture=(idx==g_capturing);
                COLORREF bg=capture?RGB(0x3A,0x3A,0x50):(pressed?COL_ACCP:COL_CARD);
                HBRUSH br=CreateSolidBrush(bg); FillRect(dc,&r,br); DeleteObject(br);
                SetBkMode(dc,TRANSPARENT);
                SetTextColor(dc,capture?COL_ACCENT:COL_TEXT);
                wchar_t t[64];
                if(capture)
                    wcscpy_s(t,L"Press key / button...");
                else if(g_bind_tab==0){
                    const wchar_t*n=key_name(g_binds[idx].kb);
                    wsprintfW(t,L"[ %s ]",n);
                } else if (g_binds[idx].gp < 0) {
                    wcscpy_s(t,L"[ \u2013 ]");
                } else {
                    const wchar_t*n=gp_name(g_binds[idx].gp);
                    wsprintfW(t,L"[ %s ]",n);
                }
                DrawTextW(dc,t,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                return TRUE;
            }

            if(id>=2200&&id<=2202){
                BOOL h=di->itemState&ODS_SELECTED;
                COLORREF bg=h?COL_ACCP:COL_ACCENT;
                if(id==2201)bg=COL_CARD;
                if(id==2202)bg=RGB(0x50,0x30,0x30);
                HBRUSH br=CreateSolidBrush(bg); FillRect(dc,&r,br); DeleteObject(br);
                SetBkMode(dc,TRANSPARENT);
                SetTextColor(dc,(id==2201)?COL_DIM:(id==2202)?RGB(0xE0,0x60,0x60):COL_BTNTXT);
                wchar_t t[32]; GetWindowTextW((HWND)di->hwndItem,t,32);
                DrawTextW(dc,t,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                return TRUE;
            }
            return DefWindowProc(hwnd,msg,wp,lp);
        }

        case WM_COMMAND:{
            int id=LOWORD(wp);
            if(id==2001){g_bind_tab=0; g_capturing=-1; for(int i=0;i<NUM_BINDS;i++){ShowWindow(g_hRowLbl[i],SW_SHOW); ShowWindow(g_hRowBtn[i],SW_SHOW);} InvalidateRect(hwnd,NULL,TRUE); return 0;}
            if(id==2002){g_bind_tab=1; g_capturing=-1; for(int i=0;i<NUM_BINDS;i++){ShowWindow(g_hRowLbl[i],SW_SHOW); ShowWindow(g_hRowBtn[i],SW_SHOW);} InvalidateRect(hwnd,NULL,TRUE); return 0;}

            if(id>=2100&&id<2100+NUM_BINDS){
                int idx=id-2100;
                if(g_bind_tab==1){
                    HMENU menu=CreatePopupMenu();
                    AppendMenuW(menu,MF_STRING,3000,g_binds[idx].gp<0?L"\u2713 Unbound":L"   Unbound");
                    AppendMenuW(menu,MF_SEPARATOR,0,NULL);
                    for(int i=0;i<15;i++){
                        wchar_t label[32];
                        wsprintfW(label,L"  %s",gp_name(i));
                        if(g_binds[idx].gp==i)label[0]=0x2713;
                        AppendMenuW(menu,MF_STRING,3001+i,label);
                    }
                    RECT r; GetWindowRect(g_hRowBtn[idx],&r);
                    int sel=TrackPopupMenu(menu,TPM_LEFTALIGN|TPM_TOPALIGN|TPM_RETURNCMD,
                        r.left,r.bottom,0,hwnd,NULL);
                    if(sel==3000){g_binds[idx].gp=-1; InvalidateRect(g_hRowBtn[idx],NULL,TRUE);}
                    else if(sel>=3001&&sel<3016){g_binds[idx].gp=sel-3001; InvalidateRect(g_hRowBtn[idx],NULL,TRUE);}
                    DestroyMenu(menu);
                    return 0;
                }
                g_capturing=idx;
                SetFocus(hwnd);
                SetCapture(hwnd);
                for(int i=0;i<NUM_BINDS;i++)InvalidateRect(g_hRowBtn[i],NULL,TRUE);
                return 0;
            }

            if(id==2200){
                write_config();
                DestroyWindow(hwnd); return 0;
            }
            if(id==2201){
                for(int i=0;i<NUM_BINDS;i++){g_binds[i].kb=g_orig_kb[i]; g_binds[i].gp=g_orig_gp[i];}
                DestroyWindow(hwnd); return 0;
            }
            if(id==2202){
                for(int i=0;i<NUM_BINDS;i++){g_binds[i].kb=kActionDefs[i].def_kb; g_binds[i].gp=kActionDefs[i].def_gp;}
                g_capturing=-1; InvalidateRect(hwnd,NULL,TRUE); return 0;
            }
            break;
        }

        case WM_KEYDOWN:{
            if(g_capturing>=0){
                int sdlk=vk_to_sdlk((int)wp);
                if(sdlk){
                    g_binds[g_capturing].kb=sdlk;
                    g_capturing=-1;
                    ReleaseCapture();
                    for(int i=0;i<NUM_BINDS;i++)InvalidateRect(g_hRowBtn[i],NULL,TRUE);
                }
                return 0;
            }
            if(wp==VK_ESCAPE){
                for(int i=0;i<NUM_BINDS;i++){g_binds[i].kb=g_orig_kb[i]; g_binds[i].gp=g_orig_gp[i];}
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:{
            if(g_capturing>=0){
                g_capturing=-1; ReleaseCapture();
                for(int i=0;i<NUM_BINDS;i++)InvalidateRect(g_hRowBtn[i],NULL,TRUE);
            }
            return DefWindowProc(hwnd,msg,wp,lp);
        }

        case WM_CLOSE:{
            for(int i=0;i<NUM_BINDS;i++){g_binds[i].kb=g_orig_kb[i]; g_binds[i].gp=g_orig_gp[i];}
            DestroyWindow(hwnd); return 0;
        }
    }
    return DefWindowProc(hwnd,msg,wp,lp);
}

void open_controls_dialog(HWND parent){
    if(g_hCd){SetForegroundWindow(g_hCd); return;}
    WNDCLASSW wc={}; wc.lpfnWndProc=controls_proc; wc.hInstance=GetModuleHandle(NULL);
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=NULL;
    wc.lpszClassName=L"SproutControls";
    RegisterClassW(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    RECT wr={(sw-CD_W)/2,(sh-CD_H)/2,(sw-CD_W)/2+CD_W,(sh-CD_H)/2+CD_H};
    AdjustWindowRect(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE);
    HWND hwnd=CreateWindowW(L"SproutControls",L"Edit Controls",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        wr.left,wr.top,wr.right-wr.left,wr.bottom-wr.top,
        parent,NULL,GetModuleHandle(NULL),NULL);
    if(!hwnd)return;
    EnableWindow(parent,FALSE);
    ShowWindow(hwnd,SW_SHOW); UpdateWindow(hwnd);
    MSG msg; while(IsWindow(hwnd)&&GetMessage(&msg,NULL,0,0)){
        if(!IsDialogMessage(hwnd,&msg)){TranslateMessage(&msg);DispatchMessage(&msg);}
    }
    EnableWindow(parent,TRUE); SetForegroundWindow(parent);
}

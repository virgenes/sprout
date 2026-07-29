#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

/* ---------- Bindings ---------- */
BindItem g_binds[NUM_BINDS];
int g_bind_tab = 0;
int g_capturing = -1;

void init_binds(void) {
    for (int i = 0; i < ACT_NUM_ACTIONS; ++i) {
        g_binds[i] = {
            kActionDefs[i].name,
            kActionDefs[i].wlabel,
            kActionDefs[i].def_kb,
            kActionDefs[i].def_gp,
            kActionDefs[i].def_kb,
            kActionDefs[i].def_gp
        };
    }
}

/* ---------- Config globals ---------- */
char g_exeDir[1024], g_configPath[1024], g_gamePath[1024], g_obbPath[512];
int g_fpsLimit = 60, g_showConsole = 0, g_emulateIap = 1, g_persistSaves = 1, g_vsync = 1;
char g_locale[64] = "en_US";
char g_videoMode[16] = "auto";
char g_quality[16] = "high";
char g_shadows[16] = "high";
char g_renderScale[8] = "1.0";
char g_renderer[16] = "hardware";
int  g_gl_msaa   = 0;
int  g_engineTab = 0;

/* ---------- Combo data ---------- */
const ComboItem kFpsLimit[] = {{0,L"Uncapped"},{30,L"30 FPS"},{60,L"60 FPS"},{120,L"120 FPS"},{144,L"144 FPS"}};
const int kNumFpsLimit = 5;
const ComboItem kVideoMode[] = {{0,L"Auto (aspect)"},{1,L"Native (full)"}};
const int kNumVideoMode = 2;
const ComboItem kQuality[] = {{2,L"High"},{1,L"Medium"},{0,L"Low"}};
const int kNumQuality = 3;
const ComboItem kShadows[] = {{2,L"High"},{1,L"Medium"},{0,L"Low"}};
const int kNumShadows = 3;
const ComboItem kRenderScale[] = {{0,L"50%"},{1,L"75%"},{2,L"100%"},{3,L"125%"},{4,L"150%"}};
const int kNumRenderScale = 5;
const ComboItem kRenderer[] = {{0,L"Hardware (OpenGL)"},{1,L"Software (CPU)"}};
const int kNumRenderer = 2;

const LangItem kLangs[] = {
    {"en_US",L"English"},{"es_ES",L"Espa\u00F1ol"},{"fr_FR",L"Fran\u00E7ais"},
    {"de_DE",L"Deutsch"},{"it_IT",L"Italiano"},{"pt_BR",L"Portugu\u00EAs"},
    {"ru_RU",L"\u0420\u0443\u0441\u0441\u043A\u0438\u0439"},
    {"ja_JP",L"\u65E5\u672C\u8A9E"},{"ko_KR",L"\uD55C\uAD6C\uC5B4"},
    {"zh_CN",L"\u4E2D\u6587"},{"pl_PL",L"Polski"},{"nl_NL",L"Nederlands"},
};
const int kNumLangs = sizeof(kLangs)/sizeof(kLangs[0]);

int lang_idx(const char *c){
    for(int i=0;i<kNumLangs;i++)if(_stricmp(kLangs[i].code,c)==0)return i;
    return 0;
}

/* ---------- Key name helpers ---------- */
const wchar_t* gp_name(int btn) {
    if (btn < 0) return L"Unbound";
    static const wchar_t *names[] = {
        L"A", L"B", L"X", L"Y", L"Back", L"Guide", L"Start",
        L"L-Stick", L"R-Stick", L"L-Bumper", L"R-Bumper",
        L"D-Up", L"D-Down", L"D-Left", L"D-Right"
    };
    if (btn >= 0 && btn < 15) return names[btn];
    return L"?";
}

const wchar_t* key_name(int sdlk) {
    if (sdlk >= 33 && sdlk <= 126) { static wchar_t b[2]; b[0]=(wchar_t)sdlk; b[1]=0; return b; }
    if (sdlk == 27) return L"Escape"; if (sdlk == 9) return L"Tab";
    if (sdlk == 13) return L"Enter"; if (sdlk == 8) return L"Backspace";
    if (sdlk == 32) return L"Space";
    if (sdlk >= 0x4000003A && sdlk <= 0x40000045) {
        static wchar_t b[8]; wsprintfW(b, L"F%d", sdlk - 0x4000003A + 1); return b;
    }
    return L"?";
}

int vk_to_sdlk(int vk) {
    if (vk >= '0' && vk <= '9') return vk;
    if (vk >= 'A' && vk <= 'Z') return vk + 32;
    if (vk == VK_ESCAPE) return 27; if (vk == VK_TAB) return 9;
    if (vk == VK_RETURN) return 13; if (vk == VK_BACK) return 8;
    if (vk == VK_SPACE) return 32;
    if (vk >= VK_F1 && vk <= VK_F12) return 0x4000003A + (vk - VK_F1);
    return 0;
}

/* ---------- Paths ---------- */
void get_paths(){
    GetModuleFileNameA(NULL,g_exeDir,sizeof(g_exeDir));
    char*p=strrchr(g_exeDir,'\\');if(p)*p='\0';
    _snprintf_s(g_gamePath,sizeof(g_gamePath),_TRUNCATE,"%s\\sprout.exe",g_exeDir);
    _snprintf_s(g_configPath,sizeof(g_configPath),_TRUNCATE,"%s\\config.ini",g_exeDir);
}

static void auto_detect_obb(){
    char p[1024]; _snprintf_s(p,sizeof(p),_TRUNCATE,"%s\\lib\\*.obb",g_exeDir);
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA(p,&fd);
    if(h!=INVALID_HANDLE_VALUE){_snprintf_s(g_obbPath,sizeof(g_obbPath),_TRUNCATE,"lib/%s",fd.cFileName);FindClose(h);}
    else strncpy_s(g_obbPath,sizeof(g_obbPath),"lib/main.147.com.ea.game.pvz2_row.obb",_TRUNCATE);
}

/* ---------- Config read/write ---------- */
void read_config(){
    FILE*f=NULL; if(fopen_s(&f,g_configPath,"r")!=0||!f)return;
    char line[512],sec[64]="";
    while(fgets(line,sizeof(line),f)){
        char*t=line; while(*t==' '||*t=='\t')++t;
        if(t[0]=='['){char*cl=strchr(t+1,']');if(cl){*cl='\0';strncpy_s(sec,sizeof(sec),t+1,_TRUNCATE);}continue;}
        char*eq=strchr(t,'=');if(!eq)continue;
        *eq='\0';char*k=t,*v=eq+1;
        while(*k==' '||*k=='\t')++k;
        {char*ke=k+strlen(k)-1;while(ke>=k&&(*ke==' '||*ke=='\t'))*ke--='\0';}
        while(*v==' '||*v=='\t')++v;
        char*nl=strchr(v,'\n');if(nl)*nl='\0';nl=strchr(v,'\r');if(nl)*nl='\0';
        int iv=atoi(v);
        if(_stricmp(sec,"video")==0){
            if(_stricmp(k,"fps_limit")==0)g_fpsLimit=iv;
            else if(_stricmp(k,"vsync")==0)g_vsync=iv;
            else if(_stricmp(k,"mode")==0&&v[0])strncpy_s(g_videoMode,sizeof(g_videoMode),v,_TRUNCATE);
        }
        else if(_stricmp(sec,"graphics")==0){
            if(_stricmp(k,"quality")==0&&v[0])strncpy_s(g_quality,sizeof(g_quality),v,_TRUNCATE);
            else if(_stricmp(k,"shadows")==0&&v[0])strncpy_s(g_shadows,sizeof(g_shadows),v,_TRUNCATE);
            else if(_stricmp(k,"render_scale")==0&&v[0])strncpy_s(g_renderScale,sizeof(g_renderScale),v,_TRUNCATE);
        }
        else if(_stricmp(sec,"game")==0){
            if(_stricmp(k,"user_locale")==0&&v[0])strncpy_s(g_locale,sizeof(g_locale),v,_TRUNCATE);
            else if(_stricmp(k,"emulate_iap")==0)g_emulateIap=iv;
            else if(_stricmp(k,"persist_saves")==0)g_persistSaves=iv;
        }
        else if(_stricmp(sec,"gl")==0){
            if(_stricmp(k,"renderer")==0&&v[0])strncpy_s(g_renderer,sizeof(g_renderer),v,_TRUNCATE);
            else if(_stricmp(k,"msaa")==0)g_gl_msaa=iv;
        }
    }
    fclose(f);
}

void write_config(){
    auto_detect_obb();
    FILE*f=NULL; if(fopen_s(&f,g_configPath,"w")!=0||!f)return;
    fprintf(f,
        "; Sprout configuration\n\n"
        "[paths]\nso  = lib/libPVZ2.so\nobb = %s\n\n"
        "[log]\nverbose = 0\ntrace = 0\npc_sample = 0\ninput = 0\n\n"
        "[runtime]\nno_page_table = 0\nheap_quarantine = 0\n\n"
        "[gl]\ndebug_clear = 0\nno_viewport_fix = 0\nflat_fragment = 0\nstrict = 0\nrenderer = %s\nmsaa = %d\n\n"
        "[video]\nmode = %s\nfullscreen = 0\nvsync = %d\nfps_limit = %d\n\n"
        "[graphics]\nquality = %s\nshadows = %s\nrender_scale = %s\n\n"
        "[game]\nuser_locale = %s\nemulate_iap = %d\npersist_saves = %d\nnetwork = none\n",
        g_obbPath, g_renderer, g_gl_msaa, g_videoMode, g_vsync, g_fpsLimit, g_quality, g_shadows, g_renderScale, g_locale, g_emulateIap, g_persistSaves);
    fprintf(f,"\n[controls]\n");
    for(int i=0;i<NUM_BINDS;i++)
        fprintf(f,"%s = %d\ngp_%s = %d\n",
            g_binds[i].key,g_binds[i].kb,g_binds[i].key,g_binds[i].gp);
    fclose(f);
}

void read_controls(void){
    FILE*f=NULL; if(fopen_s(&f,g_configPath,"r")!=0||!f)return;
    char line[512],sec[64]="";
    while(fgets(line,sizeof(line),f)){
        char*t=line; while(*t==' '||*t=='\t')++t;
        if(t[0]=='['){char*cl=strchr(t+1,']');if(cl){*cl='\0';strncpy_s(sec,sizeof(sec),t+1,_TRUNCATE);}continue;}
        if(_stricmp(sec,"controls")!=0)continue;
        char*eq=strchr(t,'=');if(!eq)continue;
        *eq='\0';char*k=t,*v=eq+1;
        while(*k==' '||*k=='\t')++k;
        {char*ke=k+strlen(k)-1;while(ke>=k&&(*ke==' '||*ke=='\t'))*ke--='\0';}
        while(*v==' '||*v=='\t')++v;
        char*nl=strchr(v,'\n');if(nl)*nl='\0';nl=strchr(v,'\r');if(nl)*nl='\0';
        int iv=atoi(v);
        for(int i=0;i<NUM_BINDS;i++){
            if(g_binds[i].key && strcmp(k,g_binds[i].key)==0){g_binds[i].kb=iv;break;}
        }
        if(strncmp(k,"gp_",3)==0){
            for(int i=0;i<NUM_BINDS;i++){
                if(g_binds[i].key && strcmp(k+3,g_binds[i].key)==0){g_binds[i].gp=iv;break;}
            }
        }
    }
    fclose(f);
}

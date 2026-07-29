#ifndef SPROUT_CONFIG_H
#define SPROUT_CONFIG_H

#include <windows.h>
#include <sprout/actions.h>
#include "widgets.h"

/* ---------- Bindings ---------- */
typedef struct { const char *key; const wchar_t *label; int def_kb; int def_gp; int kb; int gp; } BindItem;
enum { NUM_BINDS = ACT_NUM_ACTIONS };
extern BindItem g_binds[NUM_BINDS];
extern int g_bind_tab;
extern int g_capturing;

/* ---------- Config globals ---------- */
extern char g_exeDir[1024], g_configPath[1024], g_gamePath[1024], g_obbPath[512];
extern int g_fpsLimit, g_showConsole, g_emulateIap, g_persistSaves, g_vsync;
extern char g_locale[64], g_videoMode[16], g_quality[16], g_shadows[16], g_renderScale[8];
extern char g_renderer[16];  /* "hardware" or "software" */
extern int  g_gl_msaa;       /* MSAA samples: 0, 2, 4, 8 */
extern int g_engineTab;      /* current tab: 0=Options, 1=Engine */

/* ---------- Combo data (shared with main window) ---------- */
extern const ComboItem kFpsLimit[5];
extern const int kNumFpsLimit;
extern const ComboItem kVideoMode[2];
extern const int kNumVideoMode;
extern const ComboItem kQuality[3];
extern const int kNumQuality;
extern const ComboItem kShadows[3];
extern const int kNumShadows;
extern const ComboItem kRenderScale[5];
extern const int kNumRenderScale;
extern const ComboItem kRenderer[2];
extern const int kNumRenderer;
extern const LangItem kLangs[12];
extern const int kNumLangs;

/* ---------- Layout constants (V2 — see main.cpp for WW2/WH2) ---------- */
#define LABEL_X 56
#define CTRL_X  200
#define CTRL_W  200
#define CTRL_H  28
#define TAB_H   32

/* ---------- Colors ---------- */
#define COL_BG     RGB(0x18,0x18,0x24)
#define COL_CARD   RGB(0x22,0x22,0x38)
#define COL_ACCENT RGB(0x4A,0xDE,0x80)
#define COL_ACCH   RGB(0x3B,0xC7,0x6C)
#define COL_ACCP   RGB(0x2D,0xA0,0x55)
#define COL_TEXT   RGB(0xF0,0xF0,0xF5)
#define COL_DIM    RGB(0x90,0x90,0xA8)
#define COL_MUTE   RGB(0x68,0x68,0x80)
#define COL_SECTION RGB(0x6A,0xDF,0x9A)
#define COL_BTNTXT RGB(0x0D,0x0D,0x16)

/* ---------- Controls dialog constants ---------- */
#define CD_W 480
#define CD_H 600
#define CD_ROW_H 32
#define CD_TOP 55

/* ---------- Functions ---------- */
void init_binds(void);
void get_paths(void);
void read_config(void);
void write_config(void);
void read_controls(void);
int lang_idx(const char *c);
const wchar_t* gp_name(int btn);
const wchar_t* key_name(int sdlk);
int vk_to_sdlk(int vk);

#endif

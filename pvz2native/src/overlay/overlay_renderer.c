#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include <glad/gl.h>
#include <sprout/host/host_window.h>
#include <sprout/overlay/overlay_renderer.h>

static int g_show_fps = 0, g_show_coords = 0;

void overlay_toggle_fps(void) { g_show_fps = !g_show_fps; }
void overlay_toggle_coords(void) { g_show_coords = !g_show_coords; }
int overlay_visible_fps(void) { return g_show_fps; }
int overlay_visible_coords(void) { return g_show_coords; }

/* --- FPS counter --- */
static Uint32 g_fps_last = 0;
static int g_fps_frames = 0;
static int g_fps_value = 0;

void overlay_tick_fps(void) {
    ++g_fps_frames;
    Uint32 now = SDL_GetTicks();
    if (now - g_fps_last >= 1000) {
        g_fps_value = g_fps_frames;
        g_fps_frames = 0;
        g_fps_last = now;
    }
}

/* 8x12 monochrome font bitmap */
#define FONT_W 8
#define FONT_H 12
#define FONT_CHARS 14
static const unsigned char kFont8x12[FONT_CHARS][FONT_H] = {
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C},
    {0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x7E},
    {0x3C,0x66,0x06,0x06,0x06,0x0C,0x18,0x30,0x30,0x60,0x60,0x7E},
    {0x3C,0x66,0x06,0x06,0x06,0x1C,0x06,0x06,0x06,0x66,0x66,0x3C},
    {0x0C,0x1C,0x3C,0x2C,0x4C,0x4C,0x8C,0xFE,0x0C,0x0C,0x0C,0x0C},
    {0x7E,0x60,0x60,0x60,0x7C,0x06,0x06,0x06,0x06,0x06,0x46,0x3C},
    {0x3C,0x66,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C},
    {0x7E,0x06,0x06,0x0C,0x0C,0x18,0x18,0x18,0x30,0x30,0x30,0x30},
    {0x3C,0x66,0x66,0x66,0x66,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C},
    {0x3C,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x06,0x66,0x3C},
    {0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x60,0x60,0x60},
    {0x7C,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0x60,0x60},
    {0x3C,0x66,0x60,0x60,0x30,0x18,0x0C,0x06,0x06,0x66,0x66,0x3C},
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00},
};
enum { FONT_F_IDX = 10, FONT_P_IDX = 11, FONT_S_IDX = 12, FONT_COLON_IDX = 13 };

static GLuint g_fps_tex = 0;
static int g_fps_tex_w = 0, g_fps_tex_h = 0;

static void fps_init_tex(void) {
    if (g_fps_tex) return;
    g_fps_tex_w = FONT_CHARS * (FONT_W + 2);
    g_fps_tex_h = FONT_H + 2;
    unsigned char *img = (unsigned char *)calloc(1, (size_t)g_fps_tex_w * g_fps_tex_h);
    for (int c = 0; c < FONT_CHARS; ++c) {
        int ox = c * (FONT_W + 2) + 1, oy = 1;
        for (int y = 0; y < FONT_H; ++y)
            for (int x = 0; x < FONT_W; ++x)
                if (kFont8x12[c][y] & (1u << (7 - x)))
                    img[(oy + y) * g_fps_tex_w + (ox + x)] = 255;
    }
    glGenTextures(1, &g_fps_tex);
    glBindTexture(GL_TEXTURE_2D, g_fps_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, g_fps_tex_w, g_fps_tex_h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, img);
    free(img);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void fps_draw_char(int x, int y, int idx, float scale) {
    float cw = (float)(FONT_W + 2), ch = (float)(FONT_H + 2);
    float tx = (float)(idx * (FONT_W + 2) + 1) / (float)g_fps_tex_w;
    float ty = 1.0f / (float)g_fps_tex_h;
    float tw = (float)FONT_W / (float)g_fps_tex_w, th = (float)FONT_H / (float)g_fps_tex_h;
    float w = FONT_W * scale, h = FONT_H * scale;
    glTexCoord2f(tx, ty); glVertex2i(x, y);
    glTexCoord2f(tx + tw, ty); glVertex2i(x + (int)w, y);
    glTexCoord2f(tx + tw, ty + th); glVertex2i(x + (int)w, y + (int)h);
    glTexCoord2f(tx, ty + th); glVertex2i(x, y + (int)h);
}

static void push_gl_state(void) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT | GL_CLIENT_PIXEL_STORE_BIT);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
}

static void pop_gl_state(void) {
    glPopClientAttrib();
    glPopAttrib();
}

static void setup_ortho(void) {
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    int rw = host_window_render_w(), rh = host_window_render_h();
    glOrtho(0.0, (double)rw, (double)rh, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
}

static void restore_ortho(void) {
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
}

void overlay_draw_fps(void) {
    if (!g_show_fps) return;
    fps_init_tex();
    int val = g_fps_value;
    if (val < 0) val = 0;
    if (val > 9999) val = 9999;

    push_gl_state();
    setup_ortho();

    float s = 1.2f;
    int margin = 8, ch_w = (int)(FONT_W * s), ch_h = (int)(FONT_H * s), spacing = 4;
    char buf[16];
    SDL_snprintf(buf, sizeof(buf), "%d", val);
    int ndigits = (int)strlen(buf);
    int panel_w = (6 * (ch_w + spacing) - spacing + ndigits * (ch_w + spacing)) + margin;
    int panel_h = ch_h + margin;

    glColor4f(0.0f, 0.0f, 0.0f, 0.55f);
    glRecti(0, 0, panel_w, panel_h);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fps_tex);
    glColor4f(0.3f, 1.0f, 0.5f, 1.0f);
    glBegin(GL_QUADS);
    int px = margin, py = margin;
    fps_draw_char(px, py, FONT_F_IDX, s); px += ch_w + spacing;
    fps_draw_char(px, py, FONT_P_IDX, s); px += ch_w + spacing;
    fps_draw_char(px, py, FONT_S_IDX, s); px += ch_w + spacing;
    fps_draw_char(px, py, FONT_COLON_IDX, s); px += ch_w + spacing;
    for (char *p = buf; *p; ++p) {
        if (*p >= '0' && *p <= '9') { fps_draw_char(px, py, *p - '0', s); px += ch_w + spacing; }
    }
    glEnd();

    restore_ortho();
    glBindTexture(GL_TEXTURE_2D, 0);
    pop_gl_state();
}

void overlay_draw_coords(int mouse_rx, int mouse_ry) {
    if (!g_show_coords) return;
    fps_init_tex();
    char buf[32];
    SDL_snprintf(buf, sizeof(buf), "%d,%d", mouse_rx, mouse_ry);

    push_gl_state();
    setup_ortho();

    float s = 1.2f;
    int ch_w = (int)(FONT_W * s), ch_h = (int)(FONT_H * s), spacing = 4;
    int rh = host_window_render_h();

    glColor4f(0.9f, 0.9f, 0.2f, 0.55f);
    glRecti(0, rh - 24, 140, rh);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_fps_tex);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    int px = 6, py = rh - 20;
    glBegin(GL_QUADS);
    for (char *p = buf; *p; ++p) {
        int idx = -1;
        if (*p >= '0' && *p <= '9') idx = *p - '0';
        else if (*p == ',') idx = FONT_COLON_IDX;
        if (idx >= 0) { fps_draw_char(px, py, idx, s); px += ch_w + spacing; }
    }
    glEnd();

    restore_ortho();
    glBindTexture(GL_TEXTURE_2D, 0);
    pop_gl_state();
}

void overlay_draw_gamepad_cursor(int cx, int cy) {
    push_gl_state();
    setup_ortho();

    glDisable(GL_TEXTURE_2D);
    glColor4f(0.29f, 0.87f, 0.50f, 0.30f);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2i(cx, cy);
    for (int i = 0; i <= 24; ++i) { float a = 6.2831855f * i / 24; glVertex2f(cx + cosf(a) * 14.0f, cy + sinf(a) * 14.0f); }
    glEnd();
    glColor4f(0.29f, 0.87f, 0.50f, 0.85f);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2i(cx, cy);
    for (int i = 0; i <= 24; ++i) { float a = 6.2831855f * i / 24; glVertex2f(cx + cosf(a) * 4.0f, cy + sinf(a) * 4.0f); }
    glEnd();
    glColor4f(1.0f, 1.0f, 0.55f, 0.55f);
    glBegin(GL_LINES);
    glVertex2i(cx - 12, cy); glVertex2i(cx - 5, cy);
    glVertex2i(cx + 5,  cy); glVertex2i(cx + 12, cy);
    glVertex2i(cx, cy - 12); glVertex2i(cx, cy - 5);
    glVertex2i(cx, cy + 5);  glVertex2i(cx, cy + 12);
    glEnd();

    restore_ortho();
    pop_gl_state();
}

void overlay_init(void) {
    fps_init_tex();
    overlay_tick_fps();
}

void overlay_shutdown(void) {
    if (g_fps_tex) { glDeleteTextures(1, &g_fps_tex); g_fps_tex = 0; }
}

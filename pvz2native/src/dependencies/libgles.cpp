/* libGLESv2.so / libGLESv1_CM.so -- the OpenGL ES entry points.
 *
 * PvZ2 uses BOTH: fixed-function GLES1 calls (glMatrixMode, glLoadMatrixf,
 * glShadeModel, glTexCoordPointer) alongside the GLES2 shader pipeline, which
 * is why the host context is a desktop GL compatibility profile rather than a
 * pure ES one. The bodies forward to gles_compat.c's gl_* wrappers, which is
 * where any real translation work lives.
 *
 * Moved verbatim out of the old monolithic dispatch chain; only the accessors
 * were renamed onto GuestCall.
 */

#include <sprout/dependencies/dependency.h>
#include <sprout/config.h>

/* gles_compat.c is C; its symbols have C linkage. */
extern "C" {
#include <sprout/gfx/gles_compat.h>
}

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sprout {
namespace {

/* Set once at startup from the SDL window size -- see set_drawable_size. */
std::atomic<std::uint32_t> g_drawable_w{0};
std::atomic<std::uint32_t> g_drawable_h{0};

/* Which framebuffer the guest currently has bound. The engine renders its scene
 * into an offscreen FBO at its fixed resolution, then binds framebuffer 0 -- the
 * window -- to composite. Tracking this lets gl_glViewport force the composite
 * to fill the whole window: without it the composite keeps the engine's
 * render-size viewport and the picture sits in one corner of a larger or
 * maximised window. Only the composite draws to framebuffer 0. */
std::atomic<GLuint> g_bound_fbo{0};

/* Drains the host GL error queue after an operation the guest never checks
 * itself. Budgeted, so a per-frame error cannot flood the log. */
void report_gl_error(GuestCall &c, const char *what) {
    static std::atomic<std::uint32_t> budget{24};
    GLenum err = gl_get_error();
    if (err == GL_NO_ERROR) return;
    if (budget.load(std::memory_order_relaxed) == 0) return;
    budget.fetch_sub(1, std::memory_order_relaxed);
    c.log("[gl] %s -> GL error 0x%04x", what, (unsigned)err);
}

/* Same, but says whether there WAS an error, so a draw can follow up with the
 * full state dump. Kept separate because it must not consume the error queue
 * silently once the report budget runs out. */
bool gl_peek_error_for_draw(GuestCall &c, const char *what) {
    GLenum err = gl_get_error();
    if (err == GL_NO_ERROR) return false;
    static std::atomic<std::uint32_t> budget{24};
    if (budget.load(std::memory_order_relaxed) > 0) {
        budget.fetch_sub(1, std::memory_order_relaxed);
        c.log("[gl] %s -> GL error 0x%04x", what, (unsigned)err);
    }
    return true;
}

void gl_glActiveTexture(GuestCall &c) {
            gl_active_texture(c.arg(0));
}

/* [gl] debug_clear=1 forces a loud clear colour. The engine clears to opaque
 * black, which is indistinguishable from "the frame never reached the window"
 * -- and those two have completely different causes. If the window turns this
 * colour, presentation works and the problem is that nothing is drawn INTO the
 * frame; if it stays black, the frame is not reaching the screen at all. */
bool debug_clear_enabled() { return pvz2_config()->gl_debug_clear != 0; }

/* GL_INVALID_OPERATION from a draw call names no cause by itself, and the
 * possible ones are far apart: an unlinked or unvalidatable program, two
 * samplers of different types on one texture unit, an incomplete texture, an
 * incomplete framebuffer. Asking the driver at the moment it rejects the draw
 * is the only way to tell them apart, so dump the pieces of state a draw
 * depends on. Budgeted -- the first few are the whole story. */
void dump_draw_state(GuestCall &c) {
    static std::atomic<std::uint32_t> budget{3};
    if (budget.load(std::memory_order_relaxed) == 0) return;
    budget.fetch_sub(1, std::memory_order_relaxed);

    GLint program = 0, fbo = 0, array_buf = 0, active_tex = 0, tex2d = 0, vao = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d);
    /* The one that matters most here: with a non-zero VAO bound, client-side
     * vertex arrays (buffer=0 below) are illegal even in a compatibility
     * profile, and the driver rejects the draw with exactly INVALID_OPERATION.
     * GLES2 has no VAOs, so the guest never binds one -- but glad, SDL or our
     * own setup might have left one bound. */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);

    GLint linked = 0, validated = 0;
    char log[512] = {0};
    if (program != 0) {
        glGetProgramiv((GLuint)program, GL_LINK_STATUS, &linked);
        glValidateProgram((GLuint)program);
        glGetProgramiv((GLuint)program, GL_VALIDATE_STATUS, &validated);
        GLsizei n = 0;
        glGetProgramInfoLog((GLuint)program, (GLsizei)sizeof(log) - 1, &n, log);
    }

    const GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    c.log("[gl] draw-state: program=%d linked=%d validated=%d fbo=%d fb_status=0x%04x "
          "array_buffer=%d active_texture=0x%04x texture_2d=%d vao=%d",
          program, linked, validated, fbo, (unsigned)fb_status, array_buf,
          (unsigned)active_tex, tex2d, vao);
    if (log[0] != '\0') c.log("[gl] program info log: %s", log);

    /* Which attribute arrays are live, and whether each is client-side (buffer
     * 0) -- client arrays are legal in a compatibility profile but not in core,
     * and a stale enabled attribute with a dangling pointer is a classic
     * INVALID_OPERATION. */
    for (GLuint i = 0; i < 8; ++i) {
        GLint enabled = 0, buf = 0, size = 0;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        void *ptr = nullptr;
        glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
        c.log("[gl]   attrib %u: enabled buffer=%d size=%d ptr=%p", i, buf, size, ptr);
    }
}

/* What the composite-to-screen draw is actually sampling.
 *
 * The window is pure black -- max luminance 0 over 55k samples -- while the
 * scene FBO demonstrably has content, and a forced clear colour DOES show
 * through. So the composite covers the screen and samples black. A texture
 * that is INCOMPLETE samples as (0,0,0,1) with no error at all, and the usual
 * way to get there is a min filter that wants mipmaps on a texture that has
 * none -- exactly the shape of an FBO colour attachment. This dumps the bound
 * texture's filters and level-0 size so that is either confirmed or dropped. */
/* The shader source the driver was actually given.
 *
 * Everything else about the composite draw has been eliminated by measurement
 * -- state, geometry, texture, matrix, order -- so what remains is what the
 * program computes. PvZ2 ships GLSL ES, which desktop GL rejects (lowp/mediump,
 * no #version), and the harness rewrites it before compiling. If that rewrite
 * drops or mangles the sampling, the draw is valid, error-free and black --
 * exactly what is observed. Printing the post-rewrite source is the only way
 * to see what the driver really compiled. */
void dump_program_source(GuestCall &c, GLuint program) {
    static std::mutex lock;
    static std::set<GLuint> seen;
    {
        std::lock_guard<std::mutex> lk(lock);
        if (seen.size() >= 3 || !seen.insert(program).second) return;
    }
    GLuint shaders[8] = {0};
    GLsizei count = 0;
    glGetAttachedShaders(program, 8, &count, shaders);
    c.log("[gl] ===== program %u has %d attached shader(s) =====", program, (int)count);
    for (GLsizei i = 0; i < count; ++i) {
        GLint type = 0, compiled = 0, len = 0;
        glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &len);
        c.log("[gl] --- shader %u type=%s compiled=%d ---", shaders[i],
              type == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT", compiled);
        if (len <= 0 || len > 16384) continue;
        std::string src((size_t)len, '\0');
        GLsizei got = 0;
        glGetShaderSource(shaders[i], len, &got, src.data());
        /* One log line per source line: c.log has a fixed-size buffer. */
        size_t start = 0;
        while (start < src.size()) {
            size_t nl = src.find('\n', start);
            if (nl == std::string::npos) nl = src.size();
            std::string line = src.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) c.log("[gl] | %s", line.c_str());
            start = nl + 1;
        }
    }
}

void probe_composite_texture(GuestCall &c, std::uint32_t draw_index) {
    GLint prog_for_src = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog_for_src);
    if (prog_for_src != 0) dump_program_source(c, (GLuint)prog_for_src);

    GLint tex = 0, unit = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &unit);
    if (tex == 0) {
        c.log("[gl] composite draw has NO texture bound on unit 0x%04x -- it can only sample black",
              (unsigned)unit);
        return;
    }

    GLint min_f = 0, mag_f = 0, wrap_s = 0, wrap_t = 0, base = 0, maxlvl = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_f);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_f);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap_s);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrap_t);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxlvl);

    GLint w0 = 0, h0 = 0, fmt = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w0);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h0);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);

    /* 0x2600 NEAREST, 0x2601 LINEAR; anything else is a mipmapping filter. */
    const bool needs_mips = (min_f != 0x2600 && min_f != 0x2601);
    c.log("[gl] draw #%u samples texture %d on unit 0x%04x: level0=%dx%d fmt=0x%04x "
          "min=0x%04x mag=0x%04x wrap=0x%04x/0x%04x base=%d max=%d%s",
          draw_index, tex, (unsigned)unit, w0, h0, (unsigned)fmt, (unsigned)min_f, (unsigned)mag_f,
          (unsigned)wrap_s, (unsigned)wrap_t, base, maxlvl,
          needs_mips ? "  <-- MIN FILTER WANTS MIPMAPS: incomplete texture samples BLACK" : "");
}

/* The composite quad's actual geometry.
 *
 * Everything downstream of this draw is verified good -- no GL errors, correct
 * and complete source texture, sane matrices, correct viewport -- and the
 * window is still pure black while a forced clear colour shows through. A
 * clear that shows through means the quad is not covering the screen, so the
 * remaining suspect is where its vertices actually are. Client-side arrays, so
 * the data is readable straight from the pointers the driver holds. */
void probe_composite_geometry(GuestCall &c, GLint first, GLsizei count, std::uint32_t draw_index) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    c.log("[gl] draw #%u geometry: program=%d first=%d count=%d", draw_index, program, first, count);
    if (count <= 0) {
        c.log("[gl]   count is %d -- the draw covers nothing at all", count);
        return;
    }

    /* Which program is doing the composite matters as much as the geometry: if
     * it declares no sampler at all it cannot be showing the scene texture,
     * whatever is bound. Also report the blend state -- the right pixels drawn
     * with the wrong blend factors are just as invisible. */
    GLint uniforms = 0;
    glGetProgramiv((GLuint)program, GL_ACTIVE_UNIFORMS, &uniforms);
    for (GLint i = 0; i < uniforms; ++i) {
        char nm[128] = {0};
        GLsizei len = 0;
        GLint usize = 0;
        GLenum utype = 0;
        glGetActiveUniform((GLuint)program, (GLuint)i, (GLsizei)sizeof(nm) - 1, &len, &usize, &utype, nm);
        GLint value = -1;
        const GLint uloc = glGetUniformLocation((GLuint)program, nm);
        if (utype == 0x8B5E /* sampler2D */ && uloc >= 0) {
            glGetUniformiv((GLuint)program, uloc, &value);
        }
        c.log("[gl]   uniform %s loc=%d type=0x%04x%s", nm, uloc, (unsigned)utype,
              utype == 0x8B5E ? (value >= 0 ? "" : " (sampler, unread)") : "");
        if (utype == 0x8B5E && value >= 0) {
            c.log("[gl]     sampler bound to texture unit %d", value);
        }
        /* The matrix AS THE PROGRAM HOLDS IT AT DRAW TIME. The earlier dump
         * printed what was passed to glUniformMatrix4fv, which is a different
         * question: two engine sites upload this uniform, one of them
         * (sub_ABBCEC) to whatever program happens to be bound, so the value in
         * effect when the composite draws need not be the one first uploaded
         * for that program. A wrong matrix here puts the quad off-screen, and
         * then no fragment shader -- not even a constant one -- can produce
         * pixels. Printed row by row; a correct one maps 0..1365 / 0..768 onto
         * [-1,1], i.e. ~0.00147 and ~0.0026 on the diagonal with -1 translation. */
        if (utype == 0x8B5C /* mat4 */ && uloc >= 0) {
            GLfloat m[16] = {0};
            glGetUniformfv((GLuint)program, uloc, m);
            for (int row = 0; row < 4; ++row) {
                c.log("[gl]     [% .5f % .5f % .5f % .5f]", m[row], m[row + 4], m[row + 8], m[row + 12]);
            }
        }
    }

    /* Where the driver actually put each vertex input.
     *
     * GLES2 programs get their attribute indices either from
     * glBindAttribLocation before linking or from glGetAttribLocation after,
     * and the engine feeds glVertexAttribPointer(0/1/2, ...). If this desktop
     * driver numbered position/color/texcoord0 differently from the Android
     * one, the pointers land on the wrong inputs -- the exact same class of
     * bug as the mat4 uniform location fixed above, and one that produces a
     * valid, error-free, invisible draw. */
    GLint actives = 0;
    glGetProgramiv((GLuint)program, GL_ACTIVE_ATTRIBUTES, &actives);
    for (GLint i = 0; i < actives; ++i) {
        char nm[128] = {0};
        GLsizei len = 0;
        GLint asize = 0;
        GLenum atype = 0;
        glGetActiveAttrib((GLuint)program, (GLuint)i, (GLsizei)sizeof(nm) - 1, &len, &asize, &atype, nm);
        const GLint aloc = glGetAttribLocation((GLuint)program, nm);
        c.log("[gl]   attribute \"%s\" -> index %d (type=0x%04x)", nm, aloc, (unsigned)atype);
    }

    /* A constant-magenta fragment shader fills FBO 1 and still leaves the
     * window black, so the composite produces no fragments at all -- yet
     * glClear on that same framebuffer does write. Two states discard
     * primitives while leaving clears alone: GL_RASTERIZER_DISCARD, and a
     * draw buffer of GL_NONE. Neither exists in GLES2, so the guest cannot
     * have set them deliberately; if one is on, it came from the host side. */
    /* 0x8C89 is GL_RASTERIZER_DISCARD; the glad header here only defines the
     * _NV spelling, so use the value. */
    GLint discard = 0, draw_buf = 0;
    discard = glIsEnabled(0x8C89) ? 1 : 0;
    glGetIntegerv(GL_DRAW_BUFFER, &draw_buf);
    c.log("[gl]   rasterizer_discard=%d draw_buffer=0x%04x%s%s", discard, (unsigned)draw_buf,
          discard ? "  <-- RASTERIZER DISCARD IS ON: every draw is thrown away" : "",
          draw_buf == GL_NONE ? "  <-- DRAW BUFFER IS NONE: nothing can be written" : "");

    GLint blend = 0, src_rgb = 0, dst_rgb = 0;
    glGetIntegerv(GL_BLEND, &blend);
    glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
    c.log("[gl]   blend=%d src=0x%04x dst=0x%04x", blend, (unsigned)src_rgb, (unsigned)dst_rgb);

    /* The three ways to throw a whole draw away with no error and no visible
     * difference in any state checked so far: clipped out by the scissor box,
     * rejected by the depth test, or masked out of the colour buffer. Draw #0
     * and draw #600 agreed on everything else, so the difference has to be
     * here. */
    GLint scissor = 0, sbox[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_SCISSOR_TEST, &scissor);
    glGetIntegerv(GL_SCISSOR_BOX, sbox);
    GLint depth = 0, dfunc = 0, dmask = 0;
    glGetIntegerv(GL_DEPTH_TEST, &depth);
    glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
    glGetIntegerv(GL_DEPTH_WRITEMASK, &dmask);
    GLboolean cmask[4] = {0, 0, 0, 0};
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    GLint stencil = 0, cull = 0;
    glGetIntegerv(GL_STENCIL_TEST, &stencil);
    glGetIntegerv(GL_CULL_FACE, &cull);
    GLfloat drange[2] = {0.0f, 0.0f};
    glGetFloatv(GL_DEPTH_RANGE, drange);

    c.log("[gl]   scissor=%d box=%d,%d %dx%d | depth_test=%d func=0x%04x mask=%d range=%.3f..%.3f "
          "| colormask=%d%d%d%d | stencil=%d cull=%d",
          scissor, sbox[0], sbox[1], sbox[2], sbox[3], depth, (unsigned)dfunc, dmask,
          drange[0], drange[1], cmask[0], cmask[1], cmask[2], cmask[3], stencil, cull);
    if (scissor && (sbox[2] <= 0 || sbox[3] <= 0)) {
        c.log("[gl]   <-- SCISSOR BOX IS EMPTY: the whole draw is clipped away");
    }
    if (!cmask[0] && !cmask[1] && !cmask[2]) {
        c.log("[gl]   <-- COLOUR WRITES ARE MASKED OFF: the draw cannot change the screen");
    }

    for (GLuint a = 0; a < 8; ++a) {
        GLint enabled = 0;
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;

        GLint size = 0, type = 0, stride = 0, buf = 0, normalized = 0;
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buf);
        glGetVertexAttribiv(a, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
        void *base = nullptr;
        glGetVertexAttribPointerv(a, GL_VERTEX_ATTRIB_ARRAY_POINTER, &base);

        c.log("[gl]   attrib %u: size=%d type=0x%04x stride=%d buffer=%d normalized=%d ptr=%p",
              a, size, (unsigned)type, stride, buf, normalized, base);
        /* 0x1406 is GL_FLOAT. Anything else, or a VBO, and the raw read below
         * would be meaningless -- say so instead of printing noise. */
        /* 0x1401 is GL_UNSIGNED_BYTE: the vertex colour. Worth reading in full
         * -- a full-screen quad with a correct texture and a correct matrix
         * still renders black if its per-vertex colour is black, and that is
         * indistinguishable from "nothing was drawn" on a black clear. */
        if (type == 0x1401 && buf == 0 && base != nullptr) {
            const int eff = stride != 0 ? stride : size;
            const auto *bytes = static_cast<const unsigned char *>(base);
            for (GLint v = first; v < first + count && v < first + 6; ++v) {
                const unsigned char *p = bytes + (size_t)v * eff;
                c.log("[gl]     v%d: %u %u %u %u (bytes%s)", v, p[0], p[1], p[2],
                      size > 3 ? p[3] : 255u,
                      (p[0] | p[1] | p[2]) == 0 ? " -- BLACK" : "");
            }
            continue;
        }
        if (type != 0x1406 || buf != 0 || base == nullptr) {
            c.log("[gl]     (not a client-side float or ubyte array -- values not dumped)");
            continue;
        }
        const int eff_stride = stride != 0 ? stride : (int)(size * sizeof(GLfloat));
        const auto *bytes = static_cast<const unsigned char *>(base);
        for (GLint v = first; v < first + count && v < first + 6; ++v) {
            const auto *f = reinterpret_cast<const GLfloat *>(bytes + (size_t)v * eff_stride);
            char buf2[128];
            int n = std::snprintf(buf2, sizeof(buf2), "[gl]     v%d:", v);
            for (GLint k = 0; k < size && k < 4; ++k) {
                n += std::snprintf(buf2 + n, sizeof(buf2) - (size_t)n, " % .3f", f[k]);
            }
            c.log("%s", buf2);
        }
    }
}

/* Composite draws seen so far. Shared by the pre-draw probes and the post-draw
 * readback so both describe the same draw. */
std::atomic<std::uint32_t> g_composite_draws{0};

/* Orders the frame.
 *
 * The scene draws the PopCap splash correctly -- black background, one 404x375
 * logo quad in the middle, which is exactly what should be on screen -- and
 * the composite still puts nothing on the window. Every piece of GL state at
 * that draw matches a draw that DID work. What has not been checked is the
 * ORDER: if the composite runs before the scene draws, or the FBO is cleared
 * between the scene and the composite, it would sample an empty target while
 * every state check still passes. Active for a few frames deep into the broken
 * steady state. */
bool trace_frame_order() {
    const std::uint32_t n = g_composite_draws.load(std::memory_order_relaxed);
    return n >= 600 && n < 603;
}

void log_gl_step(GuestCall &c, const char *what, int a, int b) {
    if (!trace_frame_order()) return;
    GLint fbo = 0, program = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    c.log("[gl-order] fbo=%d program=%d  %s(%d, %d)", fbo, program, what, a, b);
}

static bool gl_diagnostics_enabled() {
    static const bool enabled = pvz2_config()->gl_diagnostics != 0;
    return enabled;
}

void gl_glDrawArrays(GuestCall &c) {
    /* Diagnostics = off: skip probes entirely for maximum throughput. */
    if (gl_diagnostics_enabled()) {
        /* The composite pass is the draw that happens with the window bound.
         *
         * Draw #0 puts real pixels on the window (9/9 non-black); every later one
         * produces pure black, with the source FBO still holding content. So the
         * probes have to run on a WORKING draw and a BROKEN one and be compared --
         * running them once, on draw #0, described only the frame that works. */
        std::uint32_t composite_index = 0;
        {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            if (fbo == 0) {
                composite_index = g_composite_draws.fetch_add(1, std::memory_order_relaxed);
                if (composite_index == 0 || composite_index == 600 || composite_index == 1200) {
                    probe_composite_texture(c, composite_index);
                    probe_composite_geometry(c, (GLint)c.arg(1), (GLsizei)c.arg(2), composite_index);
                }
            } else {
                static std::atomic<std::uint32_t> scene_draws{0};
                const std::uint32_t n = scene_draws.fetch_add(1, std::memory_order_relaxed);
                const bool early = n < 3;
                const bool late = n >= 1800 && n < 1803;
                if (early || late) {
                    c.log("[gl] --- scene draw #%u into fbo %d ---", n, fbo);
                    probe_composite_texture(c, n);
                    probe_composite_geometry(c, (GLint)c.arg(1), (GLsizei)c.arg(2), n);
                }
            }
        }
        {
            GLenum stale = gl_get_error();
            if (stale != GL_NO_ERROR) {
                static std::atomic<std::uint32_t> budget{6};
                if (budget.load(std::memory_order_relaxed) > 0) {
                    budget.fetch_sub(1, std::memory_order_relaxed);
                    c.log("[gl] error 0x%04x was already queued BEFORE glDrawArrays -- it came "
                          "from an earlier call, not this draw", (unsigned)stale);
                }
            }
        }

        log_gl_step(c, "glDrawArrays first,count", (int)c.arg(1), (int)c.arg(2));
        gl_draw_arrays(c.arg(0), c.arg(1), c.arg(2));
        if (gl_peek_error_for_draw(c, "glDrawArrays")) dump_draw_state(c);

        {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            if (fbo == 0) {
                const std::uint32_t n = composite_index;
                if (n < 6000 && (n % 600) == 0) {
                    GLint vp[4] = {0, 0, 0, 0};
                    glGetIntegerv(GL_VIEWPORT, vp);
                    unsigned nonblack = 0, peak = 0;
                    for (int gy = 1; gy <= 3; ++gy) {
                        for (int gx = 1; gx <= 3; ++gx) {
                            GLubyte px[4] = {0, 0, 0, 0};
                            glReadPixels(vp[0] + vp[2] * gx / 4, vp[1] + vp[3] * gy / 4, 1, 1,
                                         GL_RGBA, GL_UNSIGNED_BYTE, px);
                            const unsigned lum = (unsigned)px[0] + px[1] + px[2];
                            if (lum > 0) ++nonblack;
                            if (lum > peak) peak = lum;
                        }
                    }
                    c.log("[gl] after composite draw #%u: window viewport %dx%d -- %u/9 pixels "
                          "non-black (brightest sum=%u)", n, vp[2], vp[3], nonblack, peak);
                }
            }
        }
    } else {
        /* Production path: just forward the draw without any diagnostics. */
        gl_draw_arrays(c.arg(0), c.arg(1), c.arg(2));
    }
}

void gl_glDrawElements(GuestCall &c) {
    gl_draw_elements(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
    report_gl_error(c, "glDrawElements");
}

void gl_glViewport(GuestCall &c) {
    std::uint32_t x = c.arg(0), y = c.arg(1), w = c.arg(2), h = c.arg(3);

    /* Cache the viewport fix config flag once. */
    static const bool no_viewport_fix = pvz2_config()->gl_no_viewport_fix != 0;
    const std::uint32_t dw = g_drawable_w.load(std::memory_order_relaxed);
    const std::uint32_t dh = g_drawable_h.load(std::memory_order_relaxed);
    const bool have_drawable = !no_viewport_fix && dw != 0 && dh != 0;

    /* Any viewport on framebuffer 0 is a composite to the window, and it must
     * fill the window whatever size it is now -- otherwise a maximised or
     * dragged window shows the fixed-resolution picture in one corner. The
     * engine sizes this viewport from its own render resolution (or leaves it
     * zero-width, the original bug below), neither of which tracks the real
     * window, so override it with the current drawable size. Scene passes into
     * an offscreen FBO keep their own viewport untouched. */
    if (have_drawable && g_bound_fbo.load(std::memory_order_relaxed) == 0) {
        if (x != 0 || y != 0 || w != dw || h != dh) {
            static std::atomic<std::uint32_t> budget{3};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.log("[gl] window viewport(%u,%u,%u,%u) -> filling drawable %ux%u", x, y, w, h, dw, dh);
            }
        }
        x = 0;
        y = 0;
        w = dw;
        h = dh;
    } else if (w == 0 || h == 0) {
        /* A zero-width viewport on an FBO would make the pass a silent no-op.
         * Unlikely off framebuffer 0, but substitute defensively. */
        static std::atomic<std::uint32_t> budget{3};
        if (have_drawable && budget.load(std::memory_order_relaxed) > 0) {
            budget.fetch_sub(1, std::memory_order_relaxed);
            c.log("[gl] degenerate viewport(%u,%u,%u,%u) lr=0x%08x -- substituting the drawable size",
                  x, y, w, h, c.lr());
        }
        if (have_drawable) { x = 0; y = 0; w = dw; h = dh; }
    }
    gl_viewport(x, y, w, h);
}

void gl_glAlphaFunc(GuestCall &c) {
            gl_alpha_func(c.arg(0), c.argf(1));
}

void gl_glAttachShader(GuestCall &c) {
            gl_attach_shader(c.arg(0), c.arg(1));
}

void gl_glBindAttribLocation(GuestCall &c) {
            gl_bind_attrib_location(c.arg(0), c.arg(1), (const GLchar *)c.ptr(c.arg(2)));
}

/* Reports what an offscreen framebuffer actually contains, at the moment the
 * engine stops rendering into it.
 *
 * The screen is black even though the GL error queue is clean, the viewport is
 * correct and SDL_GL_SwapWindow runs every frame -- and forcing a loud clear
 * colour DOES show through, so presentation works and the composite is
 * covering the window with something black. The question left is whether the
 * scene FBO it samples has any content. glReadPixels on the FBO answers that
 * directly instead of by inference: it also names the colour attachment, so a
 * composite sampling the wrong texture is visible too. */
void probe_framebuffer_contents(GuestCall &c, GLuint fbo) {
    /* Sampled along a timeline rather than only at the start: the first probe
     * showed real content (the splash logo) and every later one showed pure
     * black, so WHEN it goes dark is the whole question. glReadPixels stalls
     * the pipeline, hence the stride. */
    static std::atomic<std::uint32_t> seen{0};
    const std::uint32_t n = seen.fetch_add(1, std::memory_order_relaxed);
    if (n >= 40 * 200 || (n % 200) != 0) return;

    GLint att_type = 0, att_name = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &att_type);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att_name);

    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);

    /* Nine samples across the middle of the target: a single pixel can legally
     * be black in a perfectly good frame, a whole grid cannot. */
    /* Alpha is reported separately and deliberately: every other explanation
     * for the composite going black has been eliminated by measurement, and a
     * scene rendered with colour but zero alpha would be invisible to any
     * composite shader that discards on, or multiplies by, alpha -- while
     * still reading as "has content" if only RGB is summed, which is what the
     * earlier version of this probe did. */
    unsigned nonblack = 0, peak = 0, alpha_min = 255, alpha_max = 0;
    for (int gy = 1; gy <= 3; ++gy) {
        for (int gx = 1; gx <= 3; ++gx) {
            GLubyte px[4] = {0, 0, 0, 0};
            const GLint x = vp[0] + vp[2] * gx / 4;
            const GLint y = vp[1] + vp[3] * gy / 4;
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            const unsigned lum = (unsigned)px[0] + px[1] + px[2];
            if (lum > 0) ++nonblack;
            if (lum > peak) peak = lum;
            if (px[3] < alpha_min) alpha_min = px[3];
            if (px[3] > alpha_max) alpha_max = px[3];
        }
    }
    c.log("[gl] fbo %u contents (unbind #%u): attachment type=0x%04x name=%d viewport=%d,%d %dx%d -- "
          "%u/9 sampled pixels non-black (brightest sum=%u) alpha=%u..%u%s",
          fbo, n, (unsigned)att_type, att_name, vp[0], vp[1], vp[2], vp[3], nonblack, peak,
          alpha_min, alpha_max,
          alpha_max == 0 ? "  <-- ALPHA IS ZERO EVERYWHERE" : "");
}

void gl_glBindFramebuffer(GuestCall &c) {
    log_gl_step(c, "glBindFramebuffer ->", (int)c.arg(1), 0);
    /* Probe on the way OUT of an offscreen target: that is the only moment the
     * scene is finished and still readable. */
    if (c.arg(1) == 0) {
        GLint current = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current);
        if (current != 0) probe_framebuffer_contents(c, (GLuint)current);
    }
    g_bound_fbo.store((GLuint)c.arg(1), std::memory_order_relaxed);
    gl_bind_framebuffer(c.arg(0), c.arg(1));
}

void gl_glBindTexture(GuestCall &c) {
            gl_bind_texture(c.arg(0), c.arg(1));
}

void gl_glBlendFunc(GuestCall &c) {
            gl_blend_func(c.arg(0), c.arg(1));
}

void gl_glCheckFramebufferStatus(GuestCall &c) {
            c.regs[0] = gl_check_framebuffer_status(c.arg(0));
}

void gl_glClear(GuestCall &c) {
    log_gl_step(c, "glClear mask", (int)c.arg(0), 0);
            gl_clear(c.arg(0));
}

void gl_glClearColor(GuestCall &c) {
    if (debug_clear_enabled()) {
        gl_clear_color(0.2f, 0.0f, 0.6f, 1.0f); /* purple: nothing in the game is this colour */
        return;
    }
    gl_clear_color(c.argf(0), c.argf(1), c.argf(2), c.argf(3));
}

void gl_glClearDepthf(GuestCall &c) {
            gl_clear_depth_f(c.argf(0));
}

void gl_glClientActiveTexture(GuestCall &c) {
            gl_client_active_texture(c.arg(0));
}

void gl_glColorMask(GuestCall &c) {
            gl_color_mask((GLboolean)c.arg(0), (GLboolean)c.arg(1), (GLboolean)c.arg(2), (GLboolean)c.arg(3));
}

void gl_glColorPointer(GuestCall &c) {
            gl_color_pointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

void gl_glCompileShader(GuestCall &c) {
            GLuint shader = c.arg(0);
            gl_compile_shader(shader);
            /* The engine's shaders are GLSL ES 1.00 and the host context is
             * desktop GL, which rejects them unless the driver accepts ES
             * syntax. The engine never reads the info log, so a rejected
             * shader would silently produce a black screen with draw calls
             * still being issued -- exactly the symptom. Report it here. */
            {
                GLint status = 0;
                gl_get_shader_i_v(shader, GL_COMPILE_STATUS, &status);
                if (status != GL_TRUE) {
                    GLchar log[1024] = {0};
                    GLsizei len = 0;
                    gl_get_shader_info_log(shader, (GLsizei)sizeof(log) - 1, &len, log);
                    std::lock_guard<std::mutex> lg(c.rt->log_lock);
                    std::printf("pvz2: [gl] SHADER %u FAILED TO COMPILE: %s\n", shader, log);
                }
            }
}

void gl_glLinkProgram(GuestCall &c) {
            GLuint program = c.arg(0);
            gl_link_program(program);
            {
                GLint status = 0;
                gl_get_program_i_v(program, GL_LINK_STATUS, &status);
                if (status != GL_TRUE) {
                    GLchar log[1024] = {0};
                    GLsizei len = 0;
                    gl_get_program_info_log(program, (GLsizei)sizeof(log) - 1, &len, log);
                    std::lock_guard<std::mutex> lg(c.rt->log_lock);
                    std::printf("pvz2: [gl] PROGRAM %u FAILED TO LINK: %s\n", program, log);
                }
            }
}

/* --- ETC1 software decode -------------------------------------------------
 *
 * PvZ2 uploads every non-RGBA texture as ETC1 (GL_ETC1_RGB8_OES, 0x8D64) -- the
 * universal Android compressed format. Desktop GL drivers frequently do NOT
 * expose ETC1: they expose ETC2 (its superset) or nothing, so the pass-through
 * upload fails with GL_INVALID_ENUM and the texture is black. Confirmed on an
 * AMD HD 5450 (lists ETC2 0x9274 but not ETC1) and reported the same on Intel
 * Bay Trail. The dev machine happened to accept ETC1, which is why it looked
 * fine there and nowhere else.
 *
 * Decoding on the CPU and uploading plain RGB8 removes the dependency on the
 * driver supporting ANY compressed format -- it works everywhere, including the
 * weak GPUs this is for. ETC1 is a subset of ETC2, so re-tagging as ETC2 would
 * fix the AMD but not a driver that lacks ETC2 too; software decode is the only
 * universally correct fix. The separate-alpha texture PvZ2 pairs with these is
 * already uploaded uncompressed (GL_ALPHA), so only the RGB block needs this.
 *
 * Reference: the ETC1 block format (Khronos GL_OES_compressed_ETC1_RGB8_texture),
 * matching Android's etc1.cpp. 4x4 pixels per 8-byte block, two 2x4/4x2 subblocks
 * each with a base colour and a 3-bit modifier-table codeword; per-pixel 2-bit
 * index picks the modifier added to all three channels. */
const int kEtc1Modifier[8][4] = {
    {2, 8, -2, -8},      {5, 17, -5, -17},    {9, 29, -9, -29},    {13, 42, -13, -42},
    {18, 60, -18, -60},  {24, 80, -24, -80},  {33, 106, -33, -106}, {47, 183, -47, -183},
};

inline int etc1_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline int etc1_ext4(int v) { return (v << 4) | v; }               /* 4 bits -> 8 */
inline int etc1_ext5(int v) { v &= 0x1f; return (v << 3) | (v >> 2); } /* 5 bits -> 8 */
inline int etc1_diff3(int v) { v &= 7; return v < 4 ? v : v - 8; }  /* 3-bit signed delta */

void etc1_decode_block(const std::uint8_t *b, std::uint8_t *out, int width, int height, int ox0,
                       int oy0) {
    const std::uint32_t hi = (std::uint32_t)b[0] << 24 | (std::uint32_t)b[1] << 16 |
                             (std::uint32_t)b[2] << 8 | b[3];
    const std::uint32_t lo = (std::uint32_t)b[4] << 24 | (std::uint32_t)b[5] << 16 |
                             (std::uint32_t)b[6] << 8 | b[7];
    const bool diff = (hi & 2) != 0;
    const bool flip = (hi & 1) != 0;

    int r1, g1, b1, r2, g2, b2;
    if (diff) {
        const int r = (hi >> 27) & 0x1f, g = (hi >> 19) & 0x1f, bl = (hi >> 11) & 0x1f;
        r1 = etc1_ext5(r);            g1 = etc1_ext5(g);            b1 = etc1_ext5(bl);
        r2 = etc1_ext5(r + etc1_diff3(hi >> 24));
        g2 = etc1_ext5(g + etc1_diff3(hi >> 16));
        b2 = etc1_ext5(bl + etc1_diff3(hi >> 8));
    } else {
        r1 = etc1_ext4((hi >> 28) & 0xf); r2 = etc1_ext4((hi >> 24) & 0xf);
        g1 = etc1_ext4((hi >> 20) & 0xf); g2 = etc1_ext4((hi >> 16) & 0xf);
        b1 = etc1_ext4((hi >> 12) & 0xf); b2 = etc1_ext4((hi >> 8) & 0xf);
    }
    const int *t1 = kEtc1Modifier[(hi >> 5) & 7];
    const int *t2 = kEtc1Modifier[(hi >> 2) & 7];

    for (int px = 0; px < 4; ++px) {
        for (int py = 0; py < 4; ++py) {
            const int k = py + px * 4; /* pixel bit index: column-major */
            const int idx = (int)(((lo >> k) & 1) | (((lo >> (k + 16)) & 1) << 1));
            const bool sub2 = flip ? (py >= 2) : (px >= 2);
            const int mod = sub2 ? t2[idx] : t1[idx];
            const int ox = ox0 + px, oy = oy0 + py;
            if (ox >= width || oy >= height) continue;
            std::uint8_t *p = out + ((std::size_t)oy * width + ox) * 3;
            p[0] = (std::uint8_t)etc1_clamp8((sub2 ? r2 : r1) + mod);
            p[1] = (std::uint8_t)etc1_clamp8((sub2 ? g2 : g1) + mod);
            p[2] = (std::uint8_t)etc1_clamp8((sub2 ? b2 : b1) + mod);
        }
    }
}

/* Decodes a full ETC1 image (width*height, row-major RGB8). Returns false when
 * the source is too small for the dimensions, so the caller can fall back to the
 * raw upload rather than read past the buffer. */
bool etc1_decode(const std::uint8_t *src, int imageSize, int width, int height,
                 std::vector<std::uint8_t> &out) {
    if (src == nullptr || width <= 0 || height <= 0) return false;
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if ((long long)bw * bh * 8 > imageSize) return false;
    out.assign((std::size_t)width * height * 3, 0);
    const std::uint8_t *p = src;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx, p += 8)
            etc1_decode_block(p, out.data(), width, height, bx * 4, by * 4);
    return true;
}

/* A readable name for the compressed formats PvZ2 could plausibly ship, so the
 * log names the format instead of printing a bare hex enum. */
const char *compressed_format_name(GLenum fmt) {
    switch (fmt) {
        case 0x8D64: return "ETC1_RGB8_OES";
        case 0x9274: return "COMPRESSED_RGB8_ETC2";
        case 0x9278: return "COMPRESSED_RGBA8_ETC2_EAC";
        case 0x9276: return "COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2";
        case 0x8C00: return "PVRTC_RGB_4BPPV1";
        case 0x8C01: return "PVRTC_RGB_2BPPV1";
        case 0x8C02: return "PVRTC_RGBA_4BPPV1";
        case 0x8C03: return "PVRTC_RGBA_2BPPV1";
        case 0x83F0: return "S3TC_DXT1_RGB";
        case 0x83F1: return "S3TC_DXT1_RGBA";
        case 0x83F2: return "S3TC_DXT3_RGBA";
        case 0x83F3: return "S3TC_DXT5_RGBA";
        default:     return "unknown";
    }
}

/* Once, on the first compressed upload: dump the compressed formats the HOST
 * driver actually advertises. This is the crux of the "black on some PCs"
 * report -- PvZ2 uploads Android-format compressed textures (ETC1 and friends)
 * straight through, and a driver that does not list the format rejects the
 * upload, leaving the texture black. On the dev machine the format is present
 * (or software-decoded by the driver) so nothing is wrong there; a weaker Intel
 * GPU with an older driver lists neither ETC nor PVRTC and every background and
 * plant -- everything not uploaded as plain RGBA -- comes out black, while the
 * UI built from uncompressed atlases stays visible. Listing what the driver
 * supports next to what the engine asks for turns that guess into a fact. */
void log_compressed_support_once(GuestCall &c) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    GLint n = 0;
    glGetIntegerv(0x86A2 /* GL_NUM_COMPRESSED_TEXTURE_FORMATS */, &n);
    if (n < 0) n = 0;
    if (n > 128) n = 128;
    std::vector<GLint> formats((std::size_t)n, 0);
    if (n > 0) glGetIntegerv(0x86A3 /* GL_COMPRESSED_TEXTURE_FORMATS */, formats.data());
    const GLubyte *renderer = gl_get_string(0x1F01 /* GL_RENDERER */);
    c.log("[gl] host renderer: %s", renderer ? (const char *)renderer : "?");
    c.log("[gl] host advertises %d compressed texture format(s):", (int)n);
    for (GLint i = 0; i < n; ++i) {
        c.log("[gl]   0x%04x %s", (unsigned)formats[i], compressed_format_name((GLenum)formats[i]));
    }
}

void gl_glCompressedTexImage2D(GuestCall &c) {
            const GLenum internalformat = (GLenum)c.arg(2);
            const GLsizei width = (GLsizei)c.arg(3);
            const GLsizei height = (GLsizei)c.arg(4);

            log_compressed_support_once(c);
            /* Name each distinct format the engine uploads once, so we see the
             * working set without a per-texture flood. */
            {
                static std::mutex fmt_lock;
                static std::set<GLenum> seen;
                bool first;
                {
                    std::lock_guard<std::mutex> lk(fmt_lock);
                    first = seen.insert(internalformat).second;
                }
                if (first) {
                    c.log("[gl] engine uploads compressed format 0x%04x %s (first seen %dx%d)",
                          (unsigned)internalformat, compressed_format_name(internalformat),
                          (int)width, (int)height);
                }
            }

            const GLint level = (GLint)c.arg(1);
            const GLsizei imageSize = (GLsizei)c.arg(6);
            const void *data = c.ptr(c.arg(7));

            /* ETC1: decode to RGB8 on the CPU and upload uncompressed, so it no
             * longer matters whether this driver exposes ETC1/ETC2 at all. Every
             * non-RGBA PvZ2 texture takes this path -- backgrounds, plants, the
             * lot -- and passing ETC1 straight through is exactly what left them
             * black on drivers that reject the format. A per-thread scratch
             * buffer avoids re-allocating a few MB on every texture; GL runs on
             * one thread, but thread_local costs nothing and is safe regardless. */
            if (internalformat == 0x8D64 /* GL_ETC1_RGB8_OES */) {
                static thread_local std::vector<std::uint8_t> rgb;
                if (etc1_decode((const std::uint8_t *)data, imageSize, width, height, rgb)) {
                    gl_tex_image_2_d(c.arg(0), level, 0x8051 /* GL_RGB8 */, width, height, 0,
                                     0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, rgb.data());
                    report_gl_error(c, "glTexImage2D(ETC1->RGB8)");
                    return;
                }
                /* Undersized source -- fall through to the raw upload, which at
                 * worst reproduces the old behaviour rather than reading OOB. */
            }

            gl_compressed_tex_image_2_d(c.arg(0), level, internalformat, width, height, c.arg(5),
                                        imageSize, data);

            /* Did the driver accept it? A GL_INVALID_ENUM here is the whole bug:
             * the format is not supported and this texture is now black. */
            report_gl_error(c, "glCompressedTexImage2D");
}

void gl_glCreateProgram(GuestCall &c) {
            c.regs[0] = gl_create_program();
}

void gl_glCreateShader(GuestCall &c) {
            c.regs[0] = gl_create_shader(c.arg(0));
}

void gl_glCullFace(GuestCall &c) {
            gl_cull_face(c.arg(0));
}

void gl_glDeleteFramebuffers(GuestCall &c) {
            gl_delete_framebuffers(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glDeleteProgram(GuestCall &c) {
            gl_delete_program(c.arg(0));
}

void gl_glDeleteShader(GuestCall &c) {
            gl_delete_shader(c.arg(0));
}

void gl_glDeleteTextures(GuestCall &c) {
            gl_delete_textures(c.arg(0), (const GLuint *)c.ptr(c.arg(1)));
}

void gl_glDepthFunc(GuestCall &c) {
            gl_depth_func(c.arg(0));
}

void gl_glDepthMask(GuestCall &c) {
            gl_depth_mask((GLboolean)c.arg(0));
}

void gl_glDepthRangef(GuestCall &c) {
            gl_depth_range_f(c.argf(0), c.argf(1));
}

void gl_glDisable(GuestCall &c) {
            gl_disable(c.arg(0));
}

void gl_glDisableClientState(GuestCall &c) {
            gl_disable_client_state(c.arg(0));
}

void gl_glDisableVertexAttribArray(GuestCall &c) {
            gl_disable_vertex_attrib_array(c.arg(0));
}

void gl_glEnable(GuestCall &c) {
            gl_enable(c.arg(0));
}

void gl_glEnableClientState(GuestCall &c) {
            gl_enable_client_state(c.arg(0));
}

void gl_glEnableVertexAttribArray(GuestCall &c) {
            gl_enable_vertex_attrib_array(c.arg(0));
}

void gl_glFramebufferTexture2D(GuestCall &c) {
            gl_framebuffer_texture_2_d(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4));
}

void gl_glFrontFace(GuestCall &c) {
            gl_front_face(c.arg(0));
}

void gl_glGenFramebuffers(GuestCall &c) {
            gl_gen_framebuffers(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glGenTextures(GuestCall &c) {
            gl_gen_textures(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glGetError(GuestCall &c) {
            c.regs[0] = gl_get_error();
}

void gl_glGetIntegerv(GuestCall &c) {
            gl_get_integer_v(c.arg(0), (GLint *)c.ptr(c.arg(1)));
}

void gl_glGetProgramInfoLog(GuestCall &c) {
            gl_get_program_info_log(c.arg(0), c.arg(1), (GLsizei *)c.ptr(c.arg(2)), (GLchar *)c.ptr(c.arg(3)));
}

void gl_glGetProgramiv(GuestCall &c) {
            gl_get_program_i_v(c.arg(0), c.arg(1), (GLint *)c.ptr(c.arg(2)));
}

void gl_glGetShaderInfoLog(GuestCall &c) {
            gl_get_shader_info_log(c.arg(0), c.arg(1), (GLsizei *)c.ptr(c.arg(2)), (GLchar *)c.ptr(c.arg(3)));
}

void gl_glGetShaderiv(GuestCall &c) {
            gl_get_shader_i_v(c.arg(0), c.arg(1), (GLint *)c.ptr(c.arg(2)));
}

void gl_glGetString(GuestCall &c) {
            const GLubyte *s = gl_get_string(c.arg(0));
            const char *cs = s ? (const char *)s : "";
            uint32_t len = (uint32_t)std::strlen(cs) + 1;
            uint32_t addr = c.rt->heap.alloc(len);
            /* A driver string, not guest memory -- must be copied into the
             * guest's own address space before handing its address back,
             * unlike every other pointer here which already lives in
             * c.img->mem and can be passed through as-is. */
            if (addr) std::memcpy(&c.img->mem[addr], cs, len);
            c.regs[0] = addr;
}

void gl_glIsProgram(GuestCall &c) {
            c.regs[0] = gl_is_program(c.arg(0));
}

void gl_glIsShader(GuestCall &c) {
            c.regs[0] = gl_is_shader(c.arg(0));
}

void gl_glIsTexture(GuestCall &c) {
            c.regs[0] = gl_is_texture(c.arg(0));
}

void gl_glLineWidth(GuestCall &c) {
            gl_line_width(c.argf(0));
}

void gl_glLoadIdentity(GuestCall &c) {
            gl_load_identity();
}

void gl_glLoadMatrixf(GuestCall &c) {
            gl_load_matrix_f((const GLfloat *)c.ptr(c.arg(0)));
}

void gl_glMatrixMode(GuestCall &c) {
            gl_matrix_mode(c.arg(0));
}

void gl_glNormalPointer(GuestCall &c) {
            gl_normal_pointer(c.arg(0), c.arg(1), c.ptr(c.arg(2)));
}

void gl_glPixelStorei(GuestCall &c) {
            gl_pixel_storei(c.arg(0), c.arg(1));
}

void gl_glPopMatrix(GuestCall &c) {
            gl_pop_matrix();
}

void gl_glPushMatrix(GuestCall &c) {
            gl_push_matrix();
}

void gl_glScalef(GuestCall &c) {
            gl_scale_f(c.argf(0), c.argf(1), c.argf(2));
}

void gl_glScissor(GuestCall &c) {
            gl_scissor(c.arg(0), c.arg(1), c.arg(2), c.arg(3));
}

void gl_glShadeModel(GuestCall &c) {
            gl_shade_model(c.arg(0));
}

void gl_glShaderSource(GuestCall &c) {
            /* The guest ships GLSL ES 1.00. Our context is desktop GL, whose
             * compiler rejects the ES precision qualifiers outright. Desktop
             * GLSL 1.30 accepts lowp/mediump/highp as no-ops, but GLSL 1.20
             * and earlier do not -- they error on them. So we drop to
             * #version 110 (OpenGL 2.0) and strip the precision tokens ourselves.
             * Shaders that already declare their own #version are passed
             * through untouched. */
            GLuint shader = c.arg(0);
            GLsizei count = (GLsizei)c.arg(1);
            uint32_t strings_ptr = c.arg(2);
            uint32_t length_ptr = c.arg(3);

            std::string src;
            for (GLsizei i = 0; i < count; ++i) {
                uint32_t guest_str_addr = c.read32(strings_ptr + (uint32_t)i * 4);
                const char *chunk = (const char *)c.ptr(guest_str_addr);
                if (chunk == nullptr) continue;
                if (length_ptr) {
                    GLint len = (GLint)c.read32(length_ptr + (uint32_t)i * 4);
                    if (len >= 0) { src.append(chunk, (size_t)len); continue; }
                }
                src.append(chunk);
            }

            if (src.find("#version") == std::string::npos) {
                /* Strip precision ...; statements (e.g. "precision mediump float;") */
                for (size_t p = src.find("precision"); p != std::string::npos; p = src.find("precision", p)) {
                    size_t semi = src.find(';', p);
                    if (semi == std::string::npos) break;
                    src.erase(p, semi - p + 1);
                }
                /* Strip lowp / mediump / highp qualifiers */
                auto is_id_char = [](char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_';
                };
                struct { const char *s; size_t n; } const pats[] = {
                    {"lowp", 4}, {"mediump", 7}, {"highp", 5}
                };
                for (auto &pat : pats) {
                    for (size_t p = 0; (p = src.find(pat.s, p)) != std::string::npos; ) {
                        size_t after = p + pat.n;
                        bool word = (p == 0 || !is_id_char(src[p - 1])) &&
                                    (after >= src.size() || !is_id_char(src[after]));
                        if (word) { src.erase(p, pat.n); }
                        else      { p = after; }
                    }
                }
                src.insert(0, "#version 110\n");
            }

            /* [gl] flat_fragment=1 replaces every fragment shader's body with a
             * constant magenta output, keeping its declarations so it still
             * links against the same attributes and uniforms.
             *
             * This is a bisection, not a fix. The composite draw has had every
             * input verified -- geometry, matrix, attribute indices, vertex
             * data, source texture, sampler, shader source, blend, scissor,
             * depth, stencil, cull, colour mask, frame order, GL errors -- and
             * still puts nothing on the window, while an identical draw earlier
             * in the run does. Emitting a constant splits what is left in two:
             * a magenta window means the draw rasterises and the fault is in
             * the sampling; a black one means it produces no fragments at all. */
            const bool flat = pvz2_config()->gl_flat_fragment != 0;
            if (flat && src.find("gl_FragColor") != std::string::npos) {
                const size_t body = src.find("void main");
                if (body != std::string::npos) {
                    src.resize(body);
                    src += "void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";
                }
            }

            const GLchar *one = src.c_str();
            GLint one_len = (GLint)src.size();
            gl_shader_source(shader, 1, &one, &one_len);
}

void gl_glTexCoordPointer(GuestCall &c) {
            gl_tex_coord_pointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

void gl_glTexEnvf(GuestCall &c) {
            gl_tex_env_f(c.arg(0), c.arg(1), c.argf(2));
}

void gl_glTexImage2D(GuestCall &c) {
            gl_tex_image_2_d(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5),
                              c.arg(6), c.arg(7), c.ptr(c.arg(8)));
}

void gl_glTexParameteri(GuestCall &c) {
            gl_tex_parameter_i(c.arg(0), c.arg(1), c.arg(2));
}

void gl_glTexSubImage2D(GuestCall &c) {
            gl_tex_sub_image_2_d(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5),
                                  c.arg(6), c.arg(7), c.ptr(c.arg(8)));
}

void gl_glUniform1i(GuestCall &c) {
            gl_uniform_1_i(c.arg(0), c.arg(1));
}

void gl_glUniform4fv(GuestCall &c) {
            gl_uniform_4_f_v(c.arg(0), c.arg(1), (const GLfloat *)c.ptr(c.arg(2)));
}

/* The location of a program's single mat4 uniform, or -1 if it has none or
 * more than one. Cached because it costs a full uniform enumeration and is
 * fixed for the life of a link. */
GLint sole_mat4_location(GLuint program) {
    static std::mutex lock;
    static std::unordered_map<GLuint, GLint> cache;
    {
        std::lock_guard<std::mutex> lk(lock);
        auto it = cache.find(program);
        if (it != cache.end()) return it->second;
    }

    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    GLint found = -1;
    for (GLint i = 0; i < count; ++i) {
        char nm[128] = {0};
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(nm) - 1, &len, &size, &type, nm);
        if (type != GL_FLOAT_MAT4) continue;
        if (found != -1) { found = -1; break; } /* ambiguous: leave it alone */
        found = glGetUniformLocation(program, nm);
    }
    std::lock_guard<std::mutex> lk(lock);
    cache[program] = found;
    return found;
}

void gl_glUniformMatrix4fv(GuestCall &c) {
    GLint loc = (GLint)c.arg(0);

    /* sub_ABBCEC uploads the screen matrix to TWO shaders back to back, using
     * locations it cached per shader, and without a glUseProgram between them
     * -- so both uploads land on whichever program is currently bound. That is
     * only harmless if every program agrees on where its mat4 lives, which is
     * true on the Android GLES driver (screenMatrix comes out at location 0
     * everywhere) and false here: the desktop compiler orders uniforms so that
     * programs declaring Tex0/Tex1 first put screenMatrix at 2. The mismatched
     * upload is rejected with INVALID_OPERATION, the projection matrix never
     * arrives, and the frame composites with garbage -- a black screen.
     *
     * Re-resolving against the bound program fixes it without touching the
     * engine's caching. Only done when the program has exactly one mat4, so a
     * shader with several is never second-guessed. */
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program != 0) {
        const GLint actual = sole_mat4_location((GLuint)program);
        if (actual >= 0 && actual != loc) {
            static std::atomic<std::uint32_t> budget{4};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.log("[gl] program %d: remapping mat4 uniform location %d -> %d "
                      "(engine cached another program's location)", program, loc, actual);
            }
            loc = actual;
        }
    }

    const GLfloat *m = (const GLfloat *)c.ptr(c.arg(3));

    /* Reject a non-finite projection matrix, and reuse the last good one.
     *
     * This is the actual reason the screen is black. Read back from the
     * program at draw time, screenMatrix holds
     *     [ inf  nan  nan -inf ]
     *     [ nan -inf  nan  inf ]
     * Every vertex transformed by it becomes NaN, so the rasteriser emits no
     * fragments at all -- which is why even a constant-colour fragment shader
     * could not put anything on the window, while glClear still could.
     *
     * The inf comes from the same corrupt value as the zero-width viewport
     * handled in gl_glViewport: an orthographic projection is built as
     * 2/width, and 2/0 is inf. Both are symptoms of a width of 0 arriving from
     * the engine's render-state cache (sub_ABC650 reads x/y/w/h at stride 124
     * and gets them one slot out); the real fix is upstream, in whatever
     * corrupts that cache. Until then, holding the last finite matrix keeps
     * the projection usable instead of poisoning every vertex. */
   if (m != nullptr && c.arg(1) == 1) {
        static constexpr int kMaxCached = 16;
        static GLint cached_progs[kMaxCached] = {};
        static std::array<GLfloat, 16> cached_mats[kMaxCached] = {};
        static int cached_count = 0;

        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            const float v = m[i];
            if (v != v || v > 3.0e38f || v < -3.0e38f) { finite = false; break; }
        }
        if (finite) {
            int idx = -1;
            for (int i = 0; i < cached_count; ++i) {
                if (cached_progs[i] == program) { idx = i; break; }
            }
            if (idx < 0 && cached_count < kMaxCached) idx = cached_count++;
            if (idx >= 0) {
                cached_progs[idx] = program;
                std::memcpy(cached_mats[idx].data(), m, sizeof(cached_mats[idx]));
            }
        } else {
            int idx = -1;
            for (int i = 0; i < cached_count; ++i) {
                if (cached_progs[i] == program) { idx = i; break; }
            }
            static std::atomic<std::uint32_t> budget{4};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.log("[gl] program %d: screenMatrix upload is non-finite (inf/nan) -- %s",
                      program, idx >= 0 ? "substituting the last finite one"
                                        : "no finite matrix seen yet, dropping the upload");
            }
            if (idx >= 0) {
                std::array<GLfloat, 16> fixed = cached_mats[idx];
                for (int i = 0; i < 16; ++i) {
                    const float bad = m[i];
                    if (bad != bad) continue;
                    if (bad <= 3.0e38f && bad >= -3.0e38f) continue;
                    const bool want_negative = bad < 0.0f;
                    if (want_negative != (fixed[i] < 0.0f)) fixed[i] = -fixed[i];
                }
                gl_uniform_matrix_4_f_v(loc, 1, (GLboolean)c.arg(2), fixed.data());
            }
            return;
        }
    }

    /* The window is pure black, yet a forced clear colour DOES show through --
     * which means the composite quad is not covering the screen at all, rather
     * than covering it with black. So the projection matrix it is drawn with is
     * the thing to look at. Logged once per program, column-major as GL takes
     * it. A sane screen matrix for a 1280x720 target has ~2/1280 and ~-2/720 on
     * the diagonal (or 1.0s if the engine feeds clip-space coordinates). */
    if (m != nullptr) {
        static std::mutex seen_lock;
        static std::set<GLint> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(seen_lock);
            first = seen.size() < 8 && seen.insert(program).second;
        }
        if (first) {
            c.log("[gl] screenMatrix for program %d (loc %d):", program, loc);
            for (int row = 0; row < 4; ++row) {
                c.log("[gl]   % .5f % .5f % .5f % .5f", m[row], m[row + 4], m[row + 8], m[row + 12]);
            }
        }
    }

    gl_uniform_matrix_4_f_v(loc, c.arg(1), (GLboolean)c.arg(2), m);
}

void gl_glUseProgram(GuestCall &c) {
            gl_use_program(c.arg(0));
}

void gl_glVertexAttribPointer(GuestCall &c) {
            gl_vertex_attrib_pointer(c.arg(0), c.arg(1), c.arg(2), (GLboolean)c.arg(3), c.arg(4),
                                      c.ptr(c.arg(5)));
}

void gl_glVertexPointer(GuestCall &c) {
            gl_vertex_pointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

/* glGetUniformLocation was the one GLES symbol with no implementation at all:
 * it returns the uniform's location, and returning 0 unconditionally would
 * silently alias every uniform onto slot 0. */
void gl_glGetUniformLocation(GuestCall &c) {
    std::string nm = c.cstr(c.arg(1), 256);
    const GLint loc = glGetUniformLocation(c.arg(0), nm.c_str());
    /* The engine ends up calling glUniformMatrix4fv with location 0, which is
     * a sampler2D -- so either it never asks for the matrix's location, or it
     * asks and does not get 2. Logging every query answers which, and a failed
     * lookup (-1) shows up here rather than as a mystery INVALID_OPERATION
     * three calls later. */
    if (gl_strict_enabled()) {
        static std::atomic<std::uint32_t> budget{40};
        if (budget.load(std::memory_order_relaxed) > 0) {
            budget.fetch_sub(1, std::memory_order_relaxed);
            c.log("[gl-strict] glGetUniformLocation(program=%u, \"%s\") -> %d", c.arg(0),
                  nm.c_str(), (int)loc);
        }
    }
    c.set_result((std::uint32_t)loc);
}

}  // namespace

bool gl_strict_enabled() { return pvz2_config()->gl_strict != 0; }

/* glUniformMatrix4fv rejects with INVALID_OPERATION when the uniform at that
 * location is not a mat4 (or the location does not belong to the current
 * program). Both are invisible without asking the program what it actually
 * declares, so list every active uniform with its location and type. Once. */
void dump_program_uniforms(GuestCall &c) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;

    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program == 0) {
        c.log("[gl-strict] no current program -- that alone explains the INVALID_OPERATION");
        return;
    }
    GLint count = 0;
    glGetProgramiv((GLuint)program, GL_ACTIVE_UNIFORMS, &count);
    c.log("[gl-strict] program %d declares %d active uniform(s):", program, count);
    for (GLint i = 0; i < count; ++i) {
        char nm[128] = {0};
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform((GLuint)program, (GLuint)i, (GLsizei)sizeof(nm) - 1, &len, &size, &type, nm);
        const GLint loc = glGetUniformLocation((GLuint)program, nm);
        /* 0x8B5C is GL_FLOAT_MAT4 -- the only type glUniformMatrix4fv accepts. */
        c.log("[gl-strict]   loc=%d type=0x%04x%s size=%d name=%s", loc, (unsigned)type,
              type == 0x8B5C ? " (mat4)" : "", size, nm);
    }
}

void gl_check_error_after(GuestCall &c, const char *name) {
    GLenum err = gl_get_error();
    if (err == GL_NO_ERROR) return;
    static std::atomic<std::uint32_t> budget{20};
    if (budget.load(std::memory_order_relaxed) == 0) return;
    budget.fetch_sub(1, std::memory_order_relaxed);
    c.log("[gl-strict] %s -> GL error 0x%04x (r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x) lr=0x%08x",
          name, (unsigned)err, c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.lr());
    if (std::strncmp(name, "glUniform", 9) == 0) dump_program_uniforms(c);
}

void set_drawable_size(std::uint32_t width, std::uint32_t height) {
    g_drawable_w.store(width, std::memory_order_relaxed);
    g_drawable_h.store(height, std::memory_order_relaxed);
}

void register_libgles(ImportTable &t) {
    t.add("glGetUniformLocation", gl_glGetUniformLocation);
    t.add("glDrawArrays", gl_glDrawArrays);
    t.add("glDrawElements", gl_glDrawElements);
    t.add("glViewport", gl_glViewport);
    t.add("glActiveTexture", gl_glActiveTexture);
    t.add("glAlphaFunc", gl_glAlphaFunc);
    t.add("glAttachShader", gl_glAttachShader);
    t.add("glBindAttribLocation", gl_glBindAttribLocation);
    t.add("glBindFramebuffer", gl_glBindFramebuffer);
    t.add("glBindFramebufferOES", gl_glBindFramebuffer);
    t.add("glBindTexture", gl_glBindTexture);
    t.add("glBlendFunc", gl_glBlendFunc);
    t.add("glCheckFramebufferStatus", gl_glCheckFramebufferStatus);
    t.add("glCheckFramebufferStatusOES", gl_glCheckFramebufferStatus);
    t.add("glClear", gl_glClear);
    t.add("glClearColor", gl_glClearColor);
    t.add("glClearDepthf", gl_glClearDepthf);
    t.add("glClientActiveTexture", gl_glClientActiveTexture);
    t.add("glColorMask", gl_glColorMask);
    t.add("glColorPointer", gl_glColorPointer);
    t.add("glCompileShader", gl_glCompileShader);
    t.add("glLinkProgram", gl_glLinkProgram);
    t.add("glCompressedTexImage2D", gl_glCompressedTexImage2D);
    t.add("glCreateProgram", gl_glCreateProgram);
    t.add("glCreateShader", gl_glCreateShader);
    t.add("glCullFace", gl_glCullFace);
    t.add("glDeleteFramebuffers", gl_glDeleteFramebuffers);
    t.add("glDeleteFramebuffersOES", gl_glDeleteFramebuffers);
    t.add("glDeleteProgram", gl_glDeleteProgram);
    t.add("glDeleteShader", gl_glDeleteShader);
    t.add("glDeleteTextures", gl_glDeleteTextures);
    t.add("glDepthFunc", gl_glDepthFunc);
    t.add("glDepthMask", gl_glDepthMask);
    t.add("glDepthRangef", gl_glDepthRangef);
    t.add("glDisable", gl_glDisable);
    t.add("glDisableClientState", gl_glDisableClientState);
    t.add("glDisableVertexAttribArray", gl_glDisableVertexAttribArray);
    t.add("glEnable", gl_glEnable);
    t.add("glEnableClientState", gl_glEnableClientState);
    t.add("glEnableVertexAttribArray", gl_glEnableVertexAttribArray);
    t.add("glFramebufferTexture2D", gl_glFramebufferTexture2D);
    t.add("glFramebufferTexture2DOES", gl_glFramebufferTexture2D);
    t.add("glFrontFace", gl_glFrontFace);
    t.add("glGenFramebuffers", gl_glGenFramebuffers);
    t.add("glGenFramebuffersOES", gl_glGenFramebuffers);
    t.add("glGenTextures", gl_glGenTextures);
    t.add("glGetError", gl_glGetError);
    t.add("glGetIntegerv", gl_glGetIntegerv);
    t.add("glGetProgramInfoLog", gl_glGetProgramInfoLog);
    t.add("glGetProgramiv", gl_glGetProgramiv);
    t.add("glGetShaderInfoLog", gl_glGetShaderInfoLog);
    t.add("glGetShaderiv", gl_glGetShaderiv);
    t.add("glGetString", gl_glGetString);
    t.add("glIsProgram", gl_glIsProgram);
    t.add("glIsShader", gl_glIsShader);
    t.add("glIsTexture", gl_glIsTexture);
    t.add("glLineWidth", gl_glLineWidth);
    t.add("glLoadIdentity", gl_glLoadIdentity);
    t.add("glLoadMatrixf", gl_glLoadMatrixf);
    t.add("glMatrixMode", gl_glMatrixMode);
    t.add("glNormalPointer", gl_glNormalPointer);
    t.add("glPixelStorei", gl_glPixelStorei);
    t.add("glPopMatrix", gl_glPopMatrix);
    t.add("glPushMatrix", gl_glPushMatrix);
    t.add("glScalef", gl_glScalef);
    t.add("glScissor", gl_glScissor);
    t.add("glShadeModel", gl_glShadeModel);
    t.add("glShaderSource", gl_glShaderSource);
    t.add("glTexCoordPointer", gl_glTexCoordPointer);
    t.add("glTexEnvf", gl_glTexEnvf);
    t.add("glTexImage2D", gl_glTexImage2D);
    t.add("glTexParameteri", gl_glTexParameteri);
    t.add("glTexSubImage2D", gl_glTexSubImage2D);
    t.add("glUniform1i", gl_glUniform1i);
    t.add("glUniform4fv", gl_glUniform4fv);
    t.add("glUniformMatrix4fv", gl_glUniformMatrix4fv);
    t.add("glUseProgram", gl_glUseProgram);
    t.add("glVertexAttribPointer", gl_glVertexAttribPointer);
    t.add("glVertexPointer", gl_glVertexPointer);
}

}  // namespace sprout

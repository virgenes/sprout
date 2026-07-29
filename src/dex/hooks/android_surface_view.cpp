/* com.popcap.SexyAppFramework.AndroidSurfaceView
 *
 * The GLSurfaceView the engine renders into. Everything it asks this class is
 * about the drawing surface itself: how big it is, what GL level it has, which
 * framebuffer object represents the screen.
 */

#include <sprout/dex/dex.h>

namespace sprout {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidSurfaceView";

/* boolean Graphics_IsOpenGLES20() -- the host context is a desktop GL
 * compatibility profile, which exposes everything GLES2 needs (and the GLES1
 * fixed-function calls the engine also makes). */
void is_opengl_es20(DexCall &d) { d.ret_bool(true); }

/* boolean Graphics_CanSetGLViewScaleFactor() */
void can_set_scale_factor(DexCall &d) { d.ret_bool(true); }

/* void Graphics_GetScreenSizeInPixels(int[] outWidthHeight)
 * void Graphics_GetScreenSizeInPoints(int[] outWidthHeight)
 *
 * The real Java fills the array from mOrigAppWidth/mOrigAppHeight. THIS is
 * where LawnApp's mOrigScreenWidth/mOrigScreenHeight ultimately come from, and
 * every geometric quantity derives from them: SetWidthHeight computes
 * mWidth = height * (origW / origH), and ReinitForSurfaceChange re-reads
 * mWidth/mHeight for landscape orientations. A bad pair collapses the viewport
 * -- the symptom was glViewport(0, 0, 0, 1280).
 *
 * The array is the first vararg, so it has to be read THROUGH the va_list:
 * writing to r3 directly scribbled over the argument block and left the real
 * array untouched. */
void get_screen_size(DexCall &d) {
    std::uint32_t out = d.arg(0);
    if (out == 0 || !d.c.in_bounds(out, 8)) return;
    d.c.write32(out + 0, screen_width());
    d.c.write32(out + 4, screen_height());
}

/* int Graphics_GetGLViewSysFBO() -- 0 is the default framebuffer, which is
 * exactly what the host window presents. Hooked explicitly so the answer is
 * stated rather than inherited from the unhooked default. */
void get_sys_fbo(DexCall &d) { d.ret(0); }

}  // namespace

void register_android_surface_view(HookTable &t) {
    t.add(kClass, "Graphics_IsOpenGLES20", is_opengl_es20);
    t.add(kClass, "Graphics_CanSetGLViewScaleFactor", can_set_scale_factor);
    t.add(kClass, "Graphics_GetScreenSizeInPixels", get_screen_size);
    t.add(kClass, "Graphics_GetScreenSizeInPoints", get_screen_size);
    t.add(kClass, "Graphics_GetGLViewSysFBO", get_sys_fbo);
}

}  // namespace dex
}  // namespace sprout

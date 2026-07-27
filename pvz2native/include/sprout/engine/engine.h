#ifndef SPROUT_ENGINE_ENGINE_H
#define SPROUT_ENGINE_ENGINE_H

#include <cstdint>

#include <sprout/elf32/elf32_loader.h>
#include <sprout/runtime/guest_runtime.h>

/* The game's own bring-up sequence: which entry points to call, with what, and
 * in what order.
 *
 * This is the layer that knows PvZ2 rather than Android. Every address it uses
 * comes from game/symbols.h, so the knowledge is split in two on purpose: the
 * ORDER of the calls lives here and is shared by every release, while the
 * ADDRESSES live in the symbol table and are per release. A new version should
 * only ever need the table.
 *
 * The order itself is ground-truthed against the real Java in classesdex/, not
 * guessed: Android's linker runs .init_array during dlopen() and JNI_OnLoad
 * right after System.loadLibrary(), then AndroidGameApp drives the lifecycle
 * natives from onCreate/onActivityStart/onActivityResume/onWindowFocusChanged.
 */

namespace sprout {
namespace engine {

/* Load-time bring-up, in the exact order Android performs it. Both halves must
 * run before any Java_/Native_ call: .init_array leaves global C++ objects
 * properly constructed, and JNI_OnLoad allocates the JavaVM/method-ID cache the
 * engine later re-derives a JNIEnv* from. */
void boot_native_library(pvz2_elf_image_t *img, GuestRuntime *rt);

/* EA's own framework bring-up, reached through real exported Java_* symbols
 * (unlike the RegisterNatives-only lifecycle natives below). */
void run_eaframework_startup(pvz2_elf_image_t *img, GuestRuntime *rt);

/* Native_GameAppInitialize, with the nine fake Java objects it expects. */
void run_game_app_initialize(pvz2_elf_image_t *img, GuestRuntime *rt);

/* applicationWillFinishLaunching + the two lifecycle natives that follow it in
 * the real Java call order. */
void run_application_launch(pvz2_elf_image_t *img, GuestRuntime *rt);

/* onSurfaceCreated / onSurfaceChanged / applicationDidBecomeActive. */
void run_surface_lifecycle(pvz2_elf_image_t *img, GuestRuntime *rt);

/* Re-run Native_onSurfaceChanged alone, for a live window resize: the engine
 * re-lays-out and re-renders AT the new resolution instead of upscaling a fixed
 * one. Cheap -- the heavy surface bring-up is all in onSurfaceCreated, which
 * runs once. Must be called on the frame thread (it runs guest code), between
 * frames -- see pvz2_session_frame. */
void run_surface_changed(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t width,
                         std::uint32_t height);

/* One Native_onDrawFrame. `index` only labels the call in the log. */
void draw_frame(pvz2_elf_image_t *img, GuestRuntime *rt, int index);

}  // namespace engine
}  // namespace sprout

#endif

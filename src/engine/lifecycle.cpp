/* The RegisterNatives-only lifecycle natives, in the order AndroidGameApp.java
 * drives them. None of these appear in the dynamic symbol table -- JNI_OnLoad
 * registers them at runtime -- which is why they are reached by offset. */

#include <sprout/engine/engine.h>

#include <vector>

#include <sprout/game/symbols.h>
#include <sprout/runtime/dynarmic_config.h>

namespace sprout {
namespace engine {

namespace rt_ = sprout::runtime;

/* Native_GameAppInitialize.
 *
 * It is declared in AndroidGameApp.java as an INSTANCE native, so JNI hands it
 * (JNIEnv*, jobject thiz, arg0..arg7) -- NINE values, not eight. Passing only
 * the eight declared arguments shifted every object down one register: the
 * engine took our "surfaceView" as `thiz` and our "httpProxy" as the surface
 * view, so Graphics_GetScreenSizeInPixels was invoked on the wrong fake object
 * and left LawnApp's mOrigScreenWidth/Height reading uninitialised array
 * memory. thiz goes first.
 *
 * The class names are ground-truthed against the native method declaration in
 * classesdex/com/popcap/SexyAppFramework/AndroidGameApp.java and tagged with
 * tag_object_class(), so GetObjectClass/GetMethodID/CallXxxMethod on these
 * objects resolve to real (class, method) pairs instead of opaque handles. */
void run_game_app_initialize(pvz2_elf_image_t *img, GuestRuntime *rt) {
    constexpr std::uint32_t kFakeObjBase = 0x0000A000;
    constexpr std::uint32_t kFakeObjStride = 0x40;

    static const char *kArgLabels[9] = {
        "fake_gameApp",          "fake_surfaceView",       "fake_httpProxy",
        "fake_facebookDriver",   "fake_cloud",             "fake_googlePlayConnect",
        "fake_achievements",     "fake_leaderboard",       "fake_notification",
    };
    static const char *kArgClasses[9] = {
        "com/popcap/SexyAppFramework/AndroidGameApp", /* thiz */
        "com/popcap/SexyAppFramework/AndroidSurfaceView",
        "com/popcap/SexyAppFramework/AndroidHttpProxy",
        "com/popcap/SexyAppFramework/AndroidFacebookDriver",
        "com/popcap/SexyAppFramework/cloud/Cloud",
        "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect",
        "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements",
        "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard",
        "com/popcap/SexyAppFramework/AndroidNotification",
    };

    std::vector<std::uint32_t> args;
    for (int i = 0; i < 9; ++i) {
        const std::uint32_t addr = kFakeObjBase + (std::uint32_t)i * kFakeObjStride;
        rt_::make_fake_jstring(img, addr, kArgLabels[i]);
        rt->tag_object_class(addr, kArgClasses[i]);
        args.push_back(addr);
    }
    rt_::run_at_offset(img, rt, "Native_GameAppInitialize", sym().native.game_app_initialize, args);
}

/* Native_applicationWillFinishLaunching builds argv[] and runs the whole
 * SexyAppFramework constructor, which `operator new`s the LawnApp into the
 * global at sym().global.app. Its r1 (thiz) is never dereferenced, so 0 is
 * safe; a null startUrl in r2 is handled gracefully (argv becomes
 * {"Game", NULL}).
 *
 * Then, in real Java call order: onActivityStart ->
 * Native_applicationDidFinishLaunching (a nullsub, kept for fidelity),
 * onActivityResume -> Native_applicationWillBecomeForeground. The latter does
 * NOT do the work itself: it allocates an AndroidAppEvent and appends it to a
 * global std::list, which is drained and dispatched at the top of every
 * onDrawFrame. */
void run_application_launch(pvz2_elf_image_t *img, GuestRuntime *rt) {
    rt_::run_at_offset(img, rt, "Native_applicationWillFinishLaunching",
                       sym().native.application_will_finish_launching, {0, 0});
    rt_::run_at_offset(img, rt, "Native_applicationDidFinishLaunching",
                       sym().native.application_did_finish_launching, {0});
    rt_::run_at_offset(img, rt, "Native_applicationWillBecomeForeground",
                       sym().native.application_will_become_foreground, {0});
}

/* onWindowFocusChanged(true) -> Native_applicationDidBecomeActive bookends the
 * surface setup.
 *
 * onSurfaceChanged reads its size from its THIRD and FOURTH arguments, because
 * as a JNI static method the first two are (JNIEnv*, jclass). Passing
 * {width, height} put them in r0/r1, so the engine read r2/r3 and got stack
 * garbage -- hence the "mOrigScreenWidth = 17533344" and "Resize: 334x1536" in
 * the logs. The size also feeds the projection matrix, so a bogus width poisons
 * rendering as well. */
void run_surface_changed(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t width,
                         std::uint32_t height) {
    /* The dummy words the JNI preamble eats before (width, height) differ per
     * build -- see GameSymbols::surface_changed_pad. per_frame=true keeps it
     * from logging a banner, which matters because a window drag fires this
     * every frame. */
    std::vector<std::uint32_t> size_args(sym().surface_changed_pad, 0u);
    size_args.push_back(width);
    size_args.push_back(height);
    rt_::run_at_offset(img, rt, "Native_onSurfaceChanged", sym().native.on_surface_changed,
                       size_args, true);
}

void run_surface_lifecycle(pvz2_elf_image_t *img, GuestRuntime *rt) {
    rt_::run_at_offset(img, rt, "Native_onSurfaceCreated", sym().native.on_surface_created, {});
    run_surface_changed(img, rt, rt_::kWindowWidth, rt_::kWindowHeight);
    rt_::run_at_offset(img, rt, "Native_applicationDidBecomeActive",
                       sym().native.application_did_become_active, {0});
}

}  // namespace engine
}  // namespace sprout

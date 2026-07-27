#ifndef SPROUT_GAME_SYMBOLS_H
#define SPROUT_GAME_SYMBOLS_H

#include <cstdint>

#include <sprout/elf32/elf32_loader.h>

namespace sprout {

/* Everything about the game binary that is not the same in every build.
 *
 * The harness itself -- the JIT, the libc/GLES/JNI shims, the heap, the audio
 * bridge -- is version-agnostic: it implements Android, and Android does not
 * change between two PvZ2 releases. What DOES change is every address inside
 * libPVZ2.so, and those used to be hex literals inlined at their use sites, so
 * supporting a second release meant editing the middle of the boot logic. They
 * all live here now, and adding a version is adding one entry to kVersions.
 *
 * The lifecycle natives are the reason raw offsets are unavoidable at all:
 * JNI_OnLoad registers them with RegisterNatives, so unlike the Java_* exports
 * they never appear in the dynamic symbol table and cannot be looked up by name.
 *
 * 0 means "not mapped for this version". Callers must treat it as such:
 * `natives` are required and a missing one is a hard error, while a missing
 * diagnostic offset just turns that diagnostic off. */
struct GameSymbols {
    const char *version;

    /* Registered natives -- offsets into the .so, called by run_at_offset. */
    struct {
        std::uint32_t game_app_initialize;
        std::uint32_t application_will_finish_launching;
        std::uint32_t application_did_finish_launching;
        std::uint32_t application_will_become_foreground;
        std::uint32_t application_did_become_active;
        std::uint32_t on_surface_created;
        std::uint32_t on_surface_changed;
        std::uint32_t on_draw_frame;

        /* PumpMessageQueue: drains the lifecycle event queue and dispatches it.
         *
         * 0 when the version has no such native. 1.6 does not: it drains the
         * queue at the top of onDrawFrame itself. 4.5.2 split the two apart, and
         * its onDrawFrame ONLY draws -- so failing to call this leaves every
         * applicationDidBecomeActive/WillBecomeForeground sitting in the queue
         * forever, the driver stuck in its paused state, and every frame taking
         * a fixed-cost branch that updates nothing. The window stays black with
         * no error anywhere; the giveaway is a frame that costs the same ticks
         * every time and makes zero imports. */
        std::uint32_t pump_message_queue;
    } native;

    /* How many dummy words precede (width, height) in the onSurfaceChanged
     * call -- i.e. how many of r1..r3 the JNI preamble eats before the real
     * arguments start.
     *
     * A per-version value because the two builds genuinely differ, and getting
     * it wrong is silent: the engine reads whichever registers it expects and
     * derives its whole projection matrix from them, so a shift by one register
     * produces a plausible-looking window with a poisoned transform rather than
     * a crash. 4.5.2's is the textbook static-native shape -- its body reads r2
     * and r3 -- and was read off the decompilation, not assumed. */
    std::uint32_t surface_changed_pad;

    /* Engine globals, as offsets into the .so. `app` is the LawnApp the
     * SexyAppFramework constructor stores (dword_D55650 in 1.6); `app_driver`
     * is the separate AndroidAppDriver pointer the surface/frame natives
     * actually dereference (dword_DC8FD4) -- they are NOT the same object, and
     * assuming they were cost a long hunt once already. */
    struct {
        std::uint32_t app;
        std::uint32_t app_driver;
    } global;

    /* Guest functions the harness calls directly rather than through a native. */
    struct {
        /* libstdc++ `string(const char*, const allocator&)`, for handing the
         * engine a real std::string argument. */
        std::uint32_t string_ctor;
    } fn;

    /* Engine natives the PORT calls back INTO -- the reverse of `native`. These
     * are the C++ implementations behind Java `native` methods, registered by
     * RegisterNatives, that a Java class fires as a completion callback. This
     * port IS that Java class (see dex/hooks/), so it has to invoke them itself.
     *
     * `http_transaction_error` is AndroidHttpTransaction's error callback. It
     * takes (JNIEnv*, jobject thiz, jlong nativeTransaction) and merely ENQUEUES
     * a failure message onto the same ring buffer PumpMessageQueue drains -- so
     * it is cheap and safe to call. Without it, every HTTP request the engine
     * starts hangs forever instead of failing: on a device the Java side times
     * out after 30s and fires this; here Start() does nothing, so the transaction
     * never completes and the loading screen that waits on it never finishes.
     * 0 when not mapped (1.6, which already reaches the menu, is left untouched). */
    struct {
        std::uint32_t http_transaction_error;
    } jni_native;

    /* Field offsets for the touch diagnostic -- see diagnostics/input_probe.
     * All optional: zeroed out, the probe simply reports nothing. */
    struct {
        std::uint32_t driver;        /* app_driver value -> driver          */
        std::uint32_t scaler;        /* driver -> coordinate scaler         */
        std::uint32_t touch_begin;   /* active-touch vector begin           */
        std::uint32_t touch_end;     /* active-touch vector end             */
        std::uint32_t touch_stride;  /* bytes per entry in that vector      */
        std::uint32_t multitouch;    /* byte flag: multitouch enabled       */
        std::uint32_t touch_active;  /* byte flag: touch dispatch active    */
        std::uint32_t scaler_fields; /* first of 8 int32s in the scaler     */
        std::uint32_t vt_touch_down; /* driver vtable slots, for naming the */
        std::uint32_t vt_touch_up;   /* concrete handlers in the log        */
        std::uint32_t vt_touch_move;
    } input;

    /* First 8 bytes at native.on_draw_frame and native.game_app_initialize.
     * Detection compares these rather than trusting a file size: if they match,
     * the two most important offsets in the table are proven right on this exact
     * binary, and a mismatch is caught before we branch into the middle of some
     * unrelated function. */
    std::uint64_t fingerprint_draw_frame;
    std::uint64_t fingerprint_game_app_init;
};

/* Identifies the loaded image against the built-in table.
 *
 * Returns false when nothing matches, having logged the candidates it tried and
 * what it found instead -- which is the report someone adding a version needs.
 * Booting anyway would mean calling whatever happens to sit at another build's
 * offsets, so the caller must refuse to start. */
bool game_symbols_detect(const pvz2_elf_image_t *img);

/* The detected table. Before a successful detect it is all zeroes with
 * version "unknown", so a stray read cannot hand out a plausible-looking
 * address. */
const GameSymbols &sym();

}  // namespace sprout

#endif

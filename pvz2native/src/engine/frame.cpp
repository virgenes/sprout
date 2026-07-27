/* One frame of the game.
 *
 * Native_onDrawFrame is the whole steady state: it drains the AndroidAppEvent
 * list, ticks LawnApp::Update and draws. The host, not this layer, owns the
 * loop -- SDL has to pump its message queue and swap buffers between frames,
 * which is impossible while one call runs the boot and hundreds of frames to
 * completion (the window would never pump and Windows would mark it
 * "Not responding"). */

#include <sprout/engine/engine.h>

#include <cstdio>

#include <sprout/game/symbols.h>
#include <sprout/runtime/dynarmic_config.h>

namespace sprout {
namespace engine {

void draw_frame(pvz2_elf_image_t *img, GuestRuntime *rt, int index) {
    char label[64];

    /* Deliver queued lifecycle events BEFORE drawing, when the version has a
     * separate native for it -- see GameSymbols::native.pump_message_queue.
     * The lifecycle natives only ENQUEUE (applicationDidBecomeActive builds a
     * message and hands it to a mutex-guarded ring buffer), so without this the
     * engine never learns it is in the foreground and every frame draws
     * nothing. Java calls both per frame; the order matters, because a frame
     * should act on the events that preceded it. */
    if (sym().native.pump_message_queue != 0) {
        std::snprintf(label, sizeof(label), "PumpMessageQueue[%d]", index);
        runtime::run_at_offset(img, rt, label, sym().native.pump_message_queue, {}, true);
    }

    std::snprintf(label, sizeof(label), "Native_onDrawFrame[%d]", index);
    runtime::run_at_offset(img, rt, label, sym().native.on_draw_frame, {}, true);
}

}  // namespace engine
}  // namespace sprout

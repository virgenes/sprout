#ifndef SPROUT_DIAGNOSTICS_DIAGNOSTICS_H
#define SPROUT_DIAGNOSTICS_DIAGNOSTICS_H

#include <cstdint>

#include <sprout/elf32/elf32_loader.h>
#include <sprout/runtime/guest_runtime.h>

/* Tools for asking the running guest a question, kept apart from the code that
 * boots it.
 *
 * This directory has been pruned twice and should be pruned again: a probe
 * written to chase one bug is dead weight the day that bug is understood, and
 * it is dead weight that still has to be ported to every new game version,
 * because every probe is a pile of struct offsets. What survives here is only
 * what has proved useful more than once.
 *
 * The generic instruments -- the memory watchpoint and the PC sampler -- live
 * in runtime/dynarmic_config.h instead, because they can only be implemented
 * from inside the memory callbacks and the Run() loop.
 */

namespace sprout {
namespace diagnostics {

/* The screen->app coordinate transform every touch goes through, plus the two
 * flags that gate touch dispatch.
 *
 * This is the one thing the input log cannot show. We can prove we delivered
 * DOWN and UP at sensible window pixels and that the engine kept running
 * frames -- and still not know WHERE in app space those pixels landed. The
 * transform is
 *     x_app = out_origin.x + (x_win - in_origin.x) * num.x / den.x
 * and, critically, the engine SKIPS it entirely when either denominator is
 * zero, passing raw window pixels straight through. A zero there would also
 * explain the zero-width viewport seen on the composite draw: same size state,
 * same zero. */
void dump_touch_scaler(pvz2_elf_image_t *img);

/* Dumps a guest global by raw .so offset -- for empirically checking which
 * lifecycle call writes which global, instead of guessing from static
 * disassembly. */
void dump_dword(pvz2_elf_image_t *img, const char *label, std::uint32_t offset);

/* Builds a guest std::string and returns its address, for calling engine
 * functions that take one by reference.
 *
 * Kept even with no caller: it is the only way to pass a string into the
 * engine, and rebuilding it means rediscovering that the right entry point is
 * libstdc++'s `string(const char*, const allocator&)` and that it needs a dummy
 * allocator argument. The blocks are never freed -- these are built once and
 * reused for the life of the process. Returns 0 if the heap is exhausted or the
 * detected version does not map the constructor. */
std::uint32_t make_guest_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *text);

}  // namespace diagnostics
}  // namespace sprout

#endif

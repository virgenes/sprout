/* libstdc++.so / libgcc -- the ARM C++ runtime support the engine imports.
 *
 * Only the unwinder entry point needs real behaviour. libPVZ2.so is built with
 * exceptions, and when one is thrown the personality routine asks
 * __gnu_Unwind_Find_exidx for the ARM exception index table covering the
 * throwing PC. Returning 0 (the old silent stub) means "no unwind information
 * here", which turns every throw into an immediate terminate -- and since the
 * engine catches its own exceptions for things like missing resources, that
 * failure would look like an unexplained abort far from its cause.
 *
 * The table is a real segment of the loaded image: PT_ARM_EXIDX, which the ELF
 * loader now records (26962 entries in this build).
 */

#include <sprout/dependencies/dependency.h>

#include <cstdio>

namespace sprout {
namespace {

/* const uint32_t *__gnu_Unwind_Find_exidx(uint32_t pc, int *pcount)
 *
 * Returns the base of the exidx table and writes its entry count. Entries are
 * 8 bytes. This image has exactly one such segment, so the pc argument only
 * needs a range check rather than a search across shared objects. */
void unwind_find_exidx(GuestCall &c) {
    std::uint32_t pcount = c.arg(1);
    if (c.img->exidx_size == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("pvz2: [libstdc++] no PT_ARM_EXIDX in the image -- C++ throws cannot unwind\n");
        }
        if (pcount != 0) c.write32(pcount, 0);
        c.set_result(0);
        return;
    }
    if (pcount != 0) c.write32(pcount, c.img->exidx_size / 8);
    c.set_result(c.img->so_base + c.img->exidx_vaddr);
}

}  // namespace

void register_libstdcxx(ImportTable &t) {
    t.add("__gnu_Unwind_Find_exidx", unwind_find_exidx);
    /* The same function under bionic's own name -- identical contract
     * (pc, int *pcount) -> table base. 1.6 imports the __gnu_ spelling and
     * 4.5.2 this one, and without it every C++ `throw` in 4.5.2 would fail to
     * find an unwind table and terminate instead of reaching its handler. */
    t.add("dl_unwind_find_exidx", unwind_find_exidx);
}

}  // namespace sprout

/* Reading a guest global, and handing the guest a std::string -- see
 * diagnostics.h. */

#include <sprout/diagnostics/diagnostics.h>

#include <cstdio>
#include <cstring>

#include <sprout/game/symbols.h>
#include <sprout/runtime/dynarmic_config.h>

namespace sprout {
namespace diagnostics {

void dump_dword(pvz2_elf_image_t *img, const char *label, std::uint32_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, &img->mem[img->so_base + offset], 4);
    std::printf("pvz2: [dbg] %s (offset 0x%x) = 0x%08x\n", label, offset, value);
}

std::uint32_t make_guest_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *text) {
    const std::uint32_t ctor = sym().fn.string_ctor;
    if (ctor == 0) return 0;

    const std::uint32_t len = (std::uint32_t)std::strlen(text) + 1;
    const std::uint32_t cstr = rt->heap.alloc(len);
    const std::uint32_t dest = rt->heap.alloc(8);  /* the string object itself */
    const std::uint32_t alloc = rt->heap.alloc(8); /* dummy allocator argument */
    if (cstr == 0 || dest == 0 || alloc == 0) return 0;
    std::memcpy(&img->mem[cstr], text, len);
    runtime::call_guest_quiet(img, rt, ctor, dest, cstr, alloc);
    return dest;
}

}  // namespace diagnostics
}  // namespace sprout

#ifndef SPROUT_DEPENDENCIES_LIBC_INTERNAL_H
#define SPROUT_DEPENDENCIES_LIBC_INTERNAL_H

#include <cstdint>
#include <string>

#include <sprout/dependencies/dependency.h>

/* Shared between the libc_* modules only -- not part of the public dependency
 * surface. The printf/scanf grammar is implemented once here and reused by
 * stdio (printf/sprintf/snprintf/fprintf...), by liblog (__android_log_print
 * takes the same format string) and by the scanf family. */

namespace sprout {
namespace libc {

/* Where a variadic call's arguments come from.
 *
 * Two shapes exist and they are not interchangeable:
 *  - a direct variadic call (printf, sprintf): arguments follow AAPCS, so the
 *    first four words are r0..r3 and the rest are on the stack past sp.
 *  - a va_list call (vsnprintf, __android_log_vprint): on ARM EABI a va_list is
 *    just a pointer to a contiguous argument block, so everything is read from
 *    guest memory at increasing addresses.
 * Getting this wrong is silent -- the format still "works", it just prints the
 * wrong words -- so the distinction is explicit at every construction site. */
class VaSource {
public:
    /* first_arg is the AAPCS argument index the varargs start at: 1 for
     * printf(fmt, ...), 2 for sprintf(dst, fmt, ...), 3 for snprintf. */
    static VaSource from_registers(GuestCall &c, int first_arg);
    static VaSource from_va_list(GuestCall &c, std::uint32_t va_addr);

    std::uint32_t next_u32();
    std::uint64_t next_u64();  /* 8-byte aligned, per AAPCS */
    double next_double();      /* softfp: a double IS its 64-bit pattern */

private:
    GuestCall *c_ = nullptr;
    bool from_regs_ = false;
    int arg_index_ = 0;        /* register/stack mode */
    std::uint32_t va_addr_ = 0; /* va_list mode */
};

/* Runs a guest format string against a source of arguments, returning what
 * printf would have produced. Each conversion is handed to the HOST's snprintf
 * one specifier at a time (the spec text taken verbatim from the guest string,
 * restricted to a safe character set), so field width/precision/flags behave
 * exactly like the real thing without reimplementing them. */
std::string format(GuestCall &c, std::uint32_t fmt_addr, VaSource &args);

/* The scanf half: consumes `input` per the guest format string, storing through
 * the pointers `args` yields. Returns the number of fields successfully
 * converted (the scanf return value), or -1 for "input exhausted before the
 * first conversion" (EOF). */
int scan(GuestCall &c, const std::string &input, std::uint32_t fmt_addr, VaSource &args);

}  // namespace libc
}  // namespace sprout

#endif

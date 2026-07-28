/* libc.so -- <string.h> and the __aeabi_mem* helpers the ARM compiler emits.
 *
 * Everything works directly on the flat guest buffer. Where the operands are
 * known to be in bounds the host's own memcpy/memcmp is used (they are the
 * fastest thing available and these are on the hottest path in the whole
 * emulator); the byte-at-a-time loops are for the cases where the length is
 * discovered as we go, which cannot be delegated safely.
 */

#include <sprout/dependencies/dependency.h>

#include <cctype>
#include <cstring>
#include <string>

namespace sprout {
namespace {

/* ------------------------------------------------------------------ memory */

void c_memcpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    /* memmove semantics for both: the engine does overlap these in practice
     * (in-place vector shifts), and on a flat host buffer it costs nothing. */
    if (c.in_bounds(dst, n) && c.in_bounds(src, n)) {
        std::memmove(&c.img->mem[dst], &c.img->mem[src], n);
    }
    c.set_result(dst);
}

void c_memset(GuestCall &c) {
    std::uint32_t dst = c.arg(0), val = c.arg(1), n = c.arg(2);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], (int)val, n);
    c.set_result(dst);
}

/* __aeabi_memset(dst, n, val) -- note the argument order is NOT memset's. */
void c_aeabi_memset(GuestCall &c) {
    std::uint32_t dst = c.arg(0), n = c.arg(1), val = c.arg(2);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], (int)val, n);
}

void c_aeabi_memclr(GuestCall &c) {
    std::uint32_t dst = c.arg(0), n = c.arg(1);
    if (c.in_bounds(dst, n)) std::memset(&c.img->mem[dst], 0, n);
}

void c_memcmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1), n = c.arg(2);
    int r = (c.in_bounds(a, n) && c.in_bounds(b, n))
                ? std::memcmp(&c.img->mem[a], &c.img->mem[b], n)
                : 0;
    c.set_result((std::uint32_t)r);
}

void c_memchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), ch = c.arg(1) & 0xFFu, n = c.arg(2);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (c.read8(s + i) == ch) { c.set_result(s + i); return; }
    }
    c.set_result(0);
}

/* ----------------------------------------------------------------- strings */

std::uint32_t guest_strlen(GuestCall &c, std::uint32_t s) {
    std::uint32_t len = 0;
    while (c.in_bounds(s + len, 1) && c.img->mem[s + len] != 0) ++len;
    return len;
}

void c_strlen(GuestCall &c) { c.set_result(guest_strlen(c, c.arg(0))); }

void c_strcpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(src + i);
        c.write8(dst + i, ch);
        if (ch == 0) break;
    }
    c.set_result(dst);
}

void c_strncpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    bool ended = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint8_t ch = ended ? 0 : c.read8(src + i);
        if (ch == 0) ended = true; /* strncpy pads the whole tail with NULs */
        c.write8(dst + i, ch);
    }
    c.set_result(dst);
}

void append(GuestCall &c, std::uint32_t max_n) {
    std::uint32_t dst = c.arg(0), src = c.arg(1);
    std::uint32_t dlen = guest_strlen(c, dst);
    std::uint32_t i = 0;
    for (; i < max_n; ++i) {
        std::uint8_t ch = c.read8(src + i);
        if (ch == 0) break;
        c.write8(dst + dlen + i, ch);
    }
    c.write8(dst + dlen + i, 0);
    c.set_result(dst);
}

void c_strcat(GuestCall &c) { append(c, 0xFFFFFFFFu); }
void c_strncat(GuestCall &c) { append(c, c.arg(2)); }

void c_strcmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1);
    int result = 0;
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ca = c.read8(a + i), cb = c.read8(b + i);
        if (ca != cb) { result = (int)ca - (int)cb; break; }
        if (ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void c_strncmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1), n = c.arg(2);
    int result = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint8_t ca = c.read8(a + i), cb = c.read8(b + i);
        if (ca != cb) { result = (int)ca - (int)cb; break; }
        if (ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void casecmp(GuestCall &c, std::uint32_t n) {
    std::uint32_t a = c.arg(0), b = c.arg(1);
    int result = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        int la = std::tolower(c.read8(a + i)), lb = std::tolower(c.read8(b + i));
        if (la != lb) { result = la - lb; break; }
        if (la == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void c_strcasecmp(GuestCall &c) { casecmp(c, 0xFFFFFFFFu); }
void c_strncasecmp(GuestCall &c) { casecmp(c, c.arg(2)); }

void c_strchr(GuestCall &c) {
    std::uint32_t s = c.arg(0);
    std::uint8_t want = (std::uint8_t)c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(s + i);
        if (ch == want) { c.set_result(s + i); return; } /* a NUL search finds the terminator, per the standard */
        if (ch == 0) break;
    }
    c.set_result(0);
}

void c_strrchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), found = 0;
    std::uint8_t want = (std::uint8_t)c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint8_t ch = c.read8(s + i);
        if (ch == want) found = s + i;
        if (ch == 0) break;
    }
    c.set_result(found);
}

void c_strstr(GuestCall &c) {
    std::string hay = c.cstr(c.arg(0)), needle = c.cstr(c.arg(1), 256);
    std::size_t pos = hay.find(needle);
    c.set_result(pos == std::string::npos ? 0 : c.arg(0) + (std::uint32_t)pos);
}

void c_strdup(GuestCall &c) {
    c.set_result(c.dup_cstr(c.cstr(c.arg(0))));
}

/* char *strpbrk(const char *s, const char *accept) -- first character of s that
 * appears in accept, or NULL. */
void c_strpbrk(GuestCall &c) {
    const std::uint32_t s = c.arg(0);
    const std::string accept = c.cstr(c.arg(1), 256);
    for (std::uint32_t i = 0;; ++i) {
        const std::uint8_t ch = c.read8(s + i);
        if (ch == 0) break;
        if (accept.find((char)ch) != std::string::npos) {
            c.set_result(s + i);
            return;
        }
    }
    c.set_result(0);
}

/* size_t strlcpy(char *dst, const char *src, size_t size) -- the BSD one.
 *
 * Returns the length of SRC, not of what it copied: that is how the caller
 * detects truncation (result >= size), and returning the copied length instead
 * would make every truncation look like a success. Always NUL-terminates when
 * size is non-zero, which is the whole reason the function exists. */
void c_strlcpy(GuestCall &c) {
    const std::uint32_t dst = c.arg(0), size = c.arg(2);
    const std::string src = c.cstr(c.arg(1));
    if (dst != 0 && size > 0) {
        const std::uint32_t n = std::min<std::uint32_t>((std::uint32_t)src.size(), size - 1);
        for (std::uint32_t i = 0; i < n; ++i) c.write8(dst + i, (std::uint8_t)src[i]);
        c.write8(dst + n, 0);
    }
    c.set_result((std::uint32_t)src.size());
}

/* strtok keeps its cursor in per-thread storage, exactly like the real one. */
void c_strtok(GuestCall &c) {
    std::uint32_t str = c.arg(0), delim = c.arg(1);
    std::uint32_t pos = (str != 0) ? str : guest_tls::strtok_next;

    auto is_delim = [&](std::uint8_t ch) {
        for (std::uint32_t i = 0; c.read8(delim + i) != 0; ++i) {
            if (c.read8(delim + i) == ch) return true;
        }
        return false;
    };

    while (c.read8(pos) != 0 && is_delim(c.read8(pos))) ++pos;
    if (c.read8(pos) == 0) {
        guest_tls::strtok_next = pos;
        c.set_result(0);
        return;
    }
    std::uint32_t tok_start = pos;
    while (c.read8(pos) != 0 && !is_delim(c.read8(pos))) ++pos;
    if (c.read8(pos) != 0) {
        c.write8(pos, 0);
        ++pos;
    }
    guest_tls::strtok_next = pos;
    c.set_result(tok_start);
}

}  // namespace

void register_libc_string(ImportTable &t) {
    t.add("memcpy", c_memcpy);
    t.add("memmove", c_memcpy);
    t.add("memset", c_memset);
    t.add("memcmp", c_memcmp);
    t.add("memchr", c_memchr);

    /* Compiler-emitted helpers. The 4/8-suffixed forms differ only in the
     * alignment they promise, which is nothing to us -- registering them keeps
     * a differently-optimised build of the game working without changes. */
    t.add("__aeabi_memcpy", c_memcpy);
    t.add("__aeabi_memcpy4", c_memcpy);
    t.add("__aeabi_memcpy8", c_memcpy);
    t.add("__aeabi_memmove", c_memcpy);
    t.add("__aeabi_memmove4", c_memcpy);
    t.add("__aeabi_memmove8", c_memcpy);
    t.add("__aeabi_memset", c_aeabi_memset);
    t.add("__aeabi_memset4", c_aeabi_memset);
    t.add("__aeabi_memset8", c_aeabi_memset);
    t.add("__aeabi_memclr", c_aeabi_memclr);
    t.add("__aeabi_memclr4", c_aeabi_memclr);
    t.add("__aeabi_memclr8", c_aeabi_memclr);

    t.add("strlen", c_strlen);
    t.add("strcpy", c_strcpy);
    t.add("strncpy", c_strncpy);
    t.add("strcat", c_strcat);
    t.add("strncat", c_strncat);
    t.add("strcmp", c_strcmp);
    t.add("strncmp", c_strncmp);
    t.add("strcasecmp", c_strcasecmp);
    t.add("strncasecmp", c_strncasecmp);
    t.add("strchr", c_strchr);
    t.add("strrchr", c_strrchr);
    t.add("strstr", c_strstr);
    t.add("strpbrk", c_strpbrk);
    t.add("strdup", c_strdup);
    t.add("strlcpy", c_strlcpy);
    t.add("strtok", c_strtok);
}

}  // namespace sprout

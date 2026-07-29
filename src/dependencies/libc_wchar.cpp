/* libc.so -- <wchar.h> and <wctype.h>.
 *
 * On bionic/armeabi-v7a a wchar_t is **4 bytes**. The host toolchain (MinGW)
 * uses 2, so nothing here may ever delegate to the host's wcs* functions: every
 * routine below works in explicit 32-bit units read from guest memory.
 *
 * This is not a theoretical concern. The engine's resource lookup is built on
 * std::wstring, whose copy path goes through wmemcpy; while wmem* were missing
 * the dispatcher left r0 untouched and every wide string the engine assembled
 * came out as uninitialised garbage, which is what made the RSB name lookup for
 * "properties\resources.rton" miss and end in "Unable to read resource file".
 */

#include <sprout/dependencies/dependency.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace sprout {
namespace {

/* Reads a guest wide string into a narrow host one. Only ever used for values
 * that are ASCII by construction (numeric literals, resource keys); anything
 * outside becomes '?' so a surprise is visible rather than silent. */
std::string narrow(GuestCall &c, std::uint32_t s, std::size_t max = 256) {
    std::string out;
    for (std::size_t i = 0; i < max; ++i) {
        std::uint32_t ch = c.read32(s + (std::uint32_t)i * 4);
        if (ch == 0) break;
        out.push_back(ch < 128 ? (char)ch : '?');
    }
    return out;
}

/* ------------------------------------------------------- wide C strings */

void c_wcslen(GuestCall &c) {
    std::uint32_t s = c.arg(0), len = 0;
    while (c.read32(s + len * 4) != 0) ++len;
    c.set_result(len);
}

void c_wcscmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1);
    int result = 0;
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ca = c.read32(a + i * 4), cb = c.read32(b + i * 4);
        if (ca != cb) { result = ca < cb ? -1 : 1; break; }
        if (ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void c_wcsncmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1), n = c.arg(2);
    int result = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ca = c.read32(a + i * 4), cb = c.read32(b + i * 4);
        if (ca != cb) { result = ca < cb ? -1 : 1; break; }
        if (ca == 0) break;
    }
    c.set_result((std::uint32_t)result);
}

void c_wcschr(GuestCall &c) {
    std::uint32_t s = c.arg(0), want = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(s + i * 4);
        if (ch == want) { c.set_result(s + i * 4); return; }
        if (ch == 0) break;
    }
    c.set_result(0);
}

void c_wcsrchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), want = c.arg(1), found = 0;
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(s + i * 4);
        if (ch == want) found = s + i * 4;
        if (ch == 0) break;
    }
    c.set_result(found);
}

void c_wcscpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(src + i * 4);
        c.write32(dst + i * 4, ch);
        if (ch == 0) break;
    }
    c.set_result(dst);
}

void c_wcsncpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    bool ended = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ch = ended ? 0 : c.read32(src + i * 4);
        if (ch == 0) ended = true;
        c.write32(dst + i * 4, ch);
    }
    c.set_result(dst);
}

void c_wcscat(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), dlen = 0;
    while (c.read32(dst + dlen * 4) != 0) ++dlen;
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(src + i * 4);
        c.write32(dst + (dlen + i) * 4, ch);
        if (ch == 0) break;
    }
    c.set_result(dst);
}

void c_wcsspn(GuestCall &c) {
    std::uint32_t s = c.arg(0), accept = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(s + i * 4);
        if (ch == 0) { c.set_result(i); return; }
        bool ok = false;
        for (std::uint32_t j = 0;; ++j) {
            std::uint32_t a = c.read32(accept + j * 4);
            if (a == 0) break;
            if (a == ch) { ok = true; break; }
        }
        if (!ok) { c.set_result(i); return; }
    }
}

void c_wcscspn(GuestCall &c) {
    std::uint32_t s = c.arg(0), reject = c.arg(1);
    for (std::uint32_t i = 0;; ++i) {
        std::uint32_t ch = c.read32(s + i * 4);
        if (ch == 0) { c.set_result(i); return; }
        for (std::uint32_t j = 0;; ++j) {
            std::uint32_t r = c.read32(reject + j * 4);
            if (r == 0) break;
            if (r == ch) { c.set_result(i); return; }
        }
    }
}

void c_wcsstr(GuestCall &c) {
    std::uint32_t hay = c.arg(0), needle = c.arg(1);
    if (c.read32(needle) == 0) { c.set_result(hay); return; }
    for (std::uint32_t i = 0; c.read32(hay + i * 4) != 0; ++i) {
        std::uint32_t j = 0;
        for (;; ++j) {
            std::uint32_t n = c.read32(needle + j * 4);
            if (n == 0) { c.set_result(hay + i * 4); return; }
            if (c.read32(hay + (i + j) * 4) != n) break;
        }
    }
    c.set_result(0);
}

void c_wcstol(GuestCall &c) {
    std::uint32_t s = c.arg(0), endptr = c.arg(1);
    std::string text = narrow(c, s, 128);
    char *end = nullptr;
    long v = std::strtol(text.c_str(), &end, (int)c.arg(2));
    if (endptr != 0) {
        std::uint32_t consumed = end ? (std::uint32_t)(end - text.c_str()) : 0;
        c.write32(endptr, s + consumed * 4); /* wide: advance in 4-byte units */
    }
    c.set_result((std::uint32_t)v);
}

/* ------------------------------------------------- wide memory primitives
 *
 * Unlike the wcs* family these take a COUNT of wchars and never stop at a
 * null -- std::wstring's copy/assign path is built on wmemcpy. */

void c_wmemcpy(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2);
    /* memmove semantics: the guest may legitimately overlap, and copying via a
     * temporary costs nothing at these sizes. */
    std::vector<std::uint32_t> tmp(n);
    for (std::uint32_t i = 0; i < n; ++i) tmp[i] = c.read32(src + i * 4);
    for (std::uint32_t i = 0; i < n; ++i) c.write32(dst + i * 4, tmp[i]);
    c.set_result(dst);
}

void c_wmemset(GuestCall &c) {
    std::uint32_t dst = c.arg(0), ch = c.arg(1), n = c.arg(2);
    for (std::uint32_t i = 0; i < n; ++i) c.write32(dst + i * 4, ch);
    c.set_result(dst);
}

void c_wmemcmp(GuestCall &c) {
    std::uint32_t a = c.arg(0), b = c.arg(1), n = c.arg(2);
    int result = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t ca = c.read32(a + i * 4), cb = c.read32(b + i * 4);
        if (ca != cb) { result = ca < cb ? -1 : 1; break; }
    }
    c.set_result((std::uint32_t)result);
}

void c_wmemchr(GuestCall &c) {
    std::uint32_t s = c.arg(0), want = c.arg(1), n = c.arg(2);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (c.read32(s + i * 4) == want) { c.set_result(s + i * 4); return; }
    }
    c.set_result(0);
}

/* --------------------------------------------------- multibyte conversion
 *
 * The guest runs in the C locale, where the multibyte encoding is a plain
 * byte-per-character map. Only ASCII resource names actually flow through. */

void c_wcstombs(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2), i = 0;
    for (; i < n; ++i) {
        std::uint32_t ch = c.read32(src + i * 4);
        if (dst != 0) c.write8(dst + i, (std::uint8_t)(ch < 256 ? ch : '?'));
        if (ch == 0) break;
    }
    c.set_result(i);
}

void c_mbstowcs(GuestCall &c) {
    std::uint32_t dst = c.arg(0), src = c.arg(1), n = c.arg(2), i = 0;
    for (; i < n; ++i) {
        std::uint8_t b = c.read8(src + i);
        if (dst != 0) c.write32(dst + i * 4, b);
        if (b == 0) break;
    }
    c.set_result(i);
}

void c_wcrtomb(GuestCall &c) {
    std::uint32_t dst = c.arg(0), wc = c.arg(1);
    if (dst != 0) c.write8(dst, (std::uint8_t)(wc < 256 ? wc : '?'));
    c.set_result(1);
}

void c_mbrtowc(GuestCall &c) {
    std::uint32_t pwc = c.arg(0), s = c.arg(1), n = c.arg(2);
    if (s == 0 || n == 0) { c.set_result(0); return; }
    std::uint8_t b = c.read8(s);
    if (pwc != 0) c.write32(pwc, b);
    c.set_result(b == 0 ? 0u : 1u);
}

void c_btowc(GuestCall &c) {
    /* int -> wint_t: EOF stays WEOF, single bytes map identity. */
    c.set_result(c.arg(0) == 0xFFFFFFFFu ? 0xFFFFFFFFu : (c.arg(0) & 0xFFu));
}

void c_wctob(GuestCall &c) {
    c.set_result(c.arg(0) <= 0xFFu ? c.arg(0) : 0xFFFFFFFFu); /* non-representable -> EOF */
}

/* -------------------------------------------------------------- <wctype.h>
 *
 * Classification is ASCII-only, which is exact for everything the engine
 * classifies (resource paths, config keys, numeric text). */

int ascii(std::uint32_t wc) { return wc < 128 ? (int)wc : -1; }

void c_towlower(GuestCall &c) {
    std::uint32_t ch = c.arg(0);
    c.set_result((ch >= 'A' && ch <= 'Z') ? ch + 0x20 : ch);
}

void c_towupper(GuestCall &c) {
    std::uint32_t ch = c.arg(0);
    c.set_result((ch >= 'a' && ch <= 'z') ? ch - 0x20 : ch);
}

void c_iswspace(GuestCall &c) {
    std::uint32_t w = c.arg(0);
    c.set_result((w == ' ' || (w >= 0x09 && w <= 0x0D)) ? 1u : 0u);
}

void c_iswalnum(GuestCall &c) {
    int ch = ascii(c.arg(0));
    c.set_result(ch >= 0 && std::isalnum(ch) ? 1u : 0u);
}

void c_iswalpha(GuestCall &c) {
    int ch = ascii(c.arg(0));
    c.set_result(ch >= 0 && std::isalpha(ch) ? 1u : 0u);
}

void c_iswdigit(GuestCall &c) {
    int ch = ascii(c.arg(0));
    c.set_result(ch >= 0 && std::isdigit(ch) ? 1u : 0u);
}

/* wctype() hands back an opaque class id that only iswctype() consumes, so the
 * two just have to agree on the numbering. */
const char *const kClassNames[] = {"alnum", "alpha", "blank", "cntrl", "digit",  "graph",
                                   "lower", "print", "punct", "space", "upper", "xdigit"};
constexpr std::uint32_t kClassCount = sizeof(kClassNames) / sizeof(kClassNames[0]);

void c_wctype(GuestCall &c) {
    std::string want = c.cstr(c.arg(0), 32);
    for (std::uint32_t i = 0; i < kClassCount; ++i) {
        if (want == kClassNames[i]) { c.set_result(i + 1); return; }
    }
    c.set_result(0);
}

void c_iswctype(GuestCall &c) {
    int ch = ascii(c.arg(0));
    bool r = false;
    if (ch >= 0) {
        switch (c.arg(1)) {
            case 1:  r = std::isalnum(ch); break;
            case 2:  r = std::isalpha(ch); break;
            case 3:  r = ch == ' ' || ch == '\t'; break;
            case 4:  r = std::iscntrl(ch); break;
            case 5:  r = std::isdigit(ch); break;
            case 6:  r = std::isgraph(ch); break;
            case 7:  r = std::islower(ch); break;
            case 8:  r = std::isprint(ch); break;
            case 9:  r = std::ispunct(ch); break;
            case 10: r = std::isspace(ch); break;
            case 11: r = std::isupper(ch); break;
            case 12: r = std::isxdigit(ch); break;
            default: break;
        }
    }
    c.set_result(r ? 1u : 0u);
}

}  // namespace

void register_libc_wchar(ImportTable &t) {
    t.add("wcslen", c_wcslen);
    t.add("wcscmp", c_wcscmp);
    t.add("wcsncmp", c_wcsncmp);
    t.add("wcschr", c_wcschr);
    t.add("wcsrchr", c_wcsrchr);
    t.add("wcscpy", c_wcscpy);
    t.add("wcsncpy", c_wcsncpy);
    t.add("wcscat", c_wcscat);
    t.add("wcsspn", c_wcsspn);
    t.add("wcscspn", c_wcscspn);
    t.add("wcsstr", c_wcsstr);
    t.add("wcstol", c_wcstol);

    t.add("wmemcpy", c_wmemcpy);
    t.add("wmemmove", c_wmemcpy);
    t.add("wmemset", c_wmemset);
    t.add("wmemcmp", c_wmemcmp);
    t.add("wmemchr", c_wmemchr);

    t.add("wcstombs", c_wcstombs);
    t.add("mbstowcs", c_mbstowcs);
    t.add("wcrtomb", c_wcrtomb);
    t.add("mbrtowc", c_mbrtowc);
    t.add("btowc", c_btowc);
    t.add("wctob", c_wctob);

    t.add("towlower", c_towlower);
    t.add("towupper", c_towupper);
    t.add("iswspace", c_iswspace);
    t.add("iswalnum", c_iswalnum);
    t.add("iswalpha", c_iswalpha);
    t.add("iswdigit", c_iswdigit);
    t.add("wctype", c_wctype);
    t.add("iswctype", c_iswctype);
}

}  // namespace sprout

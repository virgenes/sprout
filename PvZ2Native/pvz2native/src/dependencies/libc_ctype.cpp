/* libc.so -- <ctype.h>.
 *
 * These exist as real functions on bionic as well as macros, and 4.5.2 calls
 * the functions where 1.6 only ever used the macro form (which reads the
 * `_ctype_` table directly -- see initialize_data_imports, and note that the
 * table and these entry points must agree).
 *
 * Deliberately answering for the C LOCALE ONLY, and passing the byte through
 * the host's own <cctype> would not do that: the host classifies according to
 * whatever locale it is in, so a machine running a non-ASCII default codepage
 * would classify bytes 0x80-0xFF differently from the Android device the game
 * was written for. The engine parses its own data files with these, so that
 * difference would be a parsing bug that only reproduces on some machines.
 *
 * The argument is an int holding either an unsigned char or EOF, so anything
 * outside 0..255 is simply "not any class".
 */

#include <sprout/dependencies/dependency.h>

namespace sprout {
namespace {

bool in_range(std::uint32_t ch, int lo, int hi) {
    const int c = (int)ch;
    return c >= lo && c <= hi;
}

bool c_is_digit(std::uint32_t c) { return in_range(c, '0', '9'); }
bool c_is_upper(std::uint32_t c) { return in_range(c, 'A', 'Z'); }
bool c_is_lower(std::uint32_t c) { return in_range(c, 'a', 'z'); }
bool c_is_alpha(std::uint32_t c) { return c_is_upper(c) || c_is_lower(c); }
bool c_is_alnum(std::uint32_t c) { return c_is_alpha(c) || c_is_digit(c); }
bool c_is_space(std::uint32_t c) {
    const int v = (int)c;
    return v == ' ' || (v >= '\t' && v <= '\r');
}
bool c_is_print(std::uint32_t c) { return in_range(c, 0x20, 0x7E); }
bool c_is_graph(std::uint32_t c) { return in_range(c, 0x21, 0x7E); }
bool c_is_punct(std::uint32_t c) { return c_is_graph(c) && !c_is_alnum(c); }
bool c_is_cntrl(std::uint32_t c) { return in_range(c, 0, 0x1F) || (int)c == 0x7F; }
bool c_is_xdigit(std::uint32_t c) {
    return c_is_digit(c) || in_range(c, 'a', 'f') || in_range(c, 'A', 'F');
}
bool c_is_blank(std::uint32_t c) { return (int)c == ' ' || (int)c == '\t'; }

#define CTYPE_PREDICATE(name, pred) \
    void name(GuestCall &c) { c.set_result(pred(c.arg(0)) ? 1u : 0u); }

CTYPE_PREDICATE(t_isalnum, c_is_alnum)
CTYPE_PREDICATE(t_isalpha, c_is_alpha)
CTYPE_PREDICATE(t_isblank, c_is_blank)
CTYPE_PREDICATE(t_iscntrl, c_is_cntrl)
CTYPE_PREDICATE(t_isdigit, c_is_digit)
CTYPE_PREDICATE(t_isgraph, c_is_graph)
CTYPE_PREDICATE(t_islower, c_is_lower)
CTYPE_PREDICATE(t_isprint, c_is_print)
CTYPE_PREDICATE(t_ispunct, c_is_punct)
CTYPE_PREDICATE(t_isspace, c_is_space)
CTYPE_PREDICATE(t_isupper, c_is_upper)
CTYPE_PREDICATE(t_isxdigit, c_is_xdigit)

#undef CTYPE_PREDICATE

/* Both return the argument UNCHANGED when it is not a letter -- including for
 * EOF and for bytes above 0x7F. Folding those would corrupt any non-ASCII text
 * passed through tolower() a byte at a time. */
void t_tolower(GuestCall &c) {
    const std::uint32_t ch = c.arg(0);
    c.set_result(c_is_upper(ch) ? ch + ('a' - 'A') : ch);
}

void t_toupper(GuestCall &c) {
    const std::uint32_t ch = c.arg(0);
    c.set_result(c_is_lower(ch) ? ch - ('a' - 'A') : ch);
}

}  // namespace

void register_libc_ctype(ImportTable &t) {
    t.add("isalnum", t_isalnum);
    t.add("isalpha", t_isalpha);
    t.add("isblank", t_isblank);
    t.add("iscntrl", t_iscntrl);
    t.add("isdigit", t_isdigit);
    t.add("isgraph", t_isgraph);
    t.add("islower", t_islower);
    t.add("isprint", t_isprint);
    t.add("ispunct", t_ispunct);
    t.add("isspace", t_isspace);
    t.add("isupper", t_isupper);
    t.add("isxdigit", t_isxdigit);
    t.add("tolower", t_tolower);
    t.add("toupper", t_toupper);
}

}  // namespace sprout

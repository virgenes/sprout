/* liblog.so -- Android's logging library.
 *
 * This is the engine's own voice. Everything it reports about resource loading,
 * shader compilation, save data and asset failures comes out through here, so
 * these are the most valuable few functions in the whole dependency set: a
 * missing argument in a format string is the difference between "RSB
 * Initialization failed for <path>" and "RSB Initialization failed for ".
 */

#include "libc_internal.h"

#include <mutex>
#include <string>

namespace sprout {
namespace {

using libc::VaSource;

/* android_LogPriority, as the guest passes it. */
const char *priority_name(std::uint32_t prio) {
    switch (prio) {
        case 2: return "V";
        case 3: return "D";
        case 4: return "I";
        case 5: return "W";
        case 6: return "E";
        case 7: return "F";
        default: return "?";
    }
}

/* Emits a guest log line, but COLLAPSES consecutive identical ones.
 *
 * This is not just tidiness -- it is load-bearing. Wwise's error handler prints
 * one line per failed PostEvent, and a game state where a sound cannot resolve
 * (an event whose bank never loaded, or a nameless event hashing to the FNV
 * basis 2166136261) floods thousands of identical lines a second. Crucially
 * those prints happen ON WWISE'S AUDIO THREAD while it drains its command queue:
 * every line takes the log lock and does a printf, so the thread spends its time
 * logging instead of draining. The queue then fills faster than it empties until
 * Wwise blocks the caller ("audio command queue is full") and the game hangs.
 * Deduping returns that time to the drain and keeps the log readable, while a
 * periodic "(repeated N times)" still shows a stuck flood is happening. */
void guest_log(GuestCall &c, const char *prio, const std::string &tag, const std::string &text) {
    static std::mutex lock;
    static std::string last;
    static std::uint64_t repeat = 0;

    std::string line = std::string("[guest ") + prio + "] " + tag + ": " + text;
    std::lock_guard<std::mutex> lk(lock);
    if (line == last) {
        /* Every 4096th repeat, show it is still going without printing each one. */
        if ((++repeat & 0xFFF) == 0)
            c.log("[guest] ... last line still repeating (%llu times)", (unsigned long long)repeat);
        return;
    }
    if (repeat > 0) {
        c.log("[guest] (previous line repeated %llu times)", (unsigned long long)repeat);
        repeat = 0;
    }
    last = line;
    c.log("%s", line.c_str());
}

/* int __android_log_write(int prio, const char *tag, const char *text) */
void log_write(GuestCall &c) {
    guest_log(c, priority_name(c.arg(0)), c.cstr(c.arg(1), 128), c.cstr(c.arg(2), 2048));
    c.set_result(1);
}

/* int __android_log_print(int prio, const char *tag, const char *fmt, ...) */
void log_print(GuestCall &c) {
    VaSource args = VaSource::from_registers(c, 3);
    std::string text = libc::format(c, c.arg(2), args);
    guest_log(c, priority_name(c.arg(0)), c.cstr(c.arg(1), 128), text);
    c.set_result(1);
}

/* int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list) */
void log_vprint(GuestCall &c) {
    VaSource args = VaSource::from_va_list(c, c.arg(3));
    std::string text = libc::format(c, c.arg(2), args);
    guest_log(c, priority_name(c.arg(0)), c.cstr(c.arg(1), 128), text);
    c.set_result(1);
}

/* void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...)
 *
 * This one never returns on a device -- it aborts the process. Halting is the
 * faithful behaviour AND the useful one: the assertion text names exactly what
 * the engine found wrong, and letting execution continue past it turns a clear
 * failure into corruption somewhere later. */
void log_assert(GuestCall &c) {
    std::string cond = c.cstr(c.arg(0), 256);
    std::string tag = c.cstr(c.arg(1), 128);
    std::string text;
    if (c.arg(2) != 0) {
        VaSource args = VaSource::from_registers(c, 3);
        text = libc::format(c, c.arg(2), args);
    }
    c.log("[guest ASSERT] %s: (%s) %s", tag.c_str(), cond.c_str(), text.c_str());
    c.halt("__android_log_assert");
}

/* Present in newer liblog; harmless to answer "yes, that level is on" so the
 * engine does not suppress its own messages. */
void log_is_loggable(GuestCall &c) { c.set_result(1); }

}  // namespace

void register_liblog(ImportTable &t) {
    t.add("__android_log_write", log_write);
    t.add("__android_log_print", log_print);
    t.add("__android_log_vprint", log_vprint);
    t.add("__android_log_assert", log_assert);
    t.add("__android_log_is_loggable", log_is_loggable);
}

}  // namespace sprout

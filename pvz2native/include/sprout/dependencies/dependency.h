#ifndef SPROUT_DEPENDENCIES_DEPENDENCY_H
#define SPROUT_DEPENDENCIES_DEPENDENCY_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

#include <sprout/elf32/elf32_loader.h>
#include <sprout/runtime/guest_runtime.h>

namespace sprout {

/* One emulated Android shared library per translation unit.
 *
 * libPVZ2.so declares these in DT_NEEDED -- libc.so, libm.so, libz.so,
 * liblog.so, libGLESv2.so, libGLESv1_CM.so, libOpenSLES.so, libstdc++.so and
 * libdl.so -- and the modules under src/dependencies/ mirror that list one for
 * one (libc, the largest at ~200 symbols, is split further by header: string,
 * wchar, stdio, stdlib, time, unistd, pthread, locale, misc).
 *
 * Every symbol the binary imports is implemented here, in a module named after
 * the library that really provides it. Nothing is dispatched from the harness
 * any more: pvz2_run_test.cpp looks a name up in import_table() and calls it.
 * That is the whole point of the layout -- coverage per library is visible at
 * a glance instead of buried in one long if-else chain, which is what let whole
 * families go missing unnoticed (libm was 24 of 25 symbols unimplemented, every
 * one silently returning 0).
 *
 * The ABI is ARM EABI **softfp**, as used by Android armeabi-v7a and confirmed
 * against real traces (glDepthRangef arrives with 1.0f as 0x3f800000 in a core
 * register): floating-point arguments and results travel in the integer
 * registers, never in VFP ones. Hence the f32/f64 accessors below rather than
 * raw register reads.
 */

/* Everything a handler is allowed to touch, so modules need to know nothing
 * about Pvz2Env or dynarmic. `regs` is r0..r15. */
struct GuestCall {
    pvz2_elf_image_t *img = nullptr;
    GuestRuntime *rt = nullptr;
    std::uint32_t *regs = nullptr;

    /* --- integer arguments / result (AAPCS: r0..r3, then the stack) --- */
    std::uint32_t arg(int i) const;
    std::uint32_t lr() const { return regs[14]; }
    std::uint32_t sp() const { return regs[13]; }
    void set_result(std::uint32_t v) { regs[0] = v; }
    void set_result64(std::uint64_t v) {
        regs[0] = (std::uint32_t)(v & 0xFFFFFFFFu);
        regs[1] = (std::uint32_t)(v >> 32);
    }

    /* --- softfp floating point --- */
    float argf(int i) const;                 /* one core register */
    double argd(int i) const;                /* an even-aligned register pair */
    void set_resultf(float v);
    void set_resultd(double v);

    /* --- guest memory --- */
    bool in_bounds(std::uint32_t addr, std::uint32_t size) const;
    std::uint8_t read8(std::uint32_t addr) const;
    std::uint16_t read16(std::uint32_t addr) const;
    std::uint32_t read32(std::uint32_t addr) const;
    void write8(std::uint32_t addr, std::uint8_t v);
    void write16(std::uint32_t addr, std::uint16_t v);
    void write32(std::uint32_t addr, std::uint32_t v);
    std::string cstr(std::uint32_t addr, std::size_t max = 4096) const;
    /* Copies a host string (with its terminator) into guest memory at addr. */
    void put_cstr(std::uint32_t addr, const std::string &s);
    /* Heap-allocates a copy of a host string in guest memory; returns 0 if the
     * heap is exhausted. */
    std::uint32_t dup_cstr(const std::string &s);
    /* Raw host pointer into guest memory, or nullptr if out of range. Use for
     * bulk operations; never hold it across a guest call. */
    void *ptr(std::uint32_t addr, std::uint32_t size = 1) const;

    /* --- host handles behind the guest's opaque FILE and fd tokens --- */
    std::FILE *file(std::uint32_t token) const;
    int fd(std::uint32_t token) const;

    /* --- reporting ---
     * Takes the runtime's log lock, so interleaved guest threads don't shred
     * each other's lines. Prefix is added automatically. */
    void log(const char *fmt, ...) const;

    /* --- operations that need the executing environment ---
     *
     * A handful of entry points cannot be expressed as a pure function of
     * memory and registers: exit/abort have to stop the JIT, pthread_create has
     * to spawn one, and qsort has to call back into guest code. They are reached
     * through these hooks so the dependency modules stay free of any dependency
     * on Pvz2Env or dynarmic. */
    void *env = nullptr;
    void (*halt_fn)(void *env, const char *why) = nullptr;
    std::uint32_t (*spawn_thread_fn)(void *env, std::uint32_t start, std::uint32_t arg) = nullptr;
    std::uint32_t (*join_thread_fn)(void *env, std::uint32_t id) = nullptr;
    /* Runs guest code to completion on a fresh JIT and returns r0. Used for the
     * callbacks libc is contractually required to invoke (qsort's comparator,
     * pthread_once's init routine). */
    std::uint32_t (*call_guest_fn)(void *env, std::uint32_t fn, const std::uint32_t *args, int nargs) = nullptr;
    /* Told about every heap allocation, so the harness can arm diagnostics on
     * an object the moment it is born (see kAppObjectSize in pvz2_run_test). */
    void (*on_alloc_fn)(void *env, std::uint32_t addr, std::uint32_t size) = nullptr;
    /* Called by a blocking sync primitive when the guest thread is about to
     * wait. It is the harness's cue that the thread is making progress rather
     * than spinning, so a long-lived worker (Wwise's audio threads) does not
     * trip the runaway tick budget meant to catch a genuinely stuck loop. */
    void (*note_blocked_fn)(void *env) = nullptr;

    /* Cleared by handlers that must NOT resume at the return address --
     * pthread_exit terminates the thread instead of returning to its caller. */
    bool returns = true;

    void halt(const char *why) { if (halt_fn) halt_fn(env, why); returns = false; }
    void no_return() { returns = false; }
    std::uint32_t spawn_thread(std::uint32_t start, std::uint32_t arg) {
        return spawn_thread_fn ? spawn_thread_fn(env, start, arg) : 0;
    }
    std::uint32_t join_thread(std::uint32_t id) {
        return join_thread_fn ? join_thread_fn(env, id) : 0;
    }
    std::uint32_t call_guest(std::uint32_t fn, const std::uint32_t *args, int nargs) {
        return call_guest_fn ? call_guest_fn(env, fn, args, nargs) : 0;
    }
    void on_alloc(std::uint32_t addr, std::uint32_t size) {
        if (on_alloc_fn) on_alloc_fn(env, addr, size);
    }
    /* Announce, from a blocking primitive, that this thread is about to wait --
     * see note_blocked_fn. Cheap and safe to call even when nothing is bound. */
    void note_blocked() {
        if (note_blocked_fn) note_blocked_fn(env);
    }

    /* errno is per-thread state; the slot is a small guest allocation made on
     * first use, so `__errno` can hand the guest a real address to write. */
    std::uint32_t errno_addr() const;
    void set_errno(std::uint32_t value);
};

using ImportHandler = void (*)(GuestCall &);

/* Mints a guest-callable stub that routes to `fn`, and returns its guest
 * address -- an address the guest can legitimately `BLX` to.
 *
 * Needed because OpenSL ES is a COM-style API: the guest reaches every method
 * through `(*obj)->Method(obj, ...)`, so the vtables have to live in guest
 * memory and their entries have to be real guest code addresses. This pairs
 * pvz2_elf_add_trampoline() (which writes the SVC instruction) with binding the
 * handler for that SVC index, so callers get both halves in one call.
 *
 * Returns 0 if the trampoline table is full. Call only during startup, before
 * guest threads are running: the handler table is sized once and then read
 * concurrently without a lock. */
std::uint32_t make_guest_callback(const char *name, ImportHandler fn);

/* Name -> handler, populated once at startup by the register_* functions. */
class ImportTable {
public:
    void add(const char *name, ImportHandler fn) { map_[name] = fn; }
    ImportHandler find(const char *name) const {
        auto it = map_.find(name);
        return it == map_.end() ? nullptr : it->second;
    }
    std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::string, ImportHandler> map_;
};

/* Per-call tracing of imports and JNI calls. On during boot, off once the game
 * reaches steady state -- a traced frame writes ~400KB, which is fine for a
 * fixed diagnostic run but not for a window the user leaves open. Shared by the
 * dependency and DEX layers so one switch covers both. */
namespace trace {
void set(bool on);
bool enabled();
}  // namespace trace

/* Per-guest-thread state that libc genuinely owns. Each guest pthread maps 1:1
 * onto a host thread, so real host thread_local storage gives these exactly the
 * semantics the C standard asks for. */
namespace guest_tls {
extern thread_local std::uint32_t self_id;      /* pthread_self */
extern thread_local std::uint32_t strtok_next;  /* strtok's implicit cursor */
extern thread_local std::uint32_t errno_slot;   /* guest address of errno */
std::unordered_map<std::uint32_t, std::uint32_t> &values(); /* pthread_{get,set}specific */
}  // namespace guest_tls

/* One registrar per emulated library; each lives in its own .cpp under
 * src/dependencies/ and registers only its own symbols. */
void register_libc(ImportTable &t);      /* aggregates the libc_* modules below */
void register_libc_string(ImportTable &t);
void register_libc_wchar(ImportTable &t);
void register_libc_stdio(ImportTable &t);
void register_libc_stdlib(ImportTable &t);
void register_libc_time(ImportTable &t);
void register_libc_unistd(ImportTable &t);
void register_libc_pthread(ImportTable &t);
void register_libc_locale(ImportTable &t);
void register_libc_ctype(ImportTable &t);
/* <sys/socket.h>. New in 4.5.2 (Crashlytics/analytics) and deliberately a
 * refusal rather than an implementation -- see the file's header comment. */
void register_libc_socket(ImportTable &t);
void register_libc_misc(ImportTable &t);
void register_libm(ImportTable &t);
void register_libz(ImportTable &t);
void register_liblog(ImportTable &t);
void register_libgles(ImportTable &t);

/* PVZ2_GL_STRICT=1: check the GL error queue after every gl* entry point, so
 * the call that actually produced an error is named. Without it errors surface
 * at whatever draw happens next, which points the investigation at innocent
 * code -- exactly what happened with glDrawArrays and GL_INVALID_OPERATION.
 * Returns false when the mode is off, so the caller can skip the work. */
bool gl_strict_enabled();
void gl_check_error_after(GuestCall &c, const char *name);

/* The real drawable size, which the GL layer needs for exactly one thing: the
 * composite-to-screen pass arrives with a zero-width viewport, and a viewport
 * of width 0 turns every following draw into a silent no-op. See
 * gl_glViewport in libgles.cpp. */
void set_drawable_size(std::uint32_t width, std::uint32_t height);
void register_libstdcxx(ImportTable &t);
void register_libopensles(ImportTable &t);
void register_libdl(ImportTable &t);

/* --- OpenSL ES (src/dependencies/libopensles.cpp) --- */

/* Gives one imported SL_IID_* data symbol a distinct, non-zero value, so
 * GetInterface can tell the five interfaces apart (a zeroed slot would make
 * them all compare equal). Returns false if the name is not an SL_IID_*.
 * Called from initialize_data_imports(). */
bool initialize_sl_interface_id(pvz2_elf_image_t *img, const char *name, std::uint32_t addr);

/* Delivers one guest buffer-queue callback per buffer that finished playing.
 * Must be called from the frame loop, between top-level guest calls -- SDL's
 * audio thread must never run guest code. */
void audio_pump_callbacks();

/* Calls a guest function at an ABSOLUTE guest address with two arguments and
 * returns r0. Host side only, and only BETWEEN top-level guest calls: it
 * re-arms the shared main-thread context, so calling it while that context is
 * mid-call would clobber the call in progress. */
std::uint32_t call_guest_between_frames(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1);

/* Variadic: calls a guest function with an array of arguments and returns r0.
 * Same constraints as call_guest_between_frames. */
namespace runtime {
std::uint32_t call_guest_between_frames_n(std::uint32_t fn, const std::uint32_t args[], int nargs);
}  // namespace runtime

/* Fills in the imported STT_OBJECT symbols (ctype tables, stack canary, stdio
 * handles) that the loader gave real guest memory, and registers the three
 * __sF streams as file tokens. Call once after load, before any guest code. */
void initialize_data_imports(pvz2_elf_image_t *img, GuestRuntime *rt);

/* Builds the table on first use. */
const ImportTable &import_table();

}  // namespace sprout

#endif

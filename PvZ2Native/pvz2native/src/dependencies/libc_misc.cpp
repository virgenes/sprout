/* libc.so -- errno, non-local jumps, process introspection, the stack guard.
 * The leftovers that belong to no single header. */

#include <sprout/dependencies/dependency.h>

#include <cstring>
#include <string>

namespace sprout {
namespace {

/* ---------------------------------------------------------------- errno
 *
 * bionic's errno is `(*__errno())`, a per-thread int. The slot is a small guest
 * allocation made on first use, so the guest gets a real address to read and
 * write like any other. */
void c_errno(GuestCall &c) {
    c.set_result(c.errno_addr());
}

void c_strerror(GuestCall &c) {
    /* Returns a pointer to a static string, which must live in GUEST memory.
     * Only the handful of codes this port can actually produce are named. */
    static std::uint32_t slot = 0;
    if (slot == 0) slot = c.rt->heap.alloc(64);
    const char *msg = "Unknown error";
    switch (c.arg(0)) {
        case 2:   msg = "No such file or directory"; break;
        case 9:   msg = "Bad file descriptor"; break;
        case 11:  msg = "Try again"; break;
        case 12:  msg = "Out of memory"; break;
        case 13:  msg = "Permission denied"; break;
        case 16:  msg = "Device or resource busy"; break;
        case 22:  msg = "Invalid argument"; break;
        case 110: msg = "Connection timed out"; break;
        default: break;
    }
    c.put_cstr(slot, msg);
    c.set_result(slot);
}

/* ------------------------------------------------------------ setjmp/longjmp
 *
 * Real control flow, not a stub: the engine calls setjmp, and a longjmp that
 * did nothing would silently continue down the failing path instead of
 * unwinding. Both halves are ours, so the jmp_buf layout is ours too --
 * bionic's is `long[64]`, far more room than the callee-saved set needs.
 *
 * The dispatcher performs `pc = lr` after every handler returns, so longjmp
 * redirects execution by writing its target into lr rather than pc. */
constexpr std::uint32_t kJmpBufMagic = 0x4A4D5042; /* "JMPB" */

void c_setjmp(GuestCall &c) {
    std::uint32_t env = c.arg(0);
    if (env != 0 && c.in_bounds(env, 48)) {
        c.write32(env, kJmpBufMagic);
        for (int i = 0; i < 8; ++i) c.write32(env + 4 + (std::uint32_t)i * 4, c.regs[4 + i]); /* r4..r11 */
        c.write32(env + 36, c.regs[13]); /* sp */
        c.write32(env + 40, c.regs[14]); /* lr */
    }
    c.set_result(0); /* the direct call always returns 0 */
}

void c_longjmp(GuestCall &c) {
    std::uint32_t env = c.arg(0), val = c.arg(1);
    if (env == 0 || !c.in_bounds(env, 48) || c.read32(env) != kJmpBufMagic) {
        c.log("[libc] longjmp with an uninitialised jmp_buf (0x%08x) -- ignoring", env);
        c.set_result(val ? val : 1u);
        return;
    }
    for (int i = 0; i < 8; ++i) c.regs[4 + i] = c.read32(env + 4 + (std::uint32_t)i * 4);
    c.regs[13] = c.read32(env + 36);
    c.regs[14] = c.read32(env + 40); /* the dispatcher jumps here */
    c.set_result(val ? val : 1u);    /* setjmp must never appear to return 0 */
}

/* ------------------------------------------------------------ stack guard */

void c_stack_chk_fail(GuestCall &c) {
    c.log("__stack_chk_fail() -- the guest detected stack smashing at lr=0x%08x", c.lr());
    c.halt("__stack_chk_fail");
}

/* -------------------------------------------------------- process / system */

void c_sysconf(GuestCall &c) {
    /* bionic's _SC_* numbering. */
    switch (c.arg(0)) {
        case 39: c.set_result(4096); return; /* _SC_PAGESIZE          */
        case 97: c.set_result(4); return;    /* _SC_NPROCESSORS_ONLN  */
        case 96: c.set_result(4); return;    /* _SC_NPROCESSORS_CONF  */
        default: c.set_result(0); return;
    }
}

void c_getpagesize(GuestCall &c) { c.set_result(4096); }
void c_getpid(GuestCall &c) { c.set_result(1); }
void c_gettid(GuestCall &c) { c.set_result(guest_tls::self_id); }

/* A raw syscall cannot be serviced: the guest is asking the Linux kernel
 * directly, and there is none. Report the number so an unexpected one is
 * visible rather than a mystery -ENOSYS deep in the engine. */
void c_syscall(GuestCall &c) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        c.log("[libc] raw syscall(%u) -- returning -ENOSYS (lr=0x%08x)", c.arg(0), c.lr());
    }
    c.set_result((std::uint32_t)-38 /* -ENOSYS */);
}

/* prctl is used for PR_SET_NAME (thread naming) and little else; accepting it
 * is exactly right. ptrace is the anti-debug check -- reporting "no tracer" is
 * both true and what the engine wants to hear. */
void c_prctl(GuestCall &c) { c.set_result(0); }
void c_ptrace(GuestCall &c) { c.set_result((std::uint32_t)-1); }

void c_raise(GuestCall &c) {
    c.log("guest called raise(%u)", c.arg(0));
    c.halt("raise");
}

/* --- process control, new in 4.5.2 -----------------------------------------
 *
 * Crashlytics ships in 4.5.2 and this is its toolkit: fork a helper, exec a
 * reporter, wait for it. There is one emulated process here and no way to
 * create a second -- a guest "child" would need its own address space, and it
 * would immediately try to talk to a crash server this port has no network for
 * (see libc_socket.cpp).
 *
 * So they fail, with the errno that says the system will not make another
 * process. That is a state real Android reaches under memory pressure, so the
 * caller's error path is one the library was written to survive -- unlike
 * fork() returning 0, which would tell the guest it IS the child and send it
 * down a path that never returns. */
void c_fork(GuestCall &c) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        c.log("[libc] fork() -- refused, this port is a single emulated process (lr=0x%08x)",
              c.lr());
    }
    c.set_errno(11 /* EAGAIN */);
    c.set_result((std::uint32_t)-1);
}

void c_execv(GuestCall &c) {
    c.set_errno(2 /* ENOENT */);
    c.set_result((std::uint32_t)-1);
}

/* system(cmd) returns -1 for "could not run a shell"; system(NULL) asks whether
 * one EXISTS, and the answer to that is 0 for no. */
void c_system(GuestCall &c) { c.set_result((std::uint32_t)-1); }

/* ECHILD: there are no children to wait for, which follows from fork failing. */
void c_waitpid(GuestCall &c) {
    c.set_errno(10 /* ECHILD */);
    c.set_result((std::uint32_t)-1);
}

/* getpid() already answers 1, so the parent is the traditional init. */
void c_getppid(GuestCall &c) { c.set_result(1); }

/* sigaction(sig, act, oact): accepted and ignored.
 *
 * Deliberately reported as SUCCESS with a zeroed `oact`. The guest cannot
 * receive a real signal -- it is JIT-executed code inside our process, and a
 * host fault never becomes a guest one -- so an installed handler would never
 * run either way. 4.5.2's JNI_OnLoad installs one per signal in a loop and
 * stores the old action; failing there would be a startup error path taken for
 * no reason. */
void c_sigaction(GuestCall &c) {
    const std::uint32_t oact = c.arg(2);
    if (oact != 0) {
        for (std::uint32_t i = 0; i < 16; i += 4) c.write32(oact + i, 0);
    }
    c.set_result(0);
}

/* void __assert2(const char *file, int line, const char *func, const char *msg)
 *
 * bionic's assert(). Reaching it means the guest has already decided its own
 * state is impossible, so the one useful thing is to print WHAT it was before
 * stopping -- an abort with no message here would be indistinguishable from
 * any other halt. */
void c_assert2(GuestCall &c) {
    c.log("[guest assert] %s:%u: %s: %s", c.cstr(c.arg(0), 256).c_str(), c.arg(1),
          c.cstr(c.arg(2), 128).c_str(), c.cstr(c.arg(3), 256).c_str());
    c.halt("__assert2");
}

/* int __aeabi_atexit(void *obj, void (*dtor)(void*), void *dso_handle)
 *
 * The ARM EABI spelling of __cxa_atexit, which libc_stdlib already accepts and
 * ignores: static destructors run at process exit, and this process exits by
 * ending. Registering them would mean running guest code after the JIT and heap
 * are gone. */
void c_aeabi_atexit(GuestCall &c) { c.set_result(0); }

/* ---------------------------------------------------------- memory mapping
 *
 * mmap and friends are what Wwise's stream manager uses to load sound banks
 * and streaming audio. Without them every bank load fails, PostEvent can never
 * find the requested event (returning the empty-string hash 2166136261 =
 * 0x811C9DC5), and the flood of error messages on Wwise's audio thread
 * combined with the tick budget eventually halts the process.
 *
 * Real mmap would need a file-backed mapping, which the VFS does not support.
 * Simulating it by allocating guest heap and reading the file is doable, but
 * a stub that returns MAP_FAILED is already better than the silent zero the
 * missing-import path returns (which looks like a valid pointer to bionic's
 * caller, since MAP_FAILED is -1, not 0). */

void c_mmap(GuestCall &c) {
    /* void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) */
    std::uint32_t addr = c.arg(0), len = c.arg(1);
    std::int32_t prot = (std::int32_t)c.arg(2), flags = (std::int32_t)c.arg(3);
    std::int32_t fdesc = (std::int32_t)c.arg(4);
    std::uint32_t off = c.arg(5);

    /* MAP_ANONYMOUS (0x20) or MAP_PRIVATE|MAP_ANONYMOUS: allocate from the guest heap */
    if ((flags & 0x20) || fdesc == -1) {
        /* Round up to page boundary (0x1000) */
        if (len == 0) { c.set_result((std::uint32_t)-1); return; }
        std::uint32_t alloc_len = (len + 0xFFF) & ~0xFFFU;
        std::uint32_t result = c.rt->heap.alloc(alloc_len);
        if (result != 0) {
            void *p = c.ptr(result, alloc_len);
            if (p) std::memset(p, 0, alloc_len);
            c.log("[libc] mmap(anon, len=%u) -> 0x%08x (heap alloc)", len, result);
            c.set_result(result);
            return;
        }
    }

    c.log("[libc] mmap(addr=0x%08x, len=%u, prot=%d, flags=%d, fd=%d, off=%u) -> "
          "MAP_FAILED (unhandled, lr=0x%08x)",
          addr, len, prot, flags, fdesc, off, c.lr());
    c.set_result((std::uint32_t)-1); /* MAP_FAILED */
}

void c_munmap(GuestCall &c) {
    /* int munmap(void *addr, size_t length) */
    c.set_result(0); /* pretend success: no real mappings to tear down */
}

void c_mprotect(GuestCall &c) {
    /* int mprotect(void *addr, size_t len, int prot) */
    c.set_result(0); /* pretend success: all guest memory is already RWX */
}

}  // namespace

void register_libc_misc(ImportTable &t) {
    t.add("__errno", c_errno);
    t.add("__errno_location", c_errno);
    t.add("strerror", c_strerror);

    t.add("setjmp", c_setjmp);
    t.add("_setjmp", c_setjmp);
    t.add("longjmp", c_longjmp);
    t.add("_longjmp", c_longjmp);

    t.add("__stack_chk_fail", c_stack_chk_fail);

    t.add("sysconf", c_sysconf);
    t.add("getpagesize", c_getpagesize);
    t.add("getpid", c_getpid);
    t.add("gettid", c_gettid);
    t.add("syscall", c_syscall);
    t.add("prctl", c_prctl);
    t.add("ptrace", c_ptrace);
    t.add("raise", c_raise);

    t.add("fork", c_fork);
    t.add("execv", c_execv);
    t.add("system", c_system);
    t.add("waitpid", c_waitpid);
    t.add("getppid", c_getppid);
    t.add("sigaction", c_sigaction);

    t.add("__assert2", c_assert2);
    t.add("__aeabi_atexit", c_aeabi_atexit);

    t.add("mmap", c_mmap);
    t.add("mmap64", c_mmap);
    t.add("munmap", c_munmap);
    t.add("mprotect", c_mprotect);
}

}  // namespace sprout

/* The emulator: dynarmic's configuration, one JIT per guest thread, and the
 * primitives that enter guest code. See runtime/dynarmic_config.h for why this
 * layer holds no address into libPVZ2.so.
 */

#include <sprout/runtime/dynarmic_config.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <sprout/config.h>
#include <sprout/dex/dex.h>

namespace sprout {
namespace runtime {
namespace {

namespace dex = sprout::dex;
namespace guest_tls = sprout::guest_tls;

/* Mutable window size, defaults to the constants but overridable. */
std::uint32_t g_window_width = kWindowWidth;
std::uint32_t g_window_height = kWindowHeight;

/* Ticks per Run() slice while non-zero -- see set_slice_override. */
std::atomic<std::uint32_t> g_slice_override{0};

/* Host callback run between JIT slices on the main thread. */
std::atomic<pvz2_host_pump_fn> g_host_pump{nullptr};

std::mutex g_status_lock;
char g_status[160] = "starting";

/* Memory watchpoint state -- see watch_arm. */
std::uint32_t g_watch_lo = 0;
std::uint32_t g_watch_hi = 0;
std::uint32_t g_watch_budget = 0;

/* Imported-symbol handlers, cached by trampoline id.
 *
 * Every import arrives as "SVC #idx", and idx is stable for the life of the
 * process -- but the dispatcher used to re-resolve the NAME through a string
 * hash map on every single call. That is affordable at a few thousand calls and
 * absurd at the real volume: ResourceManager::Init issues millions of
 * __aeabi_memset calls (the compiler emits one per struct field), each of which
 * was re-hashing the same fourteen characters before doing four bytes of work.
 *
 * Sized once, before any guest code runs, and never resized: guest threads
 * index it concurrently, so growing it under them would be a data race on the
 * buffer itself. */
std::vector<ImportHandler> g_import_handlers;

pvz2_elf_image_t *g_image = nullptr;
GuestRuntime *g_runtime = nullptr;

CallStats g_last_call_stats;

ImportHandler resolve_import(std::uint32_t idx) {
    return idx < g_import_handlers.size() ? g_import_handlers[idx] : nullptr;
}

/* --- direct guest memory, without a host call per access -------------------
 *
 * Left unconfigured, dynarmic reaches guest memory only through the
 * UserCallbacks MemoryRead/MemoryWrite entry points -- a devirtualized host
 * call for every single LDR/STR the guest executes. Since img->mem is one flat
 * buffer covering the whole address space, an identity page table lets the JIT
 * inline those accesses as `page_table[vaddr >> 12][vaddr & 0xFFF]` instead,
 * and the callbacks then only serve what the table leaves unmapped (everything
 * at or above mem_size), which is exactly the range in_bounds() rejects.
 *
 * Entries are only installed for whole pages inside mem_size; the loader
 * allocates PVZ2_MEM_GUARD_SLACK past it so an unaligned wide access in the
 * final page stays inside the allocation. */
using Pvz2PageTable = std::array<std::uint8_t *, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
std::unique_ptr<Pvz2PageTable> g_page_table;

std::unique_ptr<GuestThreadCtx> g_main_ctx;

Dynarmic::A32::UserConfig make_arm_user_config(pvz2_elf_image_t *img, GuestRuntime *rt,
                                               Pvz2Env *env, size_t processor_id) {
    Dynarmic::A32::UserConfig config;
    config.callbacks = env;
    config.global_monitor = &rt->monitor;
    config.processor_id = processor_id;
    /* Inline guest memory accesses -- see build_page_table(). Null (the config
     * opt-out) simply leaves every access on the callback path. */
    config.page_table = g_page_table.get();
    /* Flat guest memory: generate `mov reg, [fastmem_base + vaddr]` directly
     * instead of going through the page-table indirection.  Accesses to
     * unmapped high addresses page-fault and fall back to the page table, then
     * to the MemoryRead/MemoryWrite callbacks if that also misses. */
    config.fastmem_pointer = reinterpret_cast<uintptr_t>(img->mem);
    /* Safe for a game that doesn't read NZCV in interrupt handlers or rely on
     * global-monitor accuracy across threads. */
    config.unsafe_optimizations = true;
    /* Tick counting already uses wall-clock time, not cycle-accurate
     * simulation, so tell dynarmic to skip the extra CNTPCT bookkeeping. */
    config.wall_clock_cntpct = true;
    return config;
}

}  // namespace

/* --- small shared services -------------------------------------------------- */

void set_slice_override(std::uint32_t ticks) {
    g_slice_override.store(ticks, std::memory_order_relaxed);
}

void set_host_pump(pvz2_host_pump_fn fn) { g_host_pump.store(fn, std::memory_order_relaxed); }

void set_status(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_status_lock);
    std::memcpy(g_status, buf, sizeof(buf));
}

void copy_status(char *out, size_t n) {
    if (out == nullptr || n == 0) return;
    std::lock_guard<std::mutex> lk(g_status_lock);
    std::snprintf(out, n, "%s", g_status);
}

bool verbose_boot() { return pvz2_config()->verbose != 0; }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void watch_arm(std::uint32_t lo, std::uint32_t hi, std::uint32_t budget) {
    g_watch_lo = lo;
    g_watch_hi = hi;
    g_watch_budget = budget;
    /* The page table bypasses the write callbacks entirely, so the target's
     * pages have to go back on the slow path or nothing is ever reported. */
    for (std::uint32_t p = lo; p < hi; p += 1u << Dynarmic::A32::UserConfig::PAGE_BITS) {
        unmap_page_for_watch(p);
    }
    if (hi > lo) unmap_page_for_watch(hi - 1);
}

void watch_disarm() {
    g_watch_lo = g_watch_hi = 0;
    g_watch_budget = 0;
}

void build_page_table(pvz2_elf_image_t *img) {
    /* The memory watchpoint only works through the callbacks, so keep a way to
     * turn the table off wholesale. */
    if (pvz2_config()->no_page_table) {
        std::printf("pvz2: [runtime] no_page_table set -- guest memory goes through the slow "
                    "callback path\n");
        return;
    }
    constexpr std::uint32_t kPageSize = 1u << Dynarmic::A32::UserConfig::PAGE_BITS;
    g_page_table = std::make_unique<Pvz2PageTable>();
    g_page_table->fill(nullptr);
    std::uint32_t mapped = 0;
    for (std::uint64_t addr = 0; addr + kPageSize <= img->mem_size; addr += kPageSize) {
        (*g_page_table)[addr >> Dynarmic::A32::UserConfig::PAGE_BITS] = img->mem + addr;
        ++mapped;
    }
    std::printf("pvz2: guest memory mapped directly into the JIT (%u pages, %u MB)\n", mapped,
                (unsigned)(img->mem_size >> 20));
}

void unmap_page_for_watch(std::uint32_t vaddr) {
    if (!g_page_table) return;
    (*g_page_table)[vaddr >> Dynarmic::A32::UserConfig::PAGE_BITS] = nullptr;
}

void build_import_handler_cache(pvz2_elf_image_t *img) {
    const ImportTable &table = import_table();
    g_import_handlers.assign(img->trampoline_capacity, nullptr);
    std::uint32_t resolved = 0;
    for (std::uint32_t i = 0; i < img->trampoline_count; ++i) {
        g_import_handlers[i] = table.find(img->trampoline_names[i]);
        if (g_import_handlers[i] != nullptr) ++resolved;
    }
    std::printf("pvz2: %u/%u imported symbols bound to a dependency module\n", resolved,
                img->trampoline_count);
}

void set_session_image(pvz2_elf_image_t *img, GuestRuntime *rt) {
    g_image = img;
    g_runtime = rt;
}

HeapLayout heap_layout_for(const pvz2_elf_image_t *img) {
    /* Kept as a floor purely so 1.6 lands on exactly the address it always
     * has: its image ends at 0x00EF9700, which rounds up to below this. */
    constexpr std::uint32_t kFloor = 0x01000000;
    constexpr std::uint32_t kGranularity = 0x00100000; /* 1MB */

    const std::uint32_t image_end = img->so_base + img->so_span;
    std::uint32_t base = (image_end + kGranularity - 1) & ~(kGranularity - 1);
    if (base < kFloor) base = kFloor;

    if (base >= kThreadStackBase) {
        std::printf("pvz2: [runtime] image ends at 0x%08x, past the thread stacks at 0x%08x -- "
                    "no room for a guest heap\n", image_end, kThreadStackBase);
        return {base, 0};
    }

    HeapLayout layout{base, kThreadStackBase - base};
    std::printf("pvz2: [runtime] image 0x%08x..0x%08x, guest heap 0x%08x..0x%08x (%u MB)\n",
                img->so_base, image_end, layout.base, kThreadStackBase, layout.size >> 20);
    return layout;
}

/* --- Pvz2Env ---------------------------------------------------------------- */

void Pvz2Env::begin_call() {
    ticks_used = 0;
    import_calls = 0;
    jni_calls = 0;
    halted_by_guest_return = false;
    exited_early = false;
    exit_retval = 0;
    should_halt = false;
    hot_lo = hot_hi = 0;
    hot_hits = 0;
    hot_seen_lrs.clear();
}

void Pvz2Env::note_watch(std::uint32_t vaddr, std::uint32_t size, std::uint32_t value) {
    if (g_watch_lo == g_watch_hi || g_watch_budget == 0) return;
    if (vaddr + size <= g_watch_lo || vaddr >= g_watch_hi) return;
    --g_watch_budget;
    auto &regs = jit->Regs();
    std::lock_guard<std::mutex> lg(rt->log_lock);
    std::printf("pvz2: [watch] write%u 0x%08x = 0x%08x (%d)  pc=0x%08x lr=0x%08x "
                "r0=0x%08x r1=0x%08x r4=0x%08x\n",
                size * 8, vaddr, value, (int)value, regs[15], regs[14], regs[0], regs[1], regs[4]);
}

std::uint8_t Pvz2Env::MemoryRead8(std::uint32_t vaddr) {
    return in_bounds(vaddr, 1) ? img->mem[vaddr] : 0;
}
std::uint16_t Pvz2Env::MemoryRead16(std::uint32_t vaddr) {
    std::uint16_t v = 0;
    if (in_bounds(vaddr, 2)) std::memcpy(&v, &img->mem[vaddr], 2);
    return v;
}
std::uint32_t Pvz2Env::MemoryRead32(std::uint32_t vaddr) {
    std::uint32_t v = 0;
    if (in_bounds(vaddr, 4)) std::memcpy(&v, &img->mem[vaddr], 4);
    return v;
}
std::uint64_t Pvz2Env::MemoryRead64(std::uint32_t vaddr) {
    std::uint64_t v = 0;
    if (in_bounds(vaddr, 8)) std::memcpy(&v, &img->mem[vaddr], 8);
    return v;
}
void Pvz2Env::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    note_watch(vaddr, 1, value);
    if (in_bounds(vaddr, 1)) img->mem[vaddr] = value;
}
void Pvz2Env::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    note_watch(vaddr, 2, value);
    if (in_bounds(vaddr, 2)) std::memcpy(&img->mem[vaddr], &value, 2);
}
void Pvz2Env::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    note_watch(vaddr, 4, value);
    if (in_bounds(vaddr, 4)) std::memcpy(&img->mem[vaddr], &value, 4);
}
void Pvz2Env::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    note_watch(vaddr, 8, (std::uint32_t)value);
    if (in_bounds(vaddr, 8)) std::memcpy(&img->mem[vaddr], &value, 8);
}

bool Pvz2Env::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t) {
    MemoryWrite8(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t) {
    MemoryWrite16(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t) {
    MemoryWrite32(vaddr, value);
    return true;
}
bool Pvz2Env::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t) {
    MemoryWrite64(vaddr, value);
    return true;
}

void Pvz2Env::InterpreterFallback(std::uint32_t pc, size_t num_instructions) {
    {
        std::lock_guard<std::mutex> lg(rt->log_lock);
        std::printf("pvz2: InterpreterFallback at pc=0x%08x (%zu instr) -- halting\n", pc,
                    num_instructions);
    }
    should_halt = true;
    jit->HaltExecution();
}

GuestCall Pvz2Env::make_guest_call() {
    GuestCall call;
    call.img = img;
    call.rt = rt;
    call.regs = jit->Regs().data();
    call.env = this;
    call.halt_fn = &Pvz2Env::hook_halt;
    call.note_blocked_fn = &Pvz2Env::hook_note_blocked;
    call.spawn_thread_fn = &Pvz2Env::hook_spawn_thread;
    call.join_thread_fn = &Pvz2Env::hook_join_thread;
    call.call_guest_fn = &Pvz2Env::hook_call_guest;
    return call;
}

/* A blocking sync primitive reports here that the thread is about to wait.
 *
 * The runaway tick budget (kTickBudget) exists to catch a guest loop that never
 * yields; for the MAIN thread it is bounded per top-level call by begin_call().
 * A spawned worker, though, runs one entry point for the whole session and never
 * calls begin_call, so its ticks_used only ever grows -- and Wwise's audio
 * threads legitimately execute billions of instructions over a few minutes, so
 * they hit the budget and got HALTED mid-session. That killed the thread that
 * posts the audio semaphores, and the game deadlocked (tid=1/tid=7 stuck in
 * sem_wait) -- the real cause behind "freezes after a long while".
 *
 * Blocking on a semaphore or condition is proof the thread is making progress,
 * not spinning, so reset its budget here. A genuine spinlock never blocks and
 * still trips the cap. Only spawned threads need it; the main thread keeps its
 * per-call accounting so the heartbeat's tick figures stay honest. */
void Pvz2Env::hook_note_blocked(void *env) {
    if (guest_tls::self_id == 1) return; /* main thread: begin_call owns its budget */
    static_cast<Pvz2Env *>(env)->ticks_used = 0;
}

void Pvz2Env::hook_halt(void *env, const char *why) {
    auto *self = static_cast<Pvz2Env *>(env);
    /* pthread_exit is the one halt that carries a value back to whoever joins
     * this thread, and the only one after which resuming at the return address
     * would be wrong. */
    if (std::strcmp(why, "pthread_exit") == 0) {
        self->exited_early = true;
        self->exit_retval = self->jit->Regs()[0];
    }
    self->should_halt = true;
    self->jit->HaltExecution();
}

std::uint32_t Pvz2Env::hook_spawn_thread(void *env, std::uint32_t start, std::uint32_t arg) {
    return spawn_guest_thread_impl(static_cast<Pvz2Env *>(env)->rt, start, arg);
}

std::uint32_t Pvz2Env::hook_join_thread(void *env, std::uint32_t id) {
    return join_guest_thread_impl(static_cast<Pvz2Env *>(env)->rt, id);
}

void Pvz2Env::return_to_caller() {
    auto &regs = jit->Regs();
    const std::uint32_t lr = regs[14];
    regs[15] = lr & ~1u;
    if ((lr & 1u) != 0) jit->SetCpsr(jit->Cpsr() | 0x20u); /* resume in Thumb */
}

void Pvz2Env::handle_import_call(std::uint32_t idx) {
    auto &regs = jit->Regs();
    const char *name = (idx < img->trampoline_count) ? img->trampoline_names[idx] : "<unknown>";
    ++import_calls;
    if (trace::enabled()) {
        if (import_calls == kTraceCallLimit) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            trace::set(false);
            std::printf("pvz2: reached the trace limit (%u calls in this guest call) -- tracing "
                        "off, execution continues\n", kTraceCallLimit);
            std::fflush(stdout);
        } else {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: import call #%u -> %s(r0=0x%08x, r1=0x%08x, r2=0x%08x, r3=0x%08x) "
                        "lr=0x%08x\n",
                        import_calls, name, regs[0], regs[1], regs[2], regs[3], regs[14]);
        }
    }

    /* No import-call cap: import_calls is kept purely as a diagnostic counter.
     * A legitimate boot issues hundreds of millions of import calls (per-field
     * __aeabi_memset in ResourceManager::Init, the 65536-iteration font-metrics
     * sweep, etc.), and long-lived worker threads run indefinitely, so any fixed
     * ceiling here is a guaranteed false positive rather than a runaway guard. */

    if (ImportHandler fn = resolve_import(idx)) {
        GuestCall call = make_guest_call();
        fn(call);
        /* See gl_strict_enabled(): names the GL call that produced an error,
         * instead of letting it surface at the next draw. */
        if (gl_strict_enabled() && name[0] == 'g' && name[1] == 'l') {
            gl_check_error_after(call, name);
        }
        /* A handler that terminates the thread (pthread_exit) or redirects it
         * (longjmp writes its target into lr) must not be followed by the
         * ordinary return-to-caller. */
        if (call.returns) return_to_caller();
        return;
    }

    /* No implementation: return 0 and resume as if the call trivially
     * succeeded -- but SAY SO, once per distinct name.
     *
     * This silence has been the single most expensive bug class in the project.
     * An unimplemented import used to fall through the whole dispatch chain
     * leaving r0 untouched, so the guest read garbage and the symptom always
     * surfaced far away, looking like corrupt engine data rather than a missing
     * shim. wmemcpy, NewStringUTF, CallLongMethodV and Resources_GetAssetFileSize
     * were each found only after a long hunt; every one would have been a single
     * line in a dependency module. */
    {
        static std::mutex seen_lock;
        static std::set<std::string> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(seen_lock);
            first = seen.insert(name).second;
        }
        if (first) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: [MISSING IMPORT] %s -- unimplemented, returning 0 (lr=0x%08x)\n",
                        name, regs[14]);
            std::fflush(stdout);
        }
    }
    regs[0] = 0;
    return_to_caller();
}

void Pvz2Env::CallSVC(std::uint32_t swi) {
    if (swi == 0) {
        if (verbose_boot()) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: guest function returned to host (halt sentinel)\n");
        }
        halted_by_guest_return = true;
        should_halt = true;
        jit->HaltExecution();
        return;
    }

    /* JNI and JavaVM slots belong to the DEX layer (src/dex/), which owns the
     * fake JNIEnv it installed at startup. */
    if (dex::owns_svc(swi)) {
        ++jni_calls;
        GuestCall call = make_guest_call();
        dex::dispatch_svc(call, swi);
        if (call.returns) return_to_caller();
        return;
    }

    handle_import_call(swi);
}

void Pvz2Env::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    auto &regs = jit->Regs();
    {
        std::lock_guard<std::mutex> lg(rt->log_lock);
        std::printf("pvz2: exception %d raised at pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x "
                    "r2=0x%08x r3=0x%08x -- halting\n",
                    static_cast<int>(exception), pc, regs[14], regs[0], regs[1], regs[2], regs[3]);
    }
    should_halt = true;
    jit->HaltExecution();
}

void Pvz2Env::AddTicks(std::uint64_t ticks) {
    ticks_used += ticks;
    if (ticks_used >= kTickBudget) {
        {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: tick budget exhausted (%llu), halting\n",
                        (unsigned long long)ticks_used);
        }
        should_halt = true;
        jit->HaltExecution();
    }
}

std::uint64_t Pvz2Env::GetTicksRemaining() {
    if (ticks_used >= kTickBudget) return 0;
    std::uint64_t remaining = kTickBudget - ticks_used;
    if (std::uint32_t slice = g_slice_override.load(std::memory_order_relaxed)) {
        return std::min<std::uint64_t>(remaining, slice);
    }
    if (hot_lo != hot_hi && hot_hits < kHotHitCap) {
        std::uint32_t pc = jit->Regs()[15];
        if (pc >= hot_lo && pc < hot_hi) return std::min<std::uint64_t>(remaining, 16);
    }
    return std::min<std::uint64_t>(remaining, kPcSampleSlice);
}

/* --- contexts and call setup ------------------------------------------------ */

GuestThreadCtx::GuestThreadCtx(pvz2_elf_image_t *img, GuestRuntime *rt, size_t processor_id)
    : jit(make_arm_user_config(img, rt, &env, processor_id)) {
    env.img = img;
    env.rt = rt;
    env.jit = &jit;
}

GuestThreadCtx &main_ctx(pvz2_elf_image_t *img, GuestRuntime *rt) {
    if (!g_main_ctx) g_main_ctx = std::make_unique<GuestThreadCtx>(img, rt, 0);
    g_main_ctx->env.begin_call();
    return *g_main_ctx;
}

void release_main_ctx() { g_main_ctx.reset(); }

void apply_call_setup(Dynarmic::A32::Jit &jit, Pvz2Env &env, pvz2_elf_image_t *img,
                      const GuestCallSetup &setup) {
    env.hot_lo = setup.hot_lo;
    env.hot_hi = setup.hot_hi;
    for (size_t i = 0; i < setup.stack_args.size(); ++i) {
        std::uint32_t val = setup.stack_args[i];
        std::memcpy(&img->mem[setup.sp + (std::uint32_t)i * 4], &val, 4);
    }
    auto &regs = jit.Regs();
    regs[0] = setup.r0;
    regs[1] = setup.r1;
    regs[2] = setup.r2;
    regs[3] = setup.r3;
    regs[13] = setup.sp;
    regs[14] = setup.lr;

    /* ARM/Thumb interworking, which every entry into guest code has to honour.
     *
     * A function POINTER carries the instruction set in its low bit: odd means
     * Thumb, and the real entry address is the value with that bit cleared.
     * Forcing ARM mode here regardless -- which is what this did -- makes the
     * JIT decode Thumb halfwords as 32-bit ARM words. That garbage decodes into
     * arbitrary instructions, and the process dies inside dynarmic's translator
     * on `assertion failed: reg != A32::Reg::PC` (a32_ir_emitter.cpp) when one
     * of them happens to name r15 as a destination -- a message that says
     * nothing about the actual cause.
     *
     * 1.6 hid this completely: every one of its 619 .init_array entries is ARM.
     * 4.5.2 has exactly ONE Thumb entry out of 1030, at index 1006, so it ran
     * 1005 constructors and then aborted. It applies to every entry point, not
     * just constructors -- a Thumb qsort comparator or OpenSL callback would
     * have failed the same way.
     *
     * Interworking WITHIN guest code needs nothing from us: BX/BLX carry the
     * bit themselves and dynarmic already tracks the T flag across them. */
    std::uint32_t entry_pc = setup.entry_pc;
    std::uint32_t cpsr = setup.cpsr;
    constexpr std::uint32_t kCpsrThumbBit = 0x20;
    if ((entry_pc & 1u) != 0) {
        entry_pc &= ~1u;
        cpsr |= kCpsrThumbBit;
    } else {
        cpsr &= ~kCpsrThumbBit;
    }
    regs[15] = entry_pc;
    jit.SetCpsr(cpsr);
}

void run_nested(Dynarmic::A32::Jit &jit, Pvz2Env &env) {
    while (!env.should_halt) jit.Run();
}

Dynarmic::HaltReason run_jit_sliced(Dynarmic::A32::Jit &jit, Pvz2Env &env) {
    Dynarmic::HaltReason hr{};
    for (;;) {
        hr = jit.Run();
        /* guest_tls::self_id is 1 on the thread driving the synchronous
         * top-level calls and 2+ inside spawned guest threads, so this keeps
         * the pump on the window's own thread. The callback rate-limits itself. */
        if (guest_tls::self_id == 1) {
            if (pvz2_host_pump_fn pump = g_host_pump.load(std::memory_order_relaxed)) pump();
        }
        if (env.should_halt) break; /* covers every real stop condition */
        if (!pvz2_config()->pc_sample) continue;
        std::uint32_t pc = jit.Regs()[15];
        if (env.hot_lo != env.hot_hi && pc >= env.hot_lo && pc < env.hot_hi) {
            env.hot_hits++;
            bool new_lr = env.hot_seen_lrs.insert(jit.Regs()[14]).second;
            if (env.hot_hits <= 40 || new_lr) {
                std::lock_guard<std::mutex> lg(env.rt->log_lock);
                std::printf("pvz2: [HOT #%u%s] tid=%u pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x "
                            "r4=0x%08x sp=0x%08x\n",
                            env.hot_hits, new_lr ? " NEW-LR" : "", guest_tls::self_id, pc,
                            jit.Regs()[14], jit.Regs()[0], jit.Regs()[1], jit.Regs()[4],
                            jit.Regs()[13]);
            } else if (env.hot_hits == env.kHotHitCap) {
                std::lock_guard<std::mutex> lg(env.rt->log_lock);
                std::printf("pvz2: [HOT] cap reached (%u hits, %zu distinct LRs) -- reverting to "
                            "coarse sampling\n", env.hot_hits, env.hot_seen_lrs.size());
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lg(env.rt->log_lock);
            std::printf("pvz2: [pc-sample] tid=%u ticks=%llu pc=0x%08x lr=0x%08x sp=0x%08x\n",
                        guest_tls::self_id, (unsigned long long)env.ticks_used, jit.Regs()[15],
                        jit.Regs()[14], jit.Regs()[13]);
        }
    }
    return hr;
}

/* --- nested calls and threads ----------------------------------------------- */

std::uint32_t Pvz2Env::hook_call_guest(void *env, std::uint32_t fn, const std::uint32_t *args,
                                       int nargs) {
    auto *outer = static_cast<Pvz2Env *>(env);
    GuestRuntime *rt = outer->rt;
    if (fn == 0) return 0;

    constexpr std::uint32_t kNestedStackSize = 128 * 1024;
    static thread_local std::uint32_t nested_stack_top = 0;
    if (nested_stack_top == 0) {
        std::uint32_t base = rt->heap.alloc(kNestedStackSize);
        if (base == 0) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: [call-guest] no room for a nested stack -- callback 0x%08x "
                        "skipped\n", fn);
            return 0;
        }
        nested_stack_top = (base + kNestedStackSize) & ~7u;
    }

    GuestCallSetup setup;
    if (nargs > 0) setup.r0 = args[0];
    if (nargs > 1) setup.r1 = args[1];
    if (nargs > 2) setup.r2 = args[2];
    if (nargs > 3) setup.r3 = args[3];
    for (int i = 4; i < nargs; ++i) setup.stack_args.push_back(args[i]);
    setup.sp = nested_stack_top;
    setup.lr = outer->img->trampoline_base; /* LR -> the "$halt" sentinel */
    setup.entry_pc = fn;

    /* A qsort comparator is called once per comparison, and building a Jit per
     * call meant reserving a 128MB code cache and re-translating the comparator
     * every single time. One reusable nested Jit per guest thread translates it
     * once. A callback that itself makes another guest callback would re-enter
     * this Jit while it is mid-Run(), so that (rare) case still gets a
     * throwaway one. */
    static thread_local std::unique_ptr<GuestThreadCtx> nested_ctx;
    static thread_local int nested_depth = 0;

    if (nested_depth == 0) {
        if (!nested_ctx) {
            nested_ctx = std::make_unique<GuestThreadCtx>(outer->img, rt, guest_tls::self_id);
        }
        Pvz2Env &inner = nested_ctx->env;
        inner.begin_call();
        apply_call_setup(nested_ctx->jit, inner, outer->img, setup);

        ++nested_depth;
        run_nested(nested_ctx->jit, inner);
        --nested_depth;

        /* The nested call's ticks are the outer call's ticks: it is the same
         * guest thread doing the work, and the budget bounds total execution. */
        outer->ticks_used += inner.ticks_used;
        return nested_ctx->jit.Regs()[0];
    }

    GuestThreadCtx deep(outer->img, rt, guest_tls::self_id);
    apply_call_setup(deep.jit, deep.env, outer->img, setup);

    ++nested_depth;
    run_nested(deep.jit, deep.env);
    --nested_depth;

    outer->ticks_used += deep.env.ticks_used;
    return deep.jit.Regs()[0];
}

std::uint32_t Pvz2Env::spawn_guest_thread_impl(GuestRuntime *rt, std::uint32_t start_routine,
                                               std::uint32_t arg) {
    std::uint32_t stack_top;
    std::uint32_t id;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        if (rt->next_stack_slot >= kThreadStackMax) {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: out of guest thread stack slots (max %u)\n", kThreadStackMax);
            return 0;
        }
        stack_top = kThreadStackBase + (rt->next_stack_slot + 1) * kThreadStackSize;
        rt->next_stack_slot++;
        id = rt->next_thread_id++;
    }

    std::thread th([rt, start_routine, arg, stack_top, id]() {
        guest_tls::self_id = id;
        /* One Jit for the whole life of the thread -- it only ever runs this one
         * entry point, but hook_call_guest may reuse the context too. */
        GuestThreadCtx ctx(rt->img, rt, id);
        Pvz2Env &env = ctx.env;
        Dynarmic::A32::Jit &jit = ctx.jit;
        auto &regs = jit.Regs();

        GuestCallSetup setup;
        setup.r0 = arg;
        setup.sp = stack_top;
        setup.lr = rt->img->trampoline_base; /* LR -> "$halt" sentinel */
        setup.entry_pc = start_routine;
        apply_call_setup(jit, env, rt->img, setup);

        {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: [thread %u] starting at 0x%08x (arg=0x%08x)\n", id, start_routine,
                        arg);
        }
        /* Same slice trap as the nested callbacks: one Run() lasts a tick
         * slice, not a whole thread. This thread used to stop dead after
         * kPcSampleSlice ticks and report itself "finished" -- which is what a
         * worker that issues its read() and then never writes a completion
         * status looks like from the outside. */
        run_nested(jit, env);
        std::uint32_t retval = env.exited_early ? env.exit_retval : regs[0];
        {
            std::lock_guard<std::mutex> lg(rt->log_lock);
            std::printf("pvz2: [thread %u] finished, retval=0x%08x, import_calls=%u, "
                        "jni_calls=%u\n", id, retval, env.import_calls, env.jni_calls);
        }

        std::lock_guard<std::mutex> lock(rt->threads_lock);
        rt->thread_retvals[id] = retval;
    });

    std::lock_guard<std::mutex> lock(rt->threads_lock);
    rt->threads[id] = std::move(th);
    return id;
}

std::uint32_t Pvz2Env::join_guest_thread_impl(GuestRuntime *rt, std::uint32_t id) {
    std::thread th;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        auto it = rt->threads.find(id);
        if (it == rt->threads.end()) return 0;
        th = std::move(it->second);
        rt->threads.erase(it);
    }
    if (th.joinable()) th.join();

    std::lock_guard<std::mutex> lock(rt->threads_lock);
    auto it = rt->thread_retvals.find(id);
    return it == rt->thread_retvals.end() ? 0 : it->second;
}

void join_leftover_guest_threads(GuestRuntime *rt) {
    std::vector<std::thread> leftover;
    {
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        for (auto &kv : rt->threads) leftover.push_back(std::move(kv.second));
        rt->threads.clear();
    }
    for (auto &th : leftover) {
        if (th.joinable()) th.join();
    }
}

/* --- entering guest code ---------------------------------------------------- */

std::uint32_t make_fake_jstring(pvz2_elf_image_t *img, std::uint32_t addr, const char *utf8) {
    size_t len = std::strlen(utf8) + 1;
    std::memcpy(&img->mem[addr], utf8, len);
    return addr;
}

/* The C++ static/global constructors Android's linker calls during dlopen(),
 * before JNI_OnLoad or anything else touches the .so. pvz2_elf_load() only
 * loads and relocates; it never executes guest code, so skipping this left
 * every non-trivial global C++ object as raw zeroed memory instead of properly
 * constructed -- root cause of the AndroidAppEvent list in onDrawFrame spinning
 * forever copying from a never-self-initialised sentinel. Called with
 * argc=argv=envp=0 like a minimal libc startup would. */
void run_init_array(pvz2_elf_image_t *img, GuestRuntime *rt) {
    if (img->init_array_count == 0) {
        std::printf("pvz2: no .init_array entries, skipping\n");
        return;
    }
    std::printf("pvz2: ---- running %u .init_array constructor(s) ----\n", img->init_array_count);
    for (std::uint32_t i = 0; i < img->init_array_count; ++i) {
        std::uint32_t entry_addr = 0;
        std::memcpy(&entry_addr, &img->mem[img->so_base + img->init_array_vaddr + i * 4], 4);
        if (entry_addr == 0 || entry_addr == 0xFFFFFFFFu) continue; /* some toolchains pad with -1 */
        set_status("booting: static constructors %u/%u", i + 1, img->init_array_count);

        GuestThreadCtx &ctx = main_ctx(img, rt);
        GuestCallSetup setup;
        setup.r0 = 0; /* argc */
        setup.r1 = 0; /* argv */
        setup.r2 = 0; /* envp */
        setup.sp = kStackTop;
        setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel */
        setup.entry_pc = entry_addr;
        apply_call_setup(ctx.jit, ctx.env, img, setup);

        Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
        if (verbose_boot()) {
            std::printf("pvz2: .init_array[%u] (0x%08x) returned, halt_reason=0x%08x, "
                        "import_calls=%u\n", i, entry_addr, static_cast<unsigned>(hr),
                        ctx.env.import_calls);
        }
    }
}

/* JNI_OnLoad(JavaVM*, void* reserved) -- a real exported symbol, but with a
 * DIFFERENT calling convention than every Java_* export (r0 is a JavaVM*, not a
 * JNIEnv*; there is no jclass in r1). Android calls this automatically right
 * after System.loadLibrary(), before any other native call. Skipping it left
 * the JavaVM and method-ID cache it allocates NULL for the whole process,
 * silently breaking every later call that re-derives a JNIEnv* via
 * JavaVM->GetEnv() instead of using the one handed to it. */
void run_jni_onload(pvz2_elf_image_t *img, GuestRuntime *rt) {
    std::uint32_t entry_addr = pvz2_elf_find_symbol(img, "JNI_OnLoad");
    if (entry_addr == 0) {
        std::printf("pvz2: symbol 'JNI_OnLoad' not found, skipping\n");
        return;
    }
    std::printf("pvz2: ---- running JNI_OnLoad at 0x%08x ----\n", entry_addr);

    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = dex::kJavaVmPtrAddr; /* JavaVM* vm */
    setup.r1 = 0;                   /* void* reserved */
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base;
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup);

    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    std::printf("pvz2: JNI_OnLoad returned, halt_reason=0x%08x, import_calls=%u, jni_calls=%u, "
                "ticks_used=%llu\n",
                static_cast<unsigned>(hr), ctx.env.import_calls, ctx.env.jni_calls,
                (unsigned long long)ctx.env.ticks_used);
}

void run_export(pvz2_elf_image_t *img, GuestRuntime *rt, const char *entry_name,
                std::uint32_t arg2, std::uint32_t arg3) {
    std::uint32_t entry_addr = pvz2_elf_find_symbol(img, entry_name);
    if (entry_addr == 0) {
        std::printf("pvz2: symbol '%s' not found, skipping\n", entry_name);
        return;
    }
    std::printf("pvz2: ---- running %s at 0x%08x ----\n", entry_name, entry_addr);

    /* Synchronous top-level calls never overlap, so they share one Jit -- and
     * with it every block it has already translated. */
    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = dex::kJniEnvPtrAddr; /* JNIEnv* env */
    setup.r1 = 0x00000200;          /* jclass clazz (dummy placeholder) */
    setup.r2 = arg2;
    setup.r3 = arg3;
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel (trampoline #0) */
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup); /* ARM mode, supervisor, IRQ/FIQ masked */

    auto t0 = std::chrono::steady_clock::now();
    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    std::printf("pvz2: %s returned, halt_reason=0x%08x, import_calls=%u, jni_calls=%u, "
                "ticks_used=%llu, ms=%.1f\n",
                entry_name, static_cast<unsigned>(hr), ctx.env.import_calls, ctx.env.jni_calls,
                (unsigned long long)ctx.env.ticks_used, ms_since(t0));
}

void run_guest_call(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                    std::uint32_t offset, std::uint32_t r0,
                    const std::vector<std::uint32_t> &extra_args, bool per_frame) {
    std::uint32_t entry_addr = img->so_base + offset;
    const bool chatty = verbose_boot() || !per_frame;
    if (chatty) {
        std::printf("pvz2: ---- running %s at 0x%08x (offset 0x%x) ----\n", label, entry_addr,
                    offset);
    }
    set_status("%s", label);

    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = r0;
    if (extra_args.size() > 0) setup.r1 = extra_args[0];
    if (extra_args.size() > 1) setup.r2 = extra_args[1];
    if (extra_args.size() > 2) setup.r3 = extra_args[2];
    if (extra_args.size() > 3) setup.stack_args.assign(extra_args.begin() + 3, extra_args.end());
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base;
    setup.entry_pc = entry_addr;
    apply_call_setup(ctx.jit, ctx.env, img, setup);

    auto t0 = std::chrono::steady_clock::now();
    Dynarmic::HaltReason hr = run_jit_sliced(ctx.jit, ctx.env);
    const double ms = ms_since(t0);
    g_last_call_stats = {ctx.env.import_calls, ctx.env.jni_calls, ctx.env.ticks_used, ms};
    if (chatty) {
        std::printf("pvz2: %s returned, halt_reason=0x%08x, import_calls=%u, jni_calls=%u, "
                    "ticks_used=%llu, ms=%.1f\n",
                    label, static_cast<unsigned>(hr), ctx.env.import_calls, ctx.env.jni_calls,
                    (unsigned long long)ctx.env.ticks_used, ms);
    }
}

const CallStats &last_call_stats() { return g_last_call_stats; }

void run_at_offset(pvz2_elf_image_t *img, GuestRuntime *rt, const char *label,
                   std::uint32_t offset, const std::vector<std::uint32_t> &extra_args,
                   bool per_frame) {
    run_guest_call(img, rt, label, offset, dex::kJniEnvPtrAddr, extra_args, per_frame);
}

std::uint32_t call_guest_quiet(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t offset,
                               std::uint32_t r0, std::uint32_t r1, std::uint32_t r2) {
    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;
    setup.r0 = r0;
    setup.r1 = r1;
    setup.r2 = r2;
    setup.sp = kStackTop;
    setup.lr = img->trampoline_base; /* LR -> "$halt" sentinel */
    setup.entry_pc = img->so_base + offset;
    apply_call_setup(ctx.jit, ctx.env, img, setup);
    run_nested(ctx.jit, ctx.env);
    return ctx.jit.Regs()[0];
}

void set_window_size(std::uint32_t width, std::uint32_t height) {
    g_window_width = width;
    g_window_height = height;
}
std::uint32_t window_width() { return g_window_width; }
std::uint32_t window_height() { return g_window_height; }

std::uint32_t call_guest_between_frames_n(std::uint32_t fn, const std::uint32_t args[], int nargs) {
    pvz2_elf_image_t *img = g_image;
    GuestRuntime *rt = g_runtime;
    if (img == nullptr || rt == nullptr || fn < img->so_base) return 0;
    if (nargs <= 0) return 0;

    const std::uint32_t offset = fn - img->so_base;
    GuestThreadCtx &ctx = main_ctx(img, rt);
    GuestCallSetup setup;

    setup.r0 = (nargs > 0) ? args[0] : 0;
    setup.r1 = (nargs > 1) ? args[1] : 0;
    setup.r2 = (nargs > 2) ? args[2] : 0;
    setup.r3 = (nargs > 3) ? args[3] : 0;

    if (nargs > 4) {
        setup.stack_args.assign(args + 4, args + nargs);
    }

    setup.sp = kStackTop;
    setup.lr = img->trampoline_base;
    setup.entry_pc = img->so_base + offset;
    apply_call_setup(ctx.jit, ctx.env, img, setup);
    run_nested(ctx.jit, ctx.env);
    return ctx.jit.Regs()[0];
}

}  // namespace runtime

/* --- the two hooks src/dependencies/ reaches back through -------------------- */

/* See dependency.h. Mints the SVC stub and binds its handler in one step, so a
 * caller building an OpenSL vtable gets back an address it can store straight
 * into guest memory. Startup only -- the handler table is never resized once
 * guest threads are indexing it. */
std::uint32_t make_guest_callback(const char *name, ImportHandler fn) {
    pvz2_elf_image_t *img = runtime::g_image;
    if (img == nullptr || fn == nullptr) return 0;
    const std::uint32_t addr = pvz2_elf_add_trampoline(img, name);
    if (addr == 0) return 0;
    const std::uint32_t idx = (addr - img->trampoline_base) / 4;
    if (idx >= runtime::g_import_handlers.size()) return 0;
    runtime::g_import_handlers[idx] = fn;
    return addr;
}

/* See dependency.h. call_guest_quiet() takes an offset and re-adds so_base, so
 * an absolute address goes in as (fn - so_base). */
std::uint32_t call_guest_between_frames(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1) {
    pvz2_elf_image_t *img = runtime::g_image;
    GuestRuntime *rt = runtime::g_runtime;
    if (img == nullptr || rt == nullptr || fn < img->so_base) return 0;
    return runtime::call_guest_quiet(img, rt, fn - img->so_base, a0, a1);
}

}  // namespace sprout

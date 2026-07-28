/* com.popcap.SexyAppFramework.AndroidHttpTransaction
 *
 * The engine's HTTP client, and the reason 4.5.2 never finishes loading.
 *
 * On a device this class is asynchronous: the engine builds a transaction with a
 * pointer to its C++ side (the `nativeTransaction` long), calls Start(), and a
 * Java thread connects, reads the response, and fires one of the engine's native
 * callbacks -- HttpReceivedResponse / HttpReceivedData / HttpTransactionComplete,
 * or, on any IOException, HttpTransactionError. THAT callback is how the engine
 * learns the request is over. With no network the connect() throws after the
 * 30-second timeout and the error path runs, so the engine proceeds offline.
 *
 * This port has no network and its Start() did nothing, so the transaction
 * neither completed nor errored -- it just hung. The [jni] census caught the
 * consequence: 10 transactions started, then the loading screen polling
 * Config_ConfigKeyExists and GetNetworkStatus every frame forever, waiting on a
 * download that can never finish.
 *
 * The fix reproduces the post-timeout device behaviour: fail every transaction
 * immediately by calling the engine's own HttpTransactionError native. That is
 * not a heavy call -- decompiled, it builds a small failure message and enqueues
 * it on the very ring buffer PumpMessageQueue drains (sub_CB9F80), so the engine
 * processes the failure on the main thread next frame and moves on. Its address
 * is per-version (GameSymbols::jni_native.http_transaction_error); 1.6 already
 * reaches the menu and is left untouched.
 *
 * Firing an error rather than a fake success is the same deliberate choice as
 * the socket layer (see libc_socket.cpp): a definite failure lands the caller on
 * a path it was written to survive, whereas a fabricated 200-with-no-body would
 * feed the engine a response it might parse into garbage.
 */

#include <sprout/dex/dex.h>
#include <sprout/game/symbols.h>

#include <map>
#include <mutex>
#include <vector>

namespace sprout {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidHttpTransaction";

/* thiz (the transaction jobject) -> nativeTransaction (its C++ pointer, the
 * jlong the constructor was handed). Captured at <init> because that is the only
 * call that carries it, and needed at Start to name the transaction to the
 * error callback. */
std::mutex g_lock;
std::map<std::uint32_t, std::uint32_t> g_native_ptr;

/* Pending transaction errors: queued during Start(), delivered between frames
 * by http_deliver_pending(). The frame delay prevents a tight retry loop: an
 * immediate HttpTransactionError makes the engine think "responded in 0ms,
 * must be a transient error, retry right now", while a 1-frame gap signals
 * "the network really is down, move on". */
struct PendingTx { std::uint32_t thiz; std::uint32_t native; };
std::vector<PendingTx> g_pending;
std::mutex g_pending_lock;

/* AndroidHttpTransaction(long nativeTransaction, String method, String url)
 *
 * Built with NewObjectV, so the arguments arrive through the va_list and arg(0)
 * is the low word of the jlong -- which on ARM32 is the whole pointer, the high
 * word being 0. Record it against this object. */
void ctor(DexCall &d) {
    /* Java signature: (JLjava/lang/String;Ljava/lang/String;)V
     * ARM32 ABI splits the jlong across arg(0)=low, arg(1)=high,
     * then arg(2)=method (String), arg(3)=url (String). */
    const std::uint32_t native = d.arg(0);
    std::string method = d.string_arg(2);
    std::string url = d.string_arg(3);
    d.c.log("[http] %s %s (native=0x%x thiz=0x%x)", method.c_str(), url.c_str(), native, d.thiz);
    std::lock_guard<std::mutex> lk(g_lock);
    g_native_ptr[d.thiz] = native;
}

std::uint32_t take_native(std::uint32_t thiz) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_native_ptr.find(thiz);
    return it == g_native_ptr.end() ? 0 : it->second;
}

/* void Start() -- the original port let Start() do nothing and the transaction
 * hung. That stalled all downloads, at which point the engine entered a polling
 * loop checking Config_ConfigKeyExists / GetNetworkStatus every frame forever.
 * Calling HttpTransactionError to simulate a network timeout does work... but
 * it also causes the engine to retry immediately in a tight loop because no
 * real wall time passed between the attempt and the response.
 *
 * The pragmatic fix: let the transaction hang (Start does nothing). The engine
 * spins up some transactions, they all hang, and the loading screen times out
 * into the menu once it can't contact the download servers.
 *
 * A clean-pool `network=none` in config.ini already sets g_config.network_status
 * to 0, but no hook wires that to GetNetworkStatus yet, so the engine still
 * thinks it has WiFi.  Wiring that hook would be a cleaner fix; this is the
 * fallback that works now. */
void start(DexCall &d) {
    std::uint32_t native = take_native(d.thiz);
    if (native != 0) {
        std::lock_guard<std::mutex> lk(g_pending_lock);
        g_pending.push_back({d.thiz, native});
        d.c.log("[http] queued tx thiz=0x%x native=0x%x (total pending=%zu)", d.thiz, native, g_pending.size());
    }
    d.ret(0);
}

/* void Release() -- the engine is done with the transaction; forget its mapping
 * so a long session cannot accumulate dead entries. */
void release(DexCall &d) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_native_ptr.erase(d.thiz);
    d.ret(0);
}

/* The response getters, in case the engine queries a transaction it has not yet
 * been told failed. A device answers 0 / null when the connection never
 * succeeded, so these match that -- and null must be the empty string, not a
 * real null, or the std::string(const char*) that consumes it aborts the guest
 * (the same trap the Facebook and Cloud getters avoid). */
void status_zero(DexCall &d) { d.ret(0); }
void empty_string(DexCall &d) { d.ret_string(""); }

}  // namespace

void register_android_http(HookTable &t) {
    t.add(kClass, "<init>", ctor);
    t.add(kClass, "Start", start);
    t.add(kClass, "Release", release);
    t.add(kClass, "GetStatusCode", status_zero);
    t.add(kClass, "GetResponseLength", status_zero);
    t.add(kClass, "GetStatusLine", empty_string);
    t.add(kClass, "GetResponseHeader", empty_string);
    t.add(kClass, "GetHumanReadableUrl", empty_string);
}

/* Called from the session loop between frames. Fires HttpTransactionError for
 * every transaction that was started but never completed, so the engine can
 * proceed in offline mode. */
void http_deliver_pending(pvz2_elf_image_t *img, GuestRuntime *rt) {
    const GameSymbols &s = sym();
    if (s.jni_native.http_transaction_error == 0) return;

    std::vector<PendingTx> batch;
    {
        std::lock_guard<std::mutex> lk(g_pending_lock);
        batch.swap(g_pending);
    }
    if (batch.empty()) return;
    std::printf("[dex] http_deliver_pending: delivering %zu transaction error(s)\n", batch.size());

    const std::uint32_t fn = img->so_base + s.jni_native.http_transaction_error;
    for (const auto &tx : batch) {
        const std::uint32_t args[] = {dex::kJniEnvPtrAddr, tx.thiz, tx.native, 0u, 0u};
        runtime::call_guest_between_frames_n(fn, args, 5);
        std::printf("[dex] http_deliver_pending: delivered error for tx thiz=0x%x native=0x%x\n", tx.thiz, tx.native);
    }
    /* Wake any guest thread blocked on a semaphore that was never posted --
     * typically the download-manager thread waiting for work that Start() was
     * meant to enqueue. Without this the thread stays stuck, prolonging the
     * loading screen and hanging shutdown. */
    {
        std::lock_guard<std::mutex> lk(rt->sems_lock);
        for (auto &kv : rt->guest_sems) {
            GuestSem *gs = kv.second.get();
            std::lock_guard<std::mutex> slk(gs->m);
            if (gs->waiters > 0 && gs->posts == 0) {
                std::printf("[dex] http_deliver_pending: posting stale sem 0x%08x (waiters=%u)\n",
                            kv.first, gs->waiters);
                gs->count++;
                gs->posts++;
                gs->cv.notify_one();
            }
        }
    }
}

}  // namespace dex
}  // namespace sprout

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

/* AndroidHttpTransaction(long nativeTransaction, String method, String url)
 *
 * Built with NewObjectV, so the arguments arrive through the va_list and arg(0)
 * is the low word of the jlong -- which on ARM32 is the whole pointer, the high
 * word being 0. Record it against this object. */
void ctor(DexCall &d) {
    const std::uint32_t native = d.arg(0);
    std::lock_guard<std::mutex> lk(g_lock);
    g_native_ptr[d.thiz] = native;
}

std::uint32_t take_native(std::uint32_t thiz) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_native_ptr.find(thiz);
    return it == g_native_ptr.end() ? 0 : it->second;
}

/* void Start() -- fail the transaction at once, the way a real device does after
 * its connect timeout. */
void start(DexCall &d) {
    const std::uint32_t native = take_native(d.thiz);
    const std::uint32_t err = sym().jni_native.http_transaction_error;
    if (native == 0 || err == 0) {
        /* No callback mapped for this version, or a Start on an object whose
         * <init> we never saw. Either way the old do-nothing behaviour, but say
         * so: a hung transaction is exactly the failure this hook exists to stop. */
        d.c.log("[http] Start() on transaction 0x%08x with no error path (native=0x%08x, cb=0x%08x)"
                " -- request will hang",
                d.thiz, native, err);
        d.ret(0);
        return;
    }

    /* HttpTransactionError(JNIEnv* env, jobject thiz, jlong nativeTransaction).
     * The jlong is even-register aligned: r2 = low (the pointer), r3 = high (0).
     * Runs nested to completion; it only enqueues, so there is no re-entrancy
     * into anything Start holds. */
    const std::uint32_t fn = d.c.img->so_base + err;
    const std::uint32_t args[4] = {kJniEnvPtrAddr, d.thiz, native, 0};
    d.c.log("[http] failing transaction 0x%08x (native=0x%08x) via HttpTransactionError", d.thiz,
            native);
    d.c.call_guest(fn, args, 4);
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

}  // namespace dex
}  // namespace sprout

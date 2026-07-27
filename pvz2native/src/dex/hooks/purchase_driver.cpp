#include <sprout/dex/dex.h>
#include <sprout/config.h>
#include <sprout/dependencies/dependency.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace sprout {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/GooglePlayPurchaseDriver";
constexpr const char *kSkuClass = "com/popcap/SexyAppFramework/purchase/SkuDetails";
constexpr const char *kPackage = "com.ea.game.pvz2_na";
constexpr int kPurchaseStatePurchased = 0;

struct Pending {
    enum Kind { Complete, Refresh } kind;
    std::string sku;
    std::vector<std::string> skus;
};

struct Store {
    std::mutex lock;
    std::uint32_t driver_thiz = 0;
    std::uint32_t native_ptr = 0;
    bool native_ptr_known = false;
    std::vector<Pending> pending;

    std::mutex sku_lock;
    std::map<std::uint32_t, std::string> sku_by_obj;
};
Store &store() {
    static Store s;
    return s;
}

bool iap_on() { return pvz2_config()->emulate_iap != 0; }

/* --- receipt fabrication --- */

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string purchase_json(const std::string &sku) {
    const std::string e = json_escape(sku);
    return std::string("{\"orderId\":\"SPROUT.") + e + "\",\"packageName\":\"" + kPackage +
           "\",\"productId\":\"" + e + "\",\"purchaseTime\":0,\"purchaseState\":" +
           std::to_string(kPurchaseStatePurchased) + ",\"purchaseToken\":\"sprout." + e +
           "\",\"developerPayload\":\"\"}";
}

std::string base64url(const std::string &in) {
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        const std::uint32_t n = (std::uint8_t)in[i] << 16 | (std::uint8_t)in[i + 1] << 8 |
                                (std::uint8_t)in[i + 2];
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += tbl[n & 63];
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const std::uint32_t n = (std::uint32_t)(std::uint8_t)in[i] << 16;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
    } else if (rem == 2) {
        const std::uint32_t n = (std::uint8_t)in[i] << 16 | (std::uint8_t)in[i + 1] << 8;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
    }
    return out;
}

/* --- guest memory helpers --- */

std::uint32_t alloc_gstr(pvz2_elf_image_t *img, GuestRuntime *rt, const std::string &s) {
    const std::uint32_t addr = rt->heap.alloc((std::uint32_t)s.size() + 1);
    if (addr == 0 || (std::uint64_t)addr + s.size() + 1 > img->mem_size) return 0;
    std::memcpy(&img->mem[addr], s.data(), s.size());
    img->mem[addr + s.size()] = 0;
    return addr;
}

void deliver_complete(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t thiz,
                      std::uint32_t native_ptr, const std::string &sku) {
    const std::uint32_t fn = find_native_method(kClass, "FirePaymentComplete");
    if (fn == 0) {
        std::printf("pvz2: [iap] cannot grant \"%s\": FirePaymentComplete not registered\n",
                    sku.c_str());
        return;
    }

    const std::string original_json = purchase_json(sku);
    const std::string signature;
    const std::string wrapper =
        std::string("{\"signedData\":") + "\"" + json_escape(original_json) + "\",\"signature\":\"" +
        json_escape(signature) + "\"}";
    const std::string receipt_id = "sprout." + sku;
    const std::string order_id = "SPROUT." + sku;
    const std::string base64 = base64url(wrapper);

    const std::uint32_t g_receipt = alloc_gstr(img, rt, receipt_id);
    const std::uint32_t g_base64 = alloc_gstr(img, rt, base64);
    const std::uint32_t g_sku = alloc_gstr(img, rt, sku);
    const std::uint32_t g_json = alloc_gstr(img, rt, original_json);
    const std::uint32_t g_sig = alloc_gstr(img, rt, signature);
    const std::uint32_t g_order = alloc_gstr(img, rt, order_id);
    if (g_receipt == 0 || g_base64 == 0 || g_sku == 0 || g_json == 0 || g_sig == 0 || g_order == 0) {
        std::printf("pvz2: [iap] cannot grant \"%s\": guest heap exhausted\n", sku.c_str());
        return;
    }

    const std::uint32_t args[] = {
        kJniEnvPtrAddr, thiz, native_ptr, 0u, g_receipt, g_base64,
        g_sku,          g_json, g_sig,    g_order, 1u,
    };
    std::printf("pvz2: [iap] granting \"%s\" -> FirePaymentComplete(fn=0x%08x, ptr=0x%08x)\n",
                sku.c_str(), fn, native_ptr);
    runtime::call_guest_between_frames_n(fn, args, (int)(sizeof(args) / sizeof(args[0])));
}

void deliver_refresh(pvz2_elf_image_t *img, GuestRuntime *rt, std::uint32_t thiz,
                     std::uint32_t native_ptr, const std::vector<std::string> &skus) {
    const std::uint32_t fn = find_native_method(kClass, "FireDidRefresh");
    if (fn == 0) {
        std::printf("pvz2: [iap] FireDidRefresh not registered -- refresh dropped\n");
        return;
    }

    const std::uint32_t n = (std::uint32_t)skus.size();
    const std::uint32_t bytes = n > 0 ? n * 4u : 4u;
    const std::uint32_t arr = rt->heap.alloc(bytes);
    if (arr == 0 || (std::uint64_t)arr + bytes > img->mem_size) {
        std::printf("pvz2: [iap] cannot build SkuDetails array -- heap exhausted\n");
        return;
    }

    const std::uint32_t sku_class = rt->find_or_alloc_class(kSkuClass);
    {
        std::lock_guard<std::mutex> lk(store().sku_lock);
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t obj = rt->new_object(sku_class);
            store().sku_by_obj[obj] = skus[i];
            std::memcpy(&img->mem[arr + i * 4], &obj, 4);
        }
    }
    set_array_length(arr, n);

    const std::uint32_t args[] = {kJniEnvPtrAddr, thiz, native_ptr, 0u, arr};
    std::printf("pvz2: [iap] FireDidRefresh(fn=0x%08x, ptr=0x%08x, %u sku(s))\n", fn, native_ptr, n);
    runtime::call_guest_between_frames_n(fn, args, (int)(sizeof(args) / sizeof(args[0])));
}

/* --- SkuDetails --- */

std::vector<std::string> read_string_array(DexCall &d, std::uint32_t arr) {
    std::vector<std::string> out;
    if (arr == 0) return out;
    std::uint32_t n = d.c.rt->heap.size_of(arr) / 4u;
    if (n > 512) n = 512;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t p = d.c.read32(arr + i * 4u);
        if (p != 0) out.push_back(d.c.cstr(p, 256));
    }
    return out;
}

std::string sku_of(std::uint32_t obj) {
    std::lock_guard<std::mutex> lk(store().sku_lock);
    auto it = store().sku_by_obj.find(obj);
    return it == store().sku_by_obj.end() ? std::string() : it->second;
}

void sku_get_sku(DexCall &d) { d.ret_string(sku_of(d.thiz)); }
void sku_get_type(DexCall &d) { d.ret_string("inapp"); }
void sku_get_price(DexCall &d) { d.ret_string("$0.00"); }
void sku_get_title(DexCall &d) { d.ret_string(sku_of(d.thiz)); }
void sku_get_description(DexCall &d) { d.ret_string(""); }

/* --- hooks --- */

void ctor(DexCall &d) {
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.driver_thiz = d.thiz;
    if (d.va != 0) {
        s.native_ptr = d.arg(0);
        s.native_ptr_known = true;
        d.c.log("[iap] driver constructed: thiz=0x%08x nativePtr=0x%08x", d.thiz, d.arg(0));
    } else {
        s.native_ptr_known = false;
        d.c.log("[iap] driver constructed WITHOUT a va_list: native pointer unknown", d.thiz);
    }
    d.ret(0);
}

void can_make_payments(DexCall &d) {
    const bool ok = iap_on();
    d.c.log("[iap] CanMakePayments -> %s", ok ? "true" : "false");
    d.ret_bool(ok);
}

void product_type_is_supported(DexCall &d) {
    const bool ok = iap_on();
    d.c.log("[iap] ProductTypeIsSupported(%u) -> %s", d.arg(0), ok ? "true" : "false");
    d.ret_bool(ok);
}

void request_payment(DexCall &d) {
    const std::string sku = d.string_arg(0);
    if (!iap_on()) {
        d.c.log("[iap] RequestPayment(\"%s\") ignored -- emulate_iap is off", sku.c_str());
        d.ret(0);
        return;
    }
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.pending.push_back({Pending::Complete, sku});
    d.c.log("[iap] RequestPayment(\"%s\") queued for grant", sku.c_str());
    d.ret(1);
}

void refresh(DexCall &d) {
    if (!iap_on()) { d.ret(0); return; }
    std::vector<std::string> skus = read_string_array(d, d.arg(0));
    Store &s = store();
    std::lock_guard<std::mutex> lk(s.lock);
    s.pending.push_back({Pending::Refresh, std::string(), std::move(skus)});
    d.c.log("[iap] Refresh(%zu sku(s)) queued", s.pending.back().skus.size());
    d.ret(1);
}

void ignored(DexCall &d) { d.ret(0); }
void has_unconfirmed_payments(DexCall &d) { d.ret_bool(false); }
void on_activity_result(DexCall &d) { d.c.log("[iap] onActivityResult -> true"); d.ret_bool(true); }

}  // namespace

void iap_deliver_pending(pvz2_elf_image_t *img, GuestRuntime *rt) {
    Store &s = store();

    std::vector<Pending> due;
    std::uint32_t thiz, native_ptr;
    bool ptr_known;
    {
        std::lock_guard<std::mutex> lk(s.lock);
        if (s.pending.empty()) return;
        due.swap(s.pending);
        thiz = s.driver_thiz;
        native_ptr = s.native_ptr;
        ptr_known = s.native_ptr_known;
    }

    if (!ptr_known || thiz == 0) {
        std::printf("pvz2: [iap] %zu action(s) pending but driver/ptr unknown -- dropping\n",
                    due.size());
        return;
    }

    for (const Pending &p : due) {
        if (p.kind == Pending::Complete) {
            deliver_complete(img, rt, thiz, native_ptr, p.sku);
        } else {
            deliver_refresh(img, rt, thiz, native_ptr, p.skus);
        }
    }
}

void register_purchase_driver(HookTable &t) {
    t.add(kClass, "<init>", ctor);
    t.add(kClass, "CanMakePayments", can_make_payments);
    t.add(kClass, "ProductTypeIsSupported", product_type_is_supported);
    t.add(kClass, "RequestPayment", request_payment);
    t.add(kClass, "Refresh", refresh);
    t.add(kClass, "ConfirmDelivery", ignored);
    t.add(kClass, "HasUnconfirmedPayments", has_unconfirmed_payments);
    t.add(kClass, "Close", ignored);
    t.add(kClass, "onActivityResult", on_activity_result);

    t.add(kSkuClass, "getSku", sku_get_sku);
    t.add(kSkuClass, "getType", sku_get_type);
    t.add(kSkuClass, "getPrice", sku_get_price);
    t.add(kSkuClass, "getTitle", sku_get_title);
    t.add(kSkuClass, "getDescription", sku_get_description);
}

}  // namespace dex
}  // namespace sprout

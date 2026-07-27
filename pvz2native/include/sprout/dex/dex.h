#ifndef SPROUT_DEX_DEX_H
#define SPROUT_DEX_DEX_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <sprout/dependencies/dependency.h>

namespace sprout {
namespace dex {

constexpr std::uint32_t kJniTableAddr = 0x00006000;
constexpr std::uint32_t kJniStubsAddr = 0x00006800;
constexpr std::uint32_t kJniEnvPtrAddr = 0x00007000;
constexpr std::uint32_t kJniSvcBase = 10000;

constexpr std::uint32_t kJavaVmTableAddr = 0x00008000;
constexpr std::uint32_t kJavaVmStubsAddr = 0x00008100;
constexpr std::uint32_t kJavaVmPtrAddr = 0x00008200;
constexpr std::uint32_t kJavaVmSlotCount = 8;
constexpr std::uint32_t kJavaVmSvcBase = 20000;

std::uint32_t jni_slot_count();
const char *jni_slot_name(std::uint32_t slot);

void install(pvz2_elf_image_t *img);

bool owns_svc(std::uint32_t swi);
void dispatch_svc(GuestCall &c, std::uint32_t swi);

/* registered natives */
void register_native_method(const std::string &class_name, const std::string &method,
                            std::uint32_t fn_addr);
std::uint32_t find_native_method(const std::string &class_name, const std::string &method);

void set_array_length(std::uint32_t array, std::uint32_t count);

/* DexCall */
struct DexCall {
    GuestCall &c;
    const std::string &class_name;
    const std::string &method_name;

    char ret_kind = 'v';
    std::uint32_t thiz = 0;
    std::uint32_t va = 0;

    std::uint32_t arg(int i) const;
    std::string string_arg(int i) const;

    void ret(std::uint32_t v) { result = v; }
    void ret_bool(bool v) { result = v ? 1u : 0u; }
    void ret64(std::uint64_t v) { result64 = v; }
    void ret_string(const std::string &s);

    std::uint32_t result = 0;
    std::uint64_t result64 = 0;
};

using MethodHook = void (*)(DexCall &);

class HookTable {
public:
    void add(const char *class_name, const char *method, MethodHook fn) {
        map_[Key(class_name, method)] = fn;
    }
    MethodHook find(const std::string &class_name, const std::string &method) const {
        auto it = map_.find(Key(class_name, method));
        return it == map_.end() ? nullptr : it->second;
    }
    std::size_t size() const { return map_.size(); }

private:
    using Key = std::pair<std::string, std::string>;
    std::map<Key, MethodHook> map_;
};

void report_call_census();

void register_android_game_app(HookTable &t);
void register_android_surface_view(HookTable &t);
void register_android_http(HookTable &t);
void register_framework_activity(HookTable &t);
void register_google_play(HookTable &t);
void register_facebook(HookTable &t);
void register_purchase_driver(HookTable &t);

const HookTable &hook_table();

void iap_deliver_pending(pvz2_elf_image_t *img, GuestRuntime *rt);

void set_screen_size(std::uint32_t width, std::uint32_t height);
std::uint32_t screen_width();
std::uint32_t screen_height();

}  // namespace dex
}  // namespace sprout

#endif

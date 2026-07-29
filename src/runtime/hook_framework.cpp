#include <sprout/runtime/hook_framework.h>

#include <mutex>
#include <unordered_map>

namespace sprout {
namespace runtime {
namespace {

std::mutex g_hook_lock;
std::unordered_map<std::uint32_t, GuestHookFn> g_hooks;

}  // namespace

void register_guest_hook(std::uint32_t guest_addr, GuestHookFn hook) {
    std::lock_guard<std::mutex> lk(g_hook_lock);
    if (hook) g_hooks[guest_addr] = std::move(hook);
}

void unregister_guest_hook(std::uint32_t guest_addr) {
    std::lock_guard<std::mutex> lk(g_hook_lock);
    g_hooks.erase(guest_addr);
}

bool dispatch_guest_hook(std::uint32_t guest_addr, GuestCall &call) {
    GuestHookFn hook = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_hook_lock);
        auto it = g_hooks.find(guest_addr);
        if (it != g_hooks.end()) hook = it->second;
    }
    if (hook) {
        bool continue_orig = hook(call);
        return !continue_orig;
    }
    return false;
}

}  // namespace runtime
}  // namespace sprout

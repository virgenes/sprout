#ifndef SPROUT_RUNTIME_HOOK_FRAMEWORK_H
#define SPROUT_RUNTIME_HOOK_FRAMEWORK_H

#include <cstdint>
#include <functional>
#include <sprout/dependencies/dependency.h>

namespace sprout {
namespace runtime {

using GuestHookFn = std::function<bool(GuestCall &call)>;

/* Registers a pre-execution hook at a guest ARM address.
 * If the callback returns true, the guest function execution continues normally.
 * If the callback returns false, execution returns immediately without executing guest body. */
void register_guest_hook(std::uint32_t guest_addr, GuestHookFn hook);

/* Unregisters a guest hook. */
void unregister_guest_hook(std::uint32_t guest_addr);

/* Evaluates if a hook exists at target_addr and executes it. Returns true if handled (suppress original). */
bool dispatch_guest_hook(std::uint32_t guest_addr, GuestCall &call);

}  // namespace runtime
}  // namespace sprout

#endif

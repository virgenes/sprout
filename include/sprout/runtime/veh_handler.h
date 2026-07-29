#ifndef SPROUT_RUNTIME_VEH_HANDLER_H
#define SPROUT_RUNTIME_VEH_HANDLER_H

#include <cstddef>
#include <cstdint>

namespace sprout {
namespace runtime {

/* Initializes Windows Vectored Exception Handling (VEH) for Dynarmic fastmem page fault traps.
 * Returns true if VEH was successfully registered. */
bool init_veh_handler(std::uint8_t *guest_mem, std::size_t guest_size);

/* Removes registered VEH handler. */
void shutdown_veh_handler();

}  // namespace runtime
}  // namespace sprout

#endif

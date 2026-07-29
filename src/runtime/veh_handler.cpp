#include <sprout/runtime/veh_handler.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace sprout {
namespace runtime {
namespace {

PVOID g_veh_handle = nullptr;
std::uint8_t *g_guest_mem = nullptr;
std::size_t g_guest_size = 0;

LONG NTAPI veh_exception_handler(PEXCEPTION_POINTERS info) {
    if (info && info->ExceptionRecord) {
        DWORD code = info->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION) {
            ULONG_PTR fault_addr = info->ExceptionRecord->ExceptionInformation[1];
            ULONG_PTR base = reinterpret_cast<ULONG_PTR>(g_guest_mem);
            if (fault_addr >= base && fault_addr < base + g_guest_size) {
                /* Access within guest bounds that generated page fault */
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool init_veh_handler(std::uint8_t *guest_mem, std::size_t guest_size) {
    g_guest_mem = guest_mem;
    g_guest_size = guest_size;
    if (!g_veh_handle) {
        g_veh_handle = AddVectoredExceptionHandler(1, veh_exception_handler);
        if (g_veh_handle) {
            std::printf("pvz2: [veh] Vectored Exception Handler registered successfully\n");
            return true;
        }
    }
    return false;
}

void shutdown_veh_handler() {
    if (g_veh_handle) {
        RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
}

}  // namespace runtime
}  // namespace sprout

#else

namespace sprout {
namespace runtime {
bool init_veh_handler(std::uint8_t *, std::size_t) { return false; }
void shutdown_veh_handler() {}
}  // namespace runtime
}  // namespace sprout

#endif

/* Load-time bring-up: what Android's linker and runtime do before the app's
 * own Java code gets a turn, plus EA's framework startup. */

#include <sprout/engine/engine.h>

#include <sprout/game/symbols.h>
#include <sprout/runtime/dynarmic_config.h>

namespace sprout {
namespace engine {

namespace rt_ = sprout::runtime;

void boot_native_library(pvz2_elf_image_t *img, GuestRuntime *rt) {
    rt_::run_init_array(img, rt);
    rt_::run_jni_onload(img, rt);
}

void run_eaframework_startup(pvz2_elf_image_t *img, GuestRuntime *rt) {
    /* Scratch area for fabricated argument payloads (fake jstring bytes, etc.),
     * well clear of the trampoline/JNI-table region. EAIO's startup needs a
     * live jstring for its documents path. */
    constexpr std::uint32_t kScratchAddr = 0x00009000;
    const std::uint32_t documents_path =
        rt_::make_fake_jstring(img, kScratchAddr, "/sprout/documents");

    rt_::run_export(img, rt, "Java_com_ea_EAThread_EAThread_Init");
    rt_::run_export(img, rt, "Java_com_ea_EAIO_EAIO_StartupNativeImpl", documents_path);
    rt_::run_export(img, rt, "Java_com_ea_EAIO_EAIO_Shutdown");
}

}  // namespace engine
}  // namespace sprout

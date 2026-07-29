/* com.popcap.SexyAppFramework.SexyAppFrameworkActivity
 *
 * The Activity itself. The engine only asks it one thing that matters, but it
 * is the single most important call in the whole boot.
 */

#include <sprout/dependencies/vfs.h>
#include <sprout/dex/dex.h>

namespace sprout {
namespace dex {
namespace {

constexpr const char *kClass = "com/popcap/SexyAppFramework/SexyAppFrameworkActivity";

/* static String FrameworkInfo_SysGetMainExpansionFilePath()
 *
 * The engine asks Java for the .obb path and then opens it with plain libc --
 * THIS is how the RSB reaches memory (doc 9.23). The real Java
 * (SexyAppFrameworkActivity.java:289) resolves it through the APK-expansion
 * downloader database to /storage/.../Android/obb/<pkg>/main.7.<pkg>.obb.
 *
 * Note the leading slash: ResStreamsManager::LoadRSB only uses a path verbatim
 * when it starts with '/', and otherwise treats it as a relative resource name
 * to be routed through the Java asset API. obb_guest_path() preserves that;
 * vfs::translate() strips the slash again on the way to the host. */
void main_expansion_file_path(DexCall &d) {
    d.ret_string(vfs::obb_guest_path());
}

}  // namespace

void register_framework_activity(HookTable &t) {
    t.add(kClass, "FrameworkInfo_SysGetMainExpansionFilePath", main_expansion_file_path);
}

}  // namespace dex
}  // namespace sprout

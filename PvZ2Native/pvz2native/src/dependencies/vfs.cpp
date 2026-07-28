/* Guest path translation -- see vfs.h for why this layer exists at all.
 *
 * Also holds a cached FILE* for the .obb expansion file: the game opens, reads,
 * and closes the .obb dozens of times during resource loading, and every such
 * cycle triggers a host fopen/fseek/fread/fclose on a multi-hundred-megabyte
 * file.  Keeping one handle alive eliminates that repeated open/close overhead.
 */

#include <sprout/dependencies/vfs.h>

#include <sprout/config.h>

#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <io.h>
#endif

namespace sprout {
namespace vfs {
namespace {

/* Cached .obb file handle.  Opened lazily on first request and never closed
 * until obb_close() is called at session shutdown. */
std::FILE *g_obb_fp = nullptr;
std::mutex g_obb_mutex;

}  // namespace

/* Real on-disk resources (doc section 9.22). The game's own native loader
 * (ResStreamsManager/ResourceManager: sub_2BE120 -> sub_867740/sub_864950)
 * opens "main.rsb" via libc fopen/fread and decodes the outer "1bsr" RSB plus
 * the inner "pgsr"/RTON formats ITSELF -- so none of those formats are
 * reimplemented here. It gets real file I/O over the real .obb, which literally
 * IS a "1bsr" archive (magic confirmed). */
/* Both come from config.ini (see pvz2_config_load): the host path is used
 * verbatim, and the guest path is the same with a leading '/' so
 * ResStreamsManager::LoadRSB takes its verbatim branch (see vfs.h). The guest
 * form is cached on first use -- the config path is fixed for the run. */
const char *obb_host_path() { return pvz2_config()->obb_path; }

const char *obb_guest_path() {
    static const std::string guest = std::string("/") + pvz2_config()->obb_path;
    return guest.c_str();
}

std::FILE *obb_fp() {
    std::lock_guard<std::mutex> lk(g_obb_mutex);
    if (g_obb_fp == nullptr) {
        const char *path = obb_host_path();
        if (path != nullptr && path[0] != '\0') {
            g_obb_fp = std::fopen(path, "rb");
            if (g_obb_fp == nullptr) {
                std::fprintf(stderr, "pvz2: [vfs] failed to open obb cache: %s\n", path);
            }
        }
    }
    return g_obb_fp;
}

void obb_close() {
    std::lock_guard<std::mutex> lk(g_obb_mutex);
    if (g_obb_fp != nullptr) {
        std::fclose(g_obb_fp);
        g_obb_fp = nullptr;
    }
}

std::string translate(GuestRuntime *rt, std::string p) {
    /* SexyAppFramework's VFS tags every resource lookup with a scheme prefix
     * before it reaches libc; on Android the AAssetManager layer consumes it,
     * so the bytes libc sees never carry it. Stripping it here is what unblocked
     * the RSB load: the engine logged "Loading main RSB from the path <obb>"
     * and then asked for "ASSET:<obb>", which no host open could satisfy. */
    static const char kAssetScheme[] = "ASSET:";
    if (p.compare(0, sizeof(kAssetScheme) - 1, kAssetScheme) == 0) {
        p.erase(0, sizeof(kAssetScheme) - 1);
    }
    for (char &c : p) if (c == '\\') c = '/';
    /* Undo the leading slash obb_guest_path() carries to look absolute to the
     * engine: "/E:/..." is not a path the host can open.
     * Also strip leading '/' from non-drive-letter paths (e.g. "/lib/main.147...obb")
     * so they resolve relative to the working directory on Windows. */
    if (p.size() >= 1 && p[0] == '/') {
        if (p.size() >= 3 && std::isalpha((unsigned char)p[1]) && p[2] == ':') {
            p.erase(0, 1);  /* "/E:..." -> "E:..." */
        } else {
            p.erase(0, 1);  /* "/lib/..." -> "lib/..." */
        }
    }

    /* Save data lands in a folder the ENGINE names: "No_Backup/" is a string
     * literal inside libPVZ2.so (on iOS/Android it marks data excluded from
     * cloud backup, which means nothing on a PC). Renaming it here rather than
     * patching the .so keeps this independent of the game build -- and this is
     * the layer every path already passes through on its way to libc, so it
     * covers the mkdir that creates it as well as the reads and writes. */
    static const char kEngineDataDir[] = "No_Backup";
    static const char kHostDataDir[] = "userdata";
    for (std::size_t at = p.find(kEngineDataDir); at != std::string::npos;
         at = p.find(kEngineDataDir, at + sizeof(kHostDataDir) - 1)) {
        p.replace(at, sizeof(kEngineDataDir) - 1, kHostDataDir);
    }

    std::size_t slash = p.find_last_of('/');
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    for (char &c : base) c = (char)std::tolower((unsigned char)c);

    if (base == "main.rsb") {
        if (rt != nullptr) rt->rsb_touched = true;
        return std::string(obb_host_path());
    }
    /* The engine may also open the .obb by the full path it got from
     * FrameworkInfo_SysGetMainExpansionFilePath -- that counts as reaching the
     * RSB load too. */
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".obb") == 0) {
        if (rt != nullptr) rt->rsb_touched = true;
    }
    return p;
}

int translate_open_flags(std::uint32_t g) {
    int h = (int)(g & 3); /* O_RDONLY/O_WRONLY/O_RDWR share values 0/1/2 */
#if defined(_WIN32)
    if (g & 0x40)  h |= _O_CREAT;
    if (g & 0x80)  h |= _O_EXCL;
    if (g & 0x200) h |= _O_TRUNC;
    if (g & 0x400) h |= _O_APPEND;
    h |= _O_BINARY;
#else
    if (g & 0x40)  h |= O_CREAT;
    if (g & 0x80)  h |= O_EXCL;
    if (g & 0x200) h |= O_TRUNC;
    if (g & 0x400) h |= O_APPEND;
#endif
    return h;
}

}  // namespace vfs
}  // namespace sprout

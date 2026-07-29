/* The per-version address tables, and the detection that picks one.
 *
 * Adding a release means adding one entry to kVersions below and nothing else:
 * no file outside this one holds an address into libPVZ2.so. To fill an entry,
 * decompile JNI_OnLoad and read its JNINativeMethod tables for the eight
 * `native` offsets, then take the two fingerprints with any hex viewer.
 */

#include <sprout/game/symbols.h>
#include <sprout/log/log.h>

#include <cstdio>
#include <cstring>

namespace sprout {
namespace {

const GameSymbols kUnknown{"unknown", {}, 0, {}, {}, {}, {}, 0, 0};

const GameSymbols kVersions[] = {
    /* --- 1.6.10 (2013, armeabi-v7a) ------------------------------------- */
    {
        "1.6.10",
        /* native */ {
            0x9eaf98, /* Native_GameAppInitialize                */
            0x9ebfb8, /* Native_applicationWillFinishLaunching   */
            0x9ec0d8, /* Native_applicationDidFinishLaunching    */
            0x9ec0f4, /* Native_applicationWillBecomeForeground  */
            0x9ec100, /* Native_applicationDidBecomeActive       */
            0x9f1878, /* Native_onSurfaceCreated                 */
            0x9f1914, /* Native_onSurfaceChanged                 */
            0x9f1944, /* Native_onDrawFrame                      */
            0,        /* no PumpMessageQueue: onDrawFrame drains */
        },
        /* surface_changed_pad */ 2,
        /* global */ {
            0xd55650, /* dword_D55650 -- LawnApp                 */
            0xdc8fd4, /* dword_DC8FD4 -- AndroidAppDriver        */
        },
        /* fn */ {
            0xb75560, /* std::string(const char*, allocator)     */
        },
        /* jni_native */ {
            0, /* http_transaction_error: 1.6 reaches the menu, left untouched */
        },
        /* input */ {
            68,  /* driver        */
            696, /* scaler        */
            192, /* touch_begin   */
            196, /* touch_end     */
            48,  /* touch_stride  */
            220, /* multitouch    */
            221, /* touch_active  */
            176, /* scaler_fields */
            432, /* vt_touch_down */
            436, /* vt_touch_up   */
            440, /* vt_touch_move */
        },
        0xE59F0014E92D4800ull, /* PUSH {R11,LR}; LDR R0,=...      */
        0xE24DD094E92D4FF0ull, /* PUSH {R4-R11,LR}; SUB SP,SP,#0x94 */
    },

    /* --- 4.5.2 (2016, armeabi-v7a, 18MB) ---------------------------------
     *
     * Same ARM32 binary shape as 1.6 and the same eight natives with the same
     * JNI signatures, so nothing in runtime/ or engine/ needed to change to
     * reach it -- which was the whole point of splitting this table out.
     *
     * Only three natives are registered from JNI_OnLoad here (GameAppInitialize,
     * GameAppTeardown, getGooglePlayAPIKey); the rest live in two more
     * JNINativeMethod arrays at 0x10A6054 (lifecycle, 11 entries) and 0x10A610C
     * (surface, 3 entries), reached by following a data xref from each method
     * NAME string. That is the reliable way to find them in a stripped build --
     * do not try to recognise the functions themselves.
     *
     * Globals and the diagnostic field offsets are NOT mapped yet: they drive
     * only diagnostics/, which checks for 0 and stays quiet, so the boot does
     * not depend on them. */
    {
        "4.5.2",
        /* native */ {
            0xcc033c, /* Native_GameAppInitialize                */
            0xcc131c, /* Native_applicationWillFinishLaunching   */
            0xcc1420, /* Native_applicationDidFinishLaunching    */
            0xcc142c, /* Native_applicationWillBecomeForeground  */
            0xcc1430, /* Native_applicationDidBecomeActive       */
            0xcc7cf0, /* Native_onSurfaceCreated                 */
            0xcc7d8c, /* Native_onSurfaceChanged                 */
            0xcc7e60, /* Native_onDrawFrame                      */
            0xcc7cd0, /* PumpMessageQueue                        */
        },
        /* surface_changed_pad */ 1, /* body reads r2/r3 -- plain static native */
        /* global */ {
            0x117a6f0, /* LawnApp: checked for NULL in onDrawFrame          */
            0x117a734, /* AndroidAppDriver: the pointer onSurfaceCreated   */
                       /* and onDrawFrame dereference (1.6's dword_DC8FD4) */
        },
        /* fn */ {0},
        /* jni_native */ {
            0xcded8c, /* HttpTransactionError: enqueues a failure via sub_CB9F80 */
        },
        /* input */ {},
        0xE59F1010E59F0010ull, /* LDR R0,[PC,#0x10]; LDR R1,[PC,#0x10] */
        0xE24DD084E92D4FF0ull, /* PUSH {R4-R11,LR}; SUB SP,SP,#0x84    */
    },
};

const GameSymbols *g_active = &kUnknown;

/* Reads the 8 bytes at a .so offset as a little-endian u64, or 0 if the offset
 * lies outside the loaded image. */
std::uint64_t fingerprint_at(const pvz2_elf_image_t *img, std::uint32_t offset) {
    const std::uint64_t addr = (std::uint64_t)img->so_base + offset;
    if (offset == 0 || addr + 8 > img->mem_size) return 0;
    std::uint64_t v = 0;
    std::memcpy(&v, &img->mem[addr], 8);
    return v;
}

bool matches(const pvz2_elf_image_t *img, const GameSymbols &v) {
    return fingerprint_at(img, v.native.on_draw_frame) == v.fingerprint_draw_frame &&
           fingerprint_at(img, v.native.game_app_initialize) == v.fingerprint_game_app_init;
}

static GameSymbols g_pattern_scanned_symbols{};

}  // namespace

const GameSymbols &sym() { return *g_active; }

bool game_symbols_pattern_scan(const pvz2_elf_image_t *img, GameSymbols *out_symbols) {
    if (img == nullptr || out_symbols == nullptr) return false;
    std::memset(out_symbols, 0, sizeof(*out_symbols));
    out_symbols->version = "auto-scanned";

    /* Scan memory span for known ARM instruction prologues */
    const std::uint8_t *mem = img->mem;
    const std::uint32_t len = img->mem_size;

    /* PUSH {R4-R11,LR} -> 0xE92D4FF0 */
    constexpr std::uint32_t kPushFrame = 0xE92D4FF0;
    std::uint32_t found_draw_frame = 0;
    std::uint32_t found_game_init = 0;

    for (std::uint32_t offset = 0; offset + 8 < len; offset += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, &mem[img->so_base + offset], 4);
        if (word == kPushFrame) {
            std::uint64_t fp = fingerprint_at(img, offset);
            for (const GameSymbols &v : kVersions) {
                if (fp == v.fingerprint_draw_frame) found_draw_frame = offset;
                if (fp == v.fingerprint_game_app_init) found_game_init = offset;
            }
        }
    }

    if (found_draw_frame && found_game_init) {
        out_symbols->native.on_draw_frame = found_draw_frame;
        out_symbols->native.game_app_initialize = found_game_init;
        out_symbols->fingerprint_draw_frame = fingerprint_at(img, found_draw_frame);
        out_symbols->fingerprint_game_app_init = fingerprint_at(img, found_game_init);
        return true;
    }
    return false;
}

bool game_symbols_detect(const pvz2_elf_image_t *img) {
    if (img == nullptr) return false;
    for (const GameSymbols &v : kVersions) {
        if (!matches(img, v)) continue;
        g_active = &v;
        LOG_VERSION("libPVZ2.so identified as %s (onDrawFrame at 0x%x, verified)",
                    v.version, v.native.on_draw_frame);
        return true;
    }

    /* Attempt pattern scanning if exact fingerprint match failed */
    if (game_symbols_pattern_scan(img, &g_pattern_scanned_symbols)) {
        g_active = &g_pattern_scanned_symbols;
        LOG_VERSION("libPVZ2.so auto-detected via pattern scan (onDrawFrame at 0x%x)",
                    g_pattern_scanned_symbols.native.on_draw_frame);
        return true;
    }

    /* Not a version we know. Say exactly what was expected and what is
     * actually there, because that difference is the whole of the work needed
     * to add the build -- and booting on a guess would call whatever function
     * happens to live at another release's offsets. */
    LOG_VERSION("this libPVZ2.so matches no known build (%u MB image, %u imports)",
                (unsigned)(img->so_span >> 20), img->trampoline_count);
    for (const GameSymbols &v : kVersions) {
        LOG_VERSION("  tried %-8s: onDrawFrame 0x%x expected %016llx, found %016llx",
                    v.version, v.native.on_draw_frame,
                    (unsigned long long)v.fingerprint_draw_frame,
                    (unsigned long long)fingerprint_at(img, v.native.on_draw_frame));
    }
    LOG_VERSION("add an entry to kVersions in src/game/symbols.cpp to support it");
    return false;
}

}  // namespace sprout

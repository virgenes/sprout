/* libz.so -- zlib.
 *
 * The engine decompresses RSG payloads and every .PTX texture through this,
 * driving a z_stream that lives in GUEST memory. GuestZlib (src/runtime/)
 * marshals that struct to a host z_stream and runs the vendored zlib directly
 * over guest memory -- one flat host buffer, so no copying is involved in
 * either direction.
 *
 * The one-shot helpers below need no state at all and go straight to the host.
 */

#include <sprout/dependencies/dependency.h>

#include <zlib.h>

namespace sprout {
namespace {

/* ------------------------------------------------------------- streaming */

void z_inflate_init(GuestCall &c) {
    /* (strm, version, stream_size) -- the version/size pair only guards ABI
     * drift between the caller's zlib.h and the library, which cannot happen
     * here. */
    c.set_result((std::uint32_t)c.rt->zlib.inflate_init(c.img->mem, c.img->mem_size, c.arg(0)));
}

void z_inflate_init2(GuestCall &c) {
    /* (strm, windowBits, version, stream_size). A negative windowBits means raw
     * deflate and 16+ means gzip; the engine only ever uses the zlib default,
     * which is what inflate_init sets up. */
    c.set_result((std::uint32_t)c.rt->zlib.inflate_init(c.img->mem, c.img->mem_size, c.arg(0)));
}

void z_inflate(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.inflate_run(c.img->mem, c.img->mem_size, c.arg(0),
                                                       (int)c.arg(1)));
}

void z_inflate_reset(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.inflate_reset(c.img->mem, c.img->mem_size, c.arg(0)));
}

void z_inflate_end(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.inflate_end(c.img->mem, c.img->mem_size, c.arg(0)));
}

void z_deflate_init(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.deflate_init(c.img->mem, c.img->mem_size, c.arg(0),
                                                        (int)c.arg(1)));
}

void z_deflate_init2(GuestCall &c) {
    /* (strm, level, method, windowBits, memLevel, strategy, ...) -- only the
     * level is honoured; nothing in the engine's save path depends on a
     * non-default window or strategy. */
    c.set_result((std::uint32_t)c.rt->zlib.deflate_init(c.img->mem, c.img->mem_size, c.arg(0),
                                                        (int)c.arg(1)));
}

void z_deflate(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.deflate_run(c.img->mem, c.img->mem_size, c.arg(0),
                                                       (int)c.arg(1)));
}

void z_deflate_reset(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.deflate_reset(c.img->mem, c.img->mem_size, c.arg(0)));
}

void z_deflate_end(GuestCall &c) {
    c.set_result((std::uint32_t)c.rt->zlib.deflate_end(c.img->mem, c.img->mem_size, c.arg(0)));
}

/* -------------------------------------------------------------- one-shot */

void z_uncompress(GuestCall &c) {
    /* (dest, destLen*, source, sourceLen): destLen is in/out. */
    std::uint32_t dest = c.arg(0), dest_len_ptr = c.arg(1), src = c.arg(2), src_len = c.arg(3);
    std::uint32_t dest_len = c.read32(dest_len_ptr);
    if (!c.in_bounds(dest, dest_len) || !c.in_bounds(src, src_len)) {
        c.set_result((std::uint32_t)Z_BUF_ERROR);
        return;
    }
    uLongf out_len = dest_len;
    int rc = uncompress(c.img->mem + dest, &out_len, c.img->mem + src, src_len);
    if (dest_len_ptr != 0) c.write32(dest_len_ptr, (std::uint32_t)out_len);
    c.set_result((std::uint32_t)rc);
}

void z_compress(GuestCall &c) {
    std::uint32_t dest = c.arg(0), dest_len_ptr = c.arg(1), src = c.arg(2), src_len = c.arg(3);
    std::uint32_t dest_len = c.read32(dest_len_ptr);
    if (!c.in_bounds(dest, dest_len) || !c.in_bounds(src, src_len)) {
        c.set_result((std::uint32_t)Z_BUF_ERROR);
        return;
    }
    uLongf out_len = dest_len;
    int rc = compress(c.img->mem + dest, &out_len, c.img->mem + src, src_len);
    if (dest_len_ptr != 0) c.write32(dest_len_ptr, (std::uint32_t)out_len);
    c.set_result((std::uint32_t)rc);
}

void z_compress_bound(GuestCall &c) {
    c.set_result((std::uint32_t)compressBound(c.arg(0)));
}

/* ------------------------------------------------------------- checksums */

void z_crc32(GuestCall &c) {
    /* (running_value, buf, len); a null buf asks for the seed value. */
    std::uint32_t running = c.arg(0), buf = c.arg(1), len = c.arg(2);
    if (buf == 0 || len == 0) { c.set_result((std::uint32_t)crc32(0, Z_NULL, 0)); return; }
    if (!c.in_bounds(buf, len)) { c.set_result(running); return; }
    c.set_result((std::uint32_t)crc32(running, c.img->mem + buf, len));
}

void z_adler32(GuestCall &c) {
    std::uint32_t running = c.arg(0), buf = c.arg(1), len = c.arg(2);
    if (buf == 0 || len == 0) { c.set_result((std::uint32_t)adler32(0, Z_NULL, 0)); return; }
    if (!c.in_bounds(buf, len)) { c.set_result(running); return; }
    c.set_result((std::uint32_t)adler32(running, c.img->mem + buf, len));
}

void z_version(GuestCall &c) {
    static std::uint32_t slot = 0;
    if (slot == 0) slot = c.dup_cstr(ZLIB_VERSION);
    c.set_result(slot);
}

}  // namespace

void register_libz(ImportTable &t) {
    t.add("inflateInit_", z_inflate_init);
    t.add("inflateInit2_", z_inflate_init2);
    t.add("inflate", z_inflate);
    t.add("inflateReset", z_inflate_reset);
    t.add("inflateEnd", z_inflate_end);

    t.add("deflateInit_", z_deflate_init);
    t.add("deflateInit2_", z_deflate_init2);
    t.add("deflate", z_deflate);
    t.add("deflateReset", z_deflate_reset);
    t.add("deflateEnd", z_deflate_end);

    t.add("uncompress", z_uncompress);
    t.add("compress", z_compress);
    t.add("compress2", z_compress);
    t.add("compressBound", z_compress_bound);

    t.add("crc32", z_crc32);
    t.add("adler32", z_adler32);
    t.add("zlibVersion", z_version);
}

}  // namespace sprout

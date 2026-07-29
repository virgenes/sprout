#ifndef SPROUT_RUNTIME_GUEST_ZLIB_H
#define SPROUT_RUNTIME_GUEST_ZLIB_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace sprout {

/* Host-backed zlib for the guest's inflate/deflate imports.
 *
 * libPVZ2.so links against Android's libz and drives it through a z_stream
 * living in GUEST memory, so the streaming calls cannot just be forwarded:
 * each one has to marshal the guest struct into a host z_stream, run the
 * host zlib over guest memory, then write the updated cursors back. The
 * internal state stays host-side, keyed by the guest z_stream address.
 *
 * Guest z_stream is the standard 32-bit layout, 14 words:
 *   +0 next_in  +4 avail_in  +8 total_in  +12 next_out  +16 avail_out
 *   +20 total_out  +24 msg  +28 state  +32 zalloc  +36 zfree  +40 opaque
 *   +44 data_type  +48 adler  +52 reserved
 * next_in/next_out are guest addresses; because guest memory is one flat host
 * buffer, the host zlib can read and write it in place with no copying. */
class GuestZlib {
public:
    /* All of these return the zlib status code to hand back to the guest
     * (Z_OK, Z_STREAM_END, Z_BUF_ERROR, Z_STREAM_ERROR, ...). `mem` is the
     * base of guest memory and `mem_size` its length, used to reject
     * out-of-bounds windows rather than letting zlib walk off the buffer. */
    int inflate_init(uint8_t *mem, uint32_t mem_size, uint32_t strm);
    int inflate_run(uint8_t *mem, uint32_t mem_size, uint32_t strm, int flush);
    int inflate_reset(uint8_t *mem, uint32_t mem_size, uint32_t strm);
    int inflate_end(uint8_t *mem, uint32_t mem_size, uint32_t strm);

    int deflate_init(uint8_t *mem, uint32_t mem_size, uint32_t strm, int level);
    int deflate_run(uint8_t *mem, uint32_t mem_size, uint32_t strm, int flush);
    int deflate_reset(uint8_t *mem, uint32_t mem_size, uint32_t strm);
    int deflate_end(uint8_t *mem, uint32_t mem_size, uint32_t strm);

    /* Both are defined in the .cpp: Stream is incomplete here, so the maps'
     * unique_ptr destructors must only ever be instantiated where it is
     * complete -- including the cleanup path the compiler emits for the
     * constructor. */
    GuestZlib();
    ~GuestZlib();

private:
    struct Stream; /* wraps a host z_stream; defined in the .cpp so zlib.h
                    * stays out of every translation unit that includes this */

    std::mutex lock_;
    std::unordered_map<uint32_t, std::unique_ptr<Stream>> inflaters_;
    std::unordered_map<uint32_t, std::unique_ptr<Stream>> deflaters_;
};

}  // namespace sprout

#endif

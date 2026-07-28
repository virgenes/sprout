#include <sprout/runtime/guest_zlib.h>

#include <cstring>

#include <zlib.h>

namespace sprout {

struct GuestZlib::Stream {
    z_stream z{};
    bool open = false;
};

namespace {

/* Guest z_stream field offsets (32-bit layout). */
constexpr uint32_t kNextIn = 0;
constexpr uint32_t kAvailIn = 4;
constexpr uint32_t kTotalIn = 8;
constexpr uint32_t kNextOut = 12;
constexpr uint32_t kAvailOut = 16;
constexpr uint32_t kTotalOut = 20;
constexpr uint32_t kMsg = 24;
constexpr uint32_t kState = 28;
constexpr uint32_t kDataType = 44;
constexpr uint32_t kAdler = 48;
constexpr uint32_t kStreamSize = 56;

uint32_t rd(const uint8_t *mem, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, mem + addr, 4);
    return v;
}

void wr(uint8_t *mem, uint32_t addr, uint32_t v) {
    std::memcpy(mem + addr, &v, 4);
}

bool window_ok(uint32_t mem_size, uint32_t addr, uint32_t len) {
    /* A zero-length window is legal and its pointer is never dereferenced. */
    if (len == 0) return true;
    return addr < mem_size && (uint64_t)addr + len <= mem_size;
}

/* Copies the guest's cursors into the host stream. Returns false if either
 * window falls outside guest memory. */
bool load_windows(z_stream &z, uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    const uint32_t in_addr = rd(mem, strm + kNextIn);
    const uint32_t in_len = rd(mem, strm + kAvailIn);
    const uint32_t out_addr = rd(mem, strm + kNextOut);
    const uint32_t out_len = rd(mem, strm + kAvailOut);
    if (!window_ok(mem_size, in_addr, in_len) || !window_ok(mem_size, out_addr, out_len)) return false;
    z.next_in = mem + in_addr;
    z.avail_in = in_len;
    z.next_out = mem + out_addr;
    z.avail_out = out_len;
    return true;
}

/* Writes the host stream's advanced cursors and counters back to the guest. */
void store_windows(const z_stream &z, uint8_t *mem, uint32_t strm) {
    wr(mem, strm + kNextIn, (uint32_t)(z.next_in - mem));
    wr(mem, strm + kAvailIn, z.avail_in);
    wr(mem, strm + kTotalIn, (uint32_t)z.total_in);
    wr(mem, strm + kNextOut, (uint32_t)(z.next_out - mem));
    wr(mem, strm + kAvailOut, z.avail_out);
    wr(mem, strm + kTotalOut, (uint32_t)z.total_out);
    wr(mem, strm + kDataType, (uint32_t)z.data_type);
    wr(mem, strm + kAdler, (uint32_t)z.adler);
    /* The guest may read msg on error and print it; it holds a HOST pointer
     * that means nothing in guest space, so never publish it. */
    wr(mem, strm + kMsg, 0);
}

}  // namespace

GuestZlib::GuestZlib() = default;

GuestZlib::~GuestZlib() {
    for (auto &kv : inflaters_) {
        if (kv.second->open) inflateEnd(&kv.second->z);
    }
    for (auto &kv : deflaters_) {
        if (kv.second->open) deflateEnd(&kv.second->z);
    }
}

int GuestZlib::inflate_init(uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    if (!window_ok(mem_size, strm, kStreamSize)) return Z_STREAM_ERROR;
    std::lock_guard<std::mutex> lk(lock_);
    auto s = std::make_unique<Stream>();
    /* zalloc/zfree left null so the host zlib uses its own allocator: the
     * inflate state lives host-side and the guest never inspects it. */
    int rc = inflateInit(&s->z);
    if (rc != Z_OK) return rc;
    s->open = true;
    /* The guest tests `state != NULL` to tell an initialised stream from a
     * fresh one, so publish a non-null marker it will never dereference. */
    wr(mem, strm + kState, strm);
    wr(mem, strm + kMsg, 0);
    wr(mem, strm + kTotalIn, 0);
    wr(mem, strm + kTotalOut, 0);
    inflaters_[strm] = std::move(s);
    return Z_OK;
}

int GuestZlib::inflate_run(uint8_t *mem, uint32_t mem_size, uint32_t strm, int flush) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = inflaters_.find(strm);
    if (it == inflaters_.end()) return Z_STREAM_ERROR;
    if (!load_windows(it->second->z, mem, mem_size, strm)) return Z_STREAM_ERROR;
    int rc = inflate(&it->second->z, flush);
    store_windows(it->second->z, mem, strm);
    return rc;
}

int GuestZlib::inflate_reset(uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = inflaters_.find(strm);
    if (it == inflaters_.end()) return Z_STREAM_ERROR;
    int rc = inflateReset(&it->second->z);
    if (window_ok(mem_size, strm, kStreamSize)) {
        wr(mem, strm + kTotalIn, 0);
        wr(mem, strm + kTotalOut, 0);
        wr(mem, strm + kMsg, 0);
    }
    return rc;
}

int GuestZlib::inflate_end(uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = inflaters_.find(strm);
    if (it == inflaters_.end()) return Z_STREAM_ERROR;
    int rc = inflateEnd(&it->second->z);
    it->second->open = false;
    inflaters_.erase(it);
    if (window_ok(mem_size, strm, kStreamSize)) wr(mem, strm + kState, 0);
    return rc;
}

int GuestZlib::deflate_init(uint8_t *mem, uint32_t mem_size, uint32_t strm, int level) {
    if (!window_ok(mem_size, strm, kStreamSize)) return Z_STREAM_ERROR;
    std::lock_guard<std::mutex> lk(lock_);
    auto s = std::make_unique<Stream>();
    int rc = deflateInit(&s->z, level);
    if (rc != Z_OK) return rc;
    s->open = true;
    wr(mem, strm + kState, strm);
    wr(mem, strm + kMsg, 0);
    wr(mem, strm + kTotalIn, 0);
    wr(mem, strm + kTotalOut, 0);
    deflaters_[strm] = std::move(s);
    return Z_OK;
}

int GuestZlib::deflate_run(uint8_t *mem, uint32_t mem_size, uint32_t strm, int flush) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = deflaters_.find(strm);
    if (it == deflaters_.end()) return Z_STREAM_ERROR;
    if (!load_windows(it->second->z, mem, mem_size, strm)) return Z_STREAM_ERROR;
    int rc = deflate(&it->second->z, flush);
    store_windows(it->second->z, mem, strm);
    return rc;
}

int GuestZlib::deflate_reset(uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = deflaters_.find(strm);
    if (it == deflaters_.end()) return Z_STREAM_ERROR;
    int rc = deflateReset(&it->second->z);
    if (window_ok(mem_size, strm, kStreamSize)) {
        wr(mem, strm + kTotalIn, 0);
        wr(mem, strm + kTotalOut, 0);
        wr(mem, strm + kMsg, 0);
    }
    return rc;
}

int GuestZlib::deflate_end(uint8_t *mem, uint32_t mem_size, uint32_t strm) {
    std::lock_guard<std::mutex> lk(lock_);
    auto it = deflaters_.find(strm);
    if (it == deflaters_.end()) return Z_STREAM_ERROR;
    int rc = deflateEnd(&it->second->z);
    it->second->open = false;
    deflaters_.erase(it);
    if (window_ok(mem_size, strm, kStreamSize)) wr(mem, strm + kState, 0);
    return rc;
}

}  // namespace sprout

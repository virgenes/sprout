/* SDL audio sink for the emulated OpenSL buffer-queue player -- see
 * include/sprout/audio/audio_output.h for the threading contract.
 *
 * Lock-free ring buffer: the SDL audio callback (real-time thread) and the
 * guest pump thread never contend on a mutex.  Buffer ownership is transferred
 * through atomic sequence numbers, so each side owns exactly the slots it
 * reads/writes and the two never touch the same slot concurrently.
 *
 * Control operations (configure, shutdown, clear) synchronise by closing the
 * device first -- SDL_CloseAudioDevice waits for any pending callback to
 * finish -- guaranteeing no callback is running when they drain the ring.
 */

#include <sprout/audio/audio_output.h>

#include <SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace sprout {
namespace audio {
namespace {

/* --- lock-free slot ring -------------------------------------------------- */

/* Power of two so that (index & kSlotMask) is a cheap modulo.  Each slot
 * holds one PCM buffer whose ownership transfers from producer (enqueue) to
 * consumer (SDL audio callback) via the atomics below.  kMaxQueued limits the
 * in-flight count to keep one slot always empty, which is what prevents the
 * producer from overwriting a slot the consumer is still reading. */
static constexpr std::size_t kSlotCount = 64;
static constexpr std::size_t kSlotMask  = kSlotCount - 1;
static constexpr std::size_t kMaxQueued = kSlotCount - 1;  /* 63 */

struct Slot {
    std::vector<std::uint8_t> *data = nullptr;
};

std::array<Slot, kSlotCount> g_slots;

/* Monotonically-increasing sequence numbers.  The producer writes to
 * slot[write_seq & kSlotMask], then bumps write_seq.  The consumer reads
 * from slot[read_seq & kSlotMask], then bumps read_seq.  Because
 * write_seq - read_seq never exceeds kMaxQueued, the producer can never
 * overtake the consumer and the two always operate on different slots. */
std::atomic<std::uint32_t> g_write_seq{0};
std::atomic<std::uint32_t> g_read_seq{0};

/* Only the consumer (SDL callback) touches these: */
std::size_t g_head_offset = 0;    /* bytes consumed from the current slot */

/* Completions are produced by the consumer (one per fully-played slot) and
 * consumed by the pump thread, so they need no lock either. */
std::atomic<std::uint32_t> g_completed{0};
std::atomic<bool>          g_playing{false};

/* --- device handle (written only under control mutex) --------------------- */

std::atomic<SDL_AudioDeviceID> g_device{0};
int g_channels = 0, g_rate = 0;
std::atomic<int> g_bits{0};

/* --- health counters ------------------------------------------------------ */
std::atomic<std::uint64_t> g_underruns{0};
std::atomic<std::uint64_t> g_enqueue_fails{0};

/* --- volume (0..256, 256 = full, read lock-free by the callback) ---------- */
std::atomic<std::uint32_t> g_volume_scaled{256};

/* --- completion CV (separate mutex -- not on the data path) --------------- */
bool g_shutdown = false;

/* Helper: deferred-init completion CV to avoid running the non-constexpr
 * constructor during CRT static initialisation (before main()), which would
 * crash with an access violation. */
static std::condition_variable& completion_cv() {
    static std::condition_variable cv;
    return cv;
}
static std::mutex& cv_mutex() {
    static std::mutex m;
    return m;
}

/* --- control mutex (clear/shutdown/configure only) ------------------------ */
static std::mutex& control_mutex() {
    static std::mutex m;
    return m;
}

/* ========================================================================== */
/*  SDL audio callback – runs on the audio thread                             */
/* ========================================================================== */
void SDLCALL audio_callback(void * /*user*/, Uint8 *stream, int len) {
    /* Snapshot the completion counter before producing anything, so we can
     * detect whether this callback produced a real completion. */
    std::uint32_t completed_before = g_completed.load(std::memory_order_relaxed);
    int written = 0;

    while (written < len) {
        /* Snapshot the consumer index.  The producer can advance write_seq
         * concurrently but that only means more data becomes visible --
         * it never invalidates the slot we are about to read. */
        std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
        std::uint32_t ws = g_write_seq.load(std::memory_order_acquire);

        if (cs == ws || !g_playing.load(std::memory_order_relaxed)) {
            break;   /* queue empty or paused – silence-fill below */
        }

        std::uint32_t slot = cs & kSlotMask;
        std::vector<std::uint8_t> *buf = g_slots[slot].data;
        if (buf == nullptr) {
            break;
        }

        std::size_t avail = buf->size() - g_head_offset;
        std::size_t want  = static_cast<std::size_t>(len - written);
        std::size_t n     = avail < want ? avail : want;
        std::memcpy(stream + written, buf->data() + g_head_offset, n);
        written += static_cast<int>(n);
        g_head_offset += n;

        if (g_head_offset >= buf->size()) {
            g_head_offset = 0;
            g_read_seq.store(cs + 1, std::memory_order_release);
            g_completed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /* Apply volume to every sample we wrote.  Uses integer fixed-point
     * (256 = full scale) so the callback never touches a float. */
    if (written > 0) {
        std::uint32_t vol = g_volume_scaled.load(std::memory_order_relaxed);
        if (vol != 256) {
            int bits = g_bits.load(std::memory_order_relaxed);
            if (bits == 16) {
                std::int16_t *samples = reinterpret_cast<std::int16_t *>(stream);
                int n = written / 2;
                for (int i = 0; i < n; ++i) {
                    samples[i] = static_cast<std::int16_t>(
                        (static_cast<std::int32_t>(samples[i]) * static_cast<std::int32_t>(vol)) >> 8);
                }
            } else if (bits == 8) {
                for (int i = 0; i < written; ++i) {
                    int s = (static_cast<int>(stream[i]) - 128) * static_cast<int>(vol) / 256 + 128;
                    stream[i] = static_cast<Uint8>(s < 0 ? 0 : s > 255 ? 255 : s);
                }
            }
        }
    }

    /* Silence-fill whatever the queue could not satisfy. */
    if (written < len) {
        std::memset(stream + written, 0, static_cast<std::size_t>(len - written));
        if (g_playing.load(std::memory_order_relaxed)) {
            g_underruns.fetch_add(1, std::memory_order_relaxed);
            /* Synthesise a completion when no real buffer finished.  The
             * bq_pump thread runs ONLY on completions, and Wwise refills the
             * buffer queue from inside that thread.  Without this, a starved
             * callback produces no completion, the pump never wakes, and the
             * audio engine stalls permanently -- "audio command queue is full"
             * followed by silence as the last DMA buffer loops. */
            if (g_completed.load(std::memory_order_relaxed) == completed_before) {
                g_completed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

/* ========================================================================== */
/*  Device helpers                                                            */
/* ========================================================================== */

void close_device(SDL_AudioDeviceID dev) {
    if (dev != 0) SDL_CloseAudioDevice(dev);
}

/* Drains every slot without waiting for the consumer.  The caller guarantees
 * that no audio callback is running (device closed or not yet opened). */
void drain_locked() {
    std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
    std::uint32_t ws = g_write_seq.load(std::memory_order_relaxed);
    std::uint32_t n  = ws - cs;
    if (n > 0) {
        g_completed.fetch_add(n, std::memory_order_relaxed);
        g_read_seq.store(ws, std::memory_order_release);
    }
    g_head_offset = 0;
}

}  // namespace

/* ========================================================================== */
/*  Public API                                                                */
/* ========================================================================== */

bool configure(int channels, int sample_rate_hz, int bits_per_sample) {
    std::lock_guard<std::mutex> ctl(control_mutex());

    /* Fast path: same format, device already open. */
    if (g_device != 0 && channels == g_channels &&
        sample_rate_hz == g_rate && bits_per_sample == g_bits) {
        return true;
    }

    SDL_AudioDeviceID old_device = g_device;
    g_device = 0;   /* stop accepting enqueues */

    /* Close the old device NOW – SDL_CloseAudioDevice blocks until any
     * pending callback returns, guaranteeing the callback is not running
     * when we drain below. */
    close_device(old_device);

    /* Drain whatever was left in the ring.  No callback can be running at
     * this point, so this is safe without atomics. */
    drain_locked();
    /* Free slot data left by the drain.  Since the callback cannot run,
     * the producer-visible slots are exactly [g_read_seq, g_write_seq) and
     * those contain data that will never be consumed. */
    {
        std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
        std::uint32_t ws = g_write_seq.load(std::memory_order_relaxed);
        for (std::uint32_t i = cs; i < ws; ++i) {
            delete g_slots[i & kSlotMask].data; g_slots[i & kSlotMask].data = nullptr;
        }
    }

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        SDL_Log("pvz2 audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = sample_rate_hz;
    want.format   = (bits_per_sample == 8) ? AUDIO_U8 : AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels);
    want.samples  = 1024;
    want.callback = audio_callback;

    SDL_AudioSpec have{};
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        SDL_Log("pvz2 audio: SDL_OpenAudioDevice(%d Hz, %d ch, %d bit) failed: %s",
                sample_rate_hz, channels, bits_per_sample, SDL_GetError());
        return false;
    }

    g_device  = dev;
    g_channels = channels;
    g_rate    = sample_rate_hz;
    g_bits    = bits_per_sample;

    SDL_PauseAudioDevice(dev, g_playing.load(std::memory_order_relaxed) ? 0 : 1);
    SDL_Log("pvz2 audio: device open -- %d Hz, %d ch, %d bit", sample_rate_hz, channels,
            bits_per_sample);
    return true;
}

void set_playing(bool playing) {
    g_playing.store(playing, std::memory_order_relaxed);
    SDL_AudioDeviceID dev;
    {
        std::lock_guard<std::mutex> ctl(control_mutex());
        dev = g_device;
    }
    if (dev != 0) SDL_PauseAudioDevice(dev, playing ? 0 : 1);
}

bool enqueue(const void *pcm, std::size_t bytes) {
    if (pcm == nullptr || bytes == 0) return false;

    /* Quick rejection if the device is gone. */
    if (g_device.load(std::memory_order_relaxed) == 0) {
        g_enqueue_fails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint32_t ws = g_write_seq.load(std::memory_order_relaxed);
    std::uint32_t cs = g_read_seq.load(std::memory_order_acquire);

    if (ws - cs >= kMaxQueued) {
        g_enqueue_fails.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::uint32_t slot = ws & kSlotMask;
    const auto *p = static_cast<const std::uint8_t *>(pcm);

    /* The consumer has already advanced past this slot (confirmed by
     * ws - cs < kMaxQueued above), so we own it exclusively.  Allocate or
     * re-use the vector. */
    auto &s = g_slots[slot];
    if (s.data == nullptr) {
        s.data = new std::vector<std::uint8_t>(p, p + bytes);
    } else {
        s.data->assign(p, p + bytes);
    }

    /* Publish AFTER the data is written.  The consumer loads this with
     * acquire ordering, so it sees the vector contents. */
    g_write_seq.store(ws + 1, std::memory_order_release);
    return true;
}

void diag(std::uint64_t &underruns, std::uint64_t &enqueue_fails) {
    underruns     = g_underruns.load(std::memory_order_relaxed);
    enqueue_fails = g_enqueue_fails.load(std::memory_order_relaxed);
}

void clear() {
    std::lock_guard<std::mutex> ctl(control_mutex());

    /* The caller (configure/shutdown) must have closed the device before
     * calling clear, which also waits for any in-flight callback, so no
     * consumer is running. */
    drain_locked();
    /* Free the data vectors that were in flight. */
    std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
    std::uint32_t ws = g_write_seq.load(std::memory_order_relaxed);
    for (std::uint32_t i = cs; i < ws; ++i) {
        delete g_slots[i & kSlotMask].data; g_slots[i & kSlotMask].data = nullptr;
    }

    completion_cv().notify_all();
}

std::uint32_t queued_count() {
    std::uint32_t ws = g_write_seq.load(std::memory_order_acquire);
    std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
    return ws - cs;
}

bool take_completion() {
    std::uint32_t c = g_completed.load(std::memory_order_relaxed);
    while (c > 0) {
        if (g_completed.compare_exchange_weak(c, c - 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool wait_for_completion() {
    std::unique_lock<std::mutex> lk(cv_mutex());
    /* Wake on any completion OR on shutdown.  On timeout (15 ms) we return
     * true anyway so Wwise keeps ticking -- see the header for why. */
    completion_cv().wait_for(lk, std::chrono::milliseconds(15),
                             [] { return g_completed.load(std::memory_order_acquire) != 0
                                  || g_shutdown; });
    if (g_shutdown) return false;
    if (g_completed.load(std::memory_order_relaxed) != 0) {
        /* Atomic decrement: consume one completion.  The pump thread is the
         * sole consumer of g_completed (take_completion is a non-blocking
         * fallback), so this is not contended between threads -- but using
         * CAS keeps it formally correct. */
        std::uint32_t c = g_completed.load(std::memory_order_relaxed);
        while (c > 0) {
            if (g_completed.compare_exchange_weak(c, c - 1,
                    std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
    }
    return true;
}

void shutdown() {
    SDL_AudioDeviceID dev;
    {
        std::lock_guard<std::mutex> ctl(control_mutex());
        dev     = g_device;
        g_device = 0;
    }

    /* Signal the pump thread to stop before closing the device. */
    {
        std::lock_guard<std::mutex> cv(cv_mutex());
        g_shutdown = true;
    }
    completion_cv().notify_all();

    /* Close the device -- this waits for any pending callback to finish. */
    close_device(dev);

    /* Now drain and reset everything.  The callback cannot run, and enqueue
     * sees g_device == 0 so it will not add new data. */
    {
        std::lock_guard<std::mutex> ctl(control_mutex());
        drain_locked();
        std::uint32_t cs = g_read_seq.load(std::memory_order_relaxed);
        std::uint32_t ws = g_write_seq.load(std::memory_order_relaxed);
        for (std::uint32_t i = cs; i < ws; ++i) {
            delete g_slots[i & kSlotMask].data; g_slots[i & kSlotMask].data = nullptr;
        }
        g_channels = 0;
        g_rate     = 0;
        g_bits     = 0;
        g_playing.store(false, std::memory_order_relaxed);
    }
}

void set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    g_volume_scaled.store(static_cast<std::uint32_t>(vol * 256.0f + 0.5f),
                          std::memory_order_relaxed);
}

float get_volume() {
    return static_cast<float>(g_volume_scaled.load(std::memory_order_relaxed)) / 256.0f;
}

}  // namespace audio
}  // namespace sprout

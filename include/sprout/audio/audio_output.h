#ifndef SPROUT_AUDIO_AUDIO_OUTPUT_H
#define SPROUT_AUDIO_AUDIO_OUTPUT_H

#include <cstddef>
#include <cstdint>

namespace sprout {
namespace audio {

/* The host end of the emulated OpenSL ES buffer-queue player.
 *
 * PvZ2 decodes and mixes its own audio (it has a whole bank/handle system of
 * its own) and uses OpenSL purely as an output sink: it enqueues finished PCM
 * and refills each buffer when told the buffer has played. So this layer never
 * needs to understand a single one of the game's audio formats -- it takes
 * PCM in and hands it to SDL.
 *
 * Threading contract: SDL's audio callback runs on its own thread, and guest
 * code must not run there (the JIT contexts are per guest thread, and a
 * callback racing the frame would touch the guest heap underneath it). So
 * enqueue() COPIES the PCM out of guest memory and the audio thread only ever
 * touches host buffers; completions are handed to a guest thread that does
 * nothing else -- see wait_for_completion(). */

/* Opens or reconfigures the device for the format the game asked for. Safe to
 * call again with a different format. Returns false if SDL refuses it. */
bool configure(int channels, int sample_rate_hz, int bits_per_sample);

/* SL_PLAYSTATE_PLAYING vs STOPPED/PAUSED. */
void set_playing(bool playing);

/* Copies `bytes` of PCM into the queue. False when the queue is full or no
 * device is open. */
bool enqueue(const void *pcm, std::size_t bytes);

/* Drops everything still queued (SLBufferQueueItf::Clear). */
void clear();

/* Buffers still waiting to play -- SLBufferQueueItf::GetState's count. */
std::uint32_t queued_count();

/* Health counters for a periodic diagnostic. `underruns` counts callbacks that
 * ran out of queued PCM and silence-filled (the queue drained -- the guest fell
 * behind or its callback thread stalled); `enqueue_fails` counts buffers refused
 * because the queue was already full (the guest ran ahead, or completions are
 * not being delivered so nothing drains). One climbing steadily is what tells a
 * gradual audio death apart from a whole-process freeze, where NEITHER moves
 * because guest code has stopped running at all. */
void diag(std::uint64_t &underruns, std::uint64_t &enqueue_fails);

/* Pops one "a buffer finished playing" notification, or returns false when
 * there are none. Non-blocking fallback used only when the dedicated callback
 * thread could not be started; the normal path is wait_for_completion(). */
bool take_completion();

/* Waits (up to a short timeout) for a buffer to finish playing; consumes that
 * notification if there is one and returns true, returns true ANYWAY on timeout
 * so the caller keeps ticking Wwise, and returns false only once shutdown() has
 * been called. The timeout is load-bearing -- see the definition: it stops a gap
 * in completion delivery from stalling Wwise's audio thread into a hang.
 *
 * This is the heartbeat of the whole audio engine, not a convenience. Wwise's
 * OpenSL sink submits its rendered ring to the buffer queue ONLY from inside
 * the buffer-queue callback, and wakes its own audio thread from there too
 * (CAkSinkOpenSL::EnqueueBufferCallback). Deliver that callback late and the
 * ring stays full, CAkSinkOpenSL::IsDataNeeded answers "0 buffers needed",
 * CAkAudioMgr::Perform skips its render loop entirely, and anything waiting on
 * a voice to finish -- AK::SoundEngine::UnloadBank, which blocks its caller on
 * a semaphore until the bank's last playing instance dies -- waits forever.
 * Draining this from the frame loop made exactly that a deadlock: the callback
 * could not be delivered until onDrawFrame returned, and onDrawFrame was inside
 * UnloadBank. Hence a thread of its own, which is also what real Android does. */
bool wait_for_completion();

void shutdown();

/* Volume control (0.0 = silent, 1.0 = full).  Applied as sample-level gain in
 * the audio callback, so it takes effect immediately without re-opening the
 * device. */
void set_volume(float vol);
float get_volume();

}  // namespace audio
}  // namespace sprout

#endif

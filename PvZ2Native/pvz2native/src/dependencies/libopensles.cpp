/* libOpenSLES.so -- Android's audio engine, emulated for real playback.
 *
 * PvZ2 imports six symbols: slCreateEngine plus five SL_IID_* interface ids
 * (ENGINE, PLAY, BUFFERQUEUE, ANDROIDCONFIGURATION, AUDIOIODEVICECAPABILITIES).
 * That set is the classic Android buffer-queue playback pattern, and it is the
 * reason audio is tractable here at all: the game decodes and mixes everything
 * itself and uses OpenSL purely as an output sink, so we never have to
 * understand one of its audio formats. It hands us finished PCM; we hand that
 * to SDL (see audio/audio_output.cpp) and tell it when each buffer has played.
 *
 * The awkward part is the calling convention. OpenSL is COM-shaped: every call
 * is `(*itf)->Method(itf, ...)`, so the vtables must live in GUEST memory and
 * their entries must be addresses the guest can branch to. make_guest_callback()
 * mints one "SVC #idx" stub per distinct method and binds our handler to it, so
 * a vtable is just an array of those stub addresses. `self` (r0) is always the
 * interface handle, which is how each handler finds its object again.
 *
 * Object layout, per object:
 *   one 4-byte guest "handle" per interface it exposes, holding that
 *   interface's vtable address. The handle's ADDRESS is what the guest holds as
 *   SLObjectItf/SLEngineItf/..., and g_handles maps it back to (object, kind).
 */

#include <sprout/audio/audio_output.h>
#include <sprout/dependencies/dependency.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sprout {
namespace {

/* --- OpenSL ES 1.0.1 constants -------------------------------------------- */

constexpr std::uint32_t kSuccess = 0;
constexpr std::uint32_t kPreconditionsViolated = 1;
constexpr std::uint32_t kParameterInvalid = 2;
constexpr std::uint32_t kMemoryFailure = 3;
constexpr std::uint32_t kFeatureUnsupported = 12;

constexpr std::uint32_t kStateUnrealized = 1;
constexpr std::uint32_t kStateRealized = 2;

constexpr std::uint32_t kPlayStateStopped = 1;
constexpr std::uint32_t kPlayStatePaused = 2;
constexpr std::uint32_t kPlayStatePlaying = 3;

/* Data locators. Note 2 is ADDRESS, not a buffer queue -- getting this wrong is
 * what made CreateAudioPlayer bounce with "unsupported player source". PvZ2
 * uses Android's own SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE (0x800007BD),
 * which has the same {locatorType, numBuffers} shape as the standard one. */
constexpr std::uint32_t kDataLocatorBufferQueue = 6;
constexpr std::uint32_t kDataLocatorAndroidSimpleBufferQueue = 0x800007BDu;
constexpr std::uint32_t kDataLocatorAndroidBufferQueue = 0x800007BEu;
constexpr std::uint32_t kDataFormatPCM = 2;

bool is_buffer_queue_locator(std::uint32_t t) {
    return t == kDataLocatorBufferQueue || t == kDataLocatorAndroidSimpleBufferQueue ||
           t == kDataLocatorAndroidBufferQueue;
}

/* --- interface identity ---------------------------------------------------
 *
 * SL_IID_* are imported DATA symbols of type SLInterfaceID, i.e. POINTERS to a
 * 16-byte UUID. The loader hands each a zeroed slot, and zero for all five
 * would make them indistinguishable at GetInterface time -- so each slot gets a
 * distinct pointer (into its own slot, where a tagged UUID is written) and
 * g_iids maps that pointer back to the interface kind. */

enum class Iface { Object, Engine, Play, BufferQueue, AndroidConfig, IODeviceCaps };

std::unordered_map<std::uint32_t, Iface> g_iids; /* IID pointer value -> kind */

const struct { const char *name; Iface kind; } kIidNames[] = {
    {"SL_IID_ENGINE", Iface::Engine},
    {"SL_IID_PLAY", Iface::Play},
    {"SL_IID_BUFFERQUEUE", Iface::BufferQueue},
    {"SL_IID_ANDROIDCONFIGURATION", Iface::AndroidConfig},
    {"SL_IID_AUDIOIODEVICECAPABILITIES", Iface::IODeviceCaps},
};

/* --- objects --------------------------------------------------------------- */

struct SlObject {
    enum class Kind { Engine, OutputMix, Player } kind;
    bool realized = false;

    std::uint32_t handle[6] = {0, 0, 0, 0, 0, 0}; /* indexed by Iface */

    /* Player state. */
    std::uint32_t play_state = kPlayStateStopped;
    std::uint32_t bq_callback = 0; /* guest fn: void(*)(SLBufferQueueItf, void*) */
    std::uint32_t bq_context = 0;
    int channels = 2, rate = 44100, bits = 16;
};

std::mutex g_lock;
std::vector<std::unique_ptr<SlObject>> g_objects;
std::unordered_map<std::uint32_t, std::pair<SlObject *, Iface>> g_handles;

/* The one player that owns the device. PvZ2 creates a single streaming player;
 * if it ever made a second, the first keeps the device and the second is inert
 * rather than fighting over it. */
SlObject *g_active_player = nullptr;

/* The dedicated guest thread that delivers buffer-queue callbacks, and the
 * trampoline it starts at. 0 means it never started, which is the only case
 * where audio_pump_callbacks()'s frame-loop fallback still does anything. */
std::uint32_t g_pump_trampoline = 0;
std::uint32_t g_pump_tid = 0;

SlObject *find(std::uint32_t self, Iface *kind_out = nullptr) {
    auto it = g_handles.find(self);
    if (it == g_handles.end()) return nullptr;
    if (kind_out != nullptr) *kind_out = it->second.second;
    return it->second.first;
}

/* --- vtables ---------------------------------------------------------------
 *
 * Built once, on the first slCreateEngine, and shared by every object: the
 * handlers recover their object from `self`, so there is nothing per-object in
 * a vtable. */

std::uint32_t g_vtable[6] = {0, 0, 0, 0, 0, 0}; /* indexed by Iface */
bool g_vtables_built = false;

/* Allocates a guest block and returns its address (0 on exhaustion). */
std::uint32_t galloc(GuestCall &c, std::uint32_t bytes) {
    return c.rt->heap.alloc(bytes);
}

/* Every unimplemented slot lands here. Returning "unsupported" rather than
 * crashing matters: the guest checks results and takes a silent-audio path,
 * which is a far better failure than a branch into address zero.
 *
 * It logs (capped) because an unimplemented method the game actually needs is
 * exactly how audio init dies quietly -- and without this the only symptom is
 * "no sound", with nothing in the log at all. */
void sl_unsupported(GuestCall &c) {
    static int reported = 0;
    if (reported < 24) {
        ++reported;
        c.log("[audio] UNSUPPORTED OpenSL method (self=0x%08x lr=0x%08x) -> FEATURE_UNSUPPORTED",
              c.arg(0), c.lr());
    }
    c.set_result(kFeatureUnsupported);
}

/* --- SLObjectItf ---------------------------------------------------------- */

void obj_realize(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr) { c.set_result(kParameterInvalid); return; }
    o->realized = true;
    /* Async realize (arg1 != 0) still completes synchronously here; the guest
     * only learns otherwise through a callback it never registers. */
    c.log("[audio] Realize(0x%08x) -> SUCCESS", c.arg(0));
    c.set_result(kSuccess);
}

void obj_get_state(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr || c.arg(1) == 0) { c.set_result(kParameterInvalid); return; }
    c.write32(c.arg(1), o->realized ? kStateRealized : kStateUnrealized);
    c.set_result(kSuccess);
}

void obj_get_interface(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    const std::uint32_t iid = c.arg(1);
    const std::uint32_t out = c.arg(2);
    if (o == nullptr || out == 0) {
        c.log("[audio] GetInterface(self=0x%08x iid=0x%08x) -> PARAMETER_INVALID (%s)", c.arg(0),
              iid, o == nullptr ? "unknown object" : "null out");
        c.set_result(kParameterInvalid);
        return;
    }
    /* Deliberately NOT requiring the object to be realized. Android's
     * SLAndroidConfigurationItf must be fetched BEFORE Realize (that is how the
     * stream type gets set), so a realized-only rule would reject the one
     * ordering the game actually uses. Handing out the interface early costs
     * nothing here -- the handle is valid for the object's whole life. */
    auto it = g_iids.find(iid);
    if (it == g_iids.end()) {
        /* The id the guest passed is not one of the five we tagged -- either the
         * data import never got its value, or it asked for an interface this
         * binary does not even import. */
        c.log("[audio] GetInterface(iid=0x%08x) -> UNKNOWN interface id", iid);
        c.set_result(kFeatureUnsupported);
        return;
    }
    const std::uint32_t handle = o->handle[(int)it->second];
    if (handle == 0) {
        c.log("[audio] GetInterface(iid kind=%d) -> this object does not expose it",
              (int)it->second);
        c.set_result(kFeatureUnsupported);
        return;
    }
    c.log("[audio] GetInterface(kind=%d) -> 0x%08x", (int)it->second, handle);
    c.write32(out, handle);
    c.set_result(kSuccess);
}

void obj_destroy(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr) { c.set_result(kSuccess); return; }
    if (o == g_active_player) {
        audio::set_playing(false);
        audio::clear();
        g_active_player = nullptr;
    }
    for (std::uint32_t h : o->handle) {
        if (h != 0) g_handles.erase(h);
    }
    /* The guest block itself is deliberately not freed: these are created once
     * at startup and the heap has no pressure worth reclaiming here. */
    for (auto it = g_objects.begin(); it != g_objects.end(); ++it) {
        if (it->get() == o) { g_objects.erase(it); break; }
    }
    c.set_result(kSuccess);
}

/* The remaining SLObjectItf methods.
 *
 * They exist for asynchronous realization and for priority arbitration between
 * clients -- neither of which exists here: Realize completes synchronously and
 * there is exactly one client. The temptation is to leave them "unsupported",
 * and that is precisely what killed audio the first time: the game called one
 * of them on the engine object immediately after GetInterface, got
 * SL_RESULT_FEATURE_UNSUPPORTED, and abandoned audio init entirely -- never
 * reaching CreateAudioPlayer. A benign SUCCESS is both harmless and the honest
 * answer for an implementation with nothing to arbitrate. */
void obj_resume(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr) { c.set_result(kParameterInvalid); return; }
    o->realized = true;
    c.log("[audio] Object.Resume(0x%08x) -> SUCCESS", c.arg(0));
    c.set_result(kSuccess);
}

void obj_register_callback(GuestCall &c) {
    /* Accepted and never invoked: realization is synchronous, so there is no
     * asynchronous completion for it to report. */
    c.log("[audio] Object.RegisterCallback(0x%08x) accepted", c.arg(0));
    c.set_result(kSuccess);
}

void obj_abort_async(GuestCall &c) {
    c.log("[audio] Object.AbortAsyncOperation(0x%08x)", c.arg(0));
    c.set_result(kSuccess);
}

void obj_set_priority(GuestCall &c) {
    c.log("[audio] Object.SetPriority(0x%08x) -> SUCCESS", c.arg(0));
    c.set_result(kSuccess);
}

void obj_get_priority(GuestCall &c) {
    if (c.arg(1) != 0) c.write32(c.arg(1), 0);
    c.set_result(kSuccess);
}

void obj_set_loss_of_control(GuestCall &c) {
    c.log("[audio] Object.SetLossOfControlInterfaces(0x%08x) -> SUCCESS", c.arg(0));
    c.set_result(kSuccess);
}

/* Realize/Resume/GetState/GetInterface/RegisterCallback/AbortAsyncOperation/
 * Destroy/SetPriority/GetPriority/SetLossOfControlInterfaces -- spec order. */
void build_object_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t slots[10] = {
        make_guest_callback("SL.Object.Realize", obj_realize),
        make_guest_callback("SL.Object.Resume", obj_resume),
        make_guest_callback("SL.Object.GetState", obj_get_state),
        make_guest_callback("SL.Object.GetInterface", obj_get_interface),
        make_guest_callback("SL.Object.RegisterCallback", obj_register_callback),
        make_guest_callback("SL.Object.AbortAsyncOperation", obj_abort_async),
        make_guest_callback("SL.Object.Destroy", obj_destroy),
        make_guest_callback("SL.Object.SetPriority", obj_set_priority),
        make_guest_callback("SL.Object.GetPriority", obj_get_priority),
        make_guest_callback("SL.Object.SetLossOfControlInterfaces", obj_set_loss_of_control),
    };
    for (int i = 0; i < 10; ++i) c.write32(addr + 4u * i, slots[i]);
}

/* --- SLEngineItf ---------------------------------------------------------- */

/* Creates an object, its interface handles, and registers them. Returns the
 * SLObjectItf handle, or 0 on heap exhaustion. Caller holds g_lock. */
SlObject *new_object(GuestCall &c, SlObject::Kind kind, std::initializer_list<Iface> ifaces) {
    auto obj = std::make_unique<SlObject>();
    obj->kind = kind;
    SlObject *raw = obj.get();

    /* Every SL object exposes SLObjectItf, plus whatever this kind adds. */
    std::vector<Iface> wanted{Iface::Object};
    wanted.insert(wanted.end(), ifaces.begin(), ifaces.end());

    for (Iface f : wanted) {
        const std::uint32_t h = galloc(c, 4);
        if (h == 0) {
            /* Unwind: the handles already registered point at an object that is
             * about to be destroyed with this unique_ptr. */
            for (std::uint32_t done : raw->handle) {
                if (done != 0) g_handles.erase(done);
            }
            return nullptr;
        }
        c.write32(h, g_vtable[(int)f]);
        raw->handle[(int)f] = h;
        g_handles[h] = {raw, f};
    }
    g_objects.push_back(std::move(obj));
    return raw;
}

void engine_create_output_mix(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    const std::uint32_t out = c.arg(1);
    if (out == 0) { c.set_result(kParameterInvalid); return; }
    SlObject *o = new_object(c, SlObject::Kind::OutputMix, {});
    if (o == nullptr) { c.set_result(kMemoryFailure); return; }
    c.write32(out, o->handle[(int)Iface::Object]);
    c.log("[audio] CreateOutputMix -> 0x%08x", o->handle[(int)Iface::Object]);
    c.set_result(kSuccess);
}

/* CreateAudioPlayer(self, pPlayer, pAudioSrc, pAudioSnk, numItf, itfIds, itfReq)
 *
 * Only the source matters: SLDataSource is {pLocator, pFormat}, the locator is
 * SLDataLocator_BufferQueue {type, numBuffers} and the format is
 * SLDataFormat_PCM {type, channels, samplesPerSec (MILLIHertz), bitsPerSample,
 * containerSize, channelMask, endianness}. The sink is the output mix, which
 * carries nothing we need. */
void engine_create_audio_player(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    const std::uint32_t out = c.arg(1);
    const std::uint32_t src = c.arg(2);
    if (out == 0 || src == 0) { c.set_result(kParameterInvalid); return; }

    const std::uint32_t locator = c.read32(src + 0);
    const std::uint32_t format = c.read32(src + 4);
    if (locator == 0 || format == 0) { c.set_result(kParameterInvalid); return; }

    if (!is_buffer_queue_locator(c.read32(locator + 0)) ||
        c.read32(format + 0) != kDataFormatPCM) {
        /* A URI or file-descriptor source would mean the game wanted US to
         * decode, which this layer deliberately does not do. */
        c.log("[audio] unsupported player source (locator=%u format=%u)",
              c.read32(locator + 0), c.read32(format + 0));
        c.set_result(kFeatureUnsupported);
        return;
    }

    SlObject *o = new_object(c, SlObject::Kind::Player,
                             {Iface::Play, Iface::BufferQueue, Iface::AndroidConfig});
    if (o == nullptr) { c.set_result(kMemoryFailure); return; }

    o->channels = (int)c.read32(format + 4);
    o->rate = (int)(c.read32(format + 8) / 1000u); /* milliHz -> Hz */
    o->bits = (int)c.read32(format + 12);
    if (o->channels <= 0 || o->channels > 8 || o->rate <= 0 ||
        (o->bits != 8 && o->bits != 16)) {
        c.log("[audio] refusing odd PCM format: %d ch, %d Hz, %d bit", o->channels, o->rate,
              o->bits);
        c.set_result(kFeatureUnsupported);
        return;
    }

    c.log("[audio] player created: %d ch, %d Hz, %d bit", o->channels, o->rate, o->bits);
    if (g_active_player == nullptr) {
        g_active_player = o;
        audio::configure(o->channels, o->rate, o->bits);
    }
    c.write32(out, o->handle[(int)Iface::Object]);
    c.set_result(kSuccess);
}

/* Spec order: CreateLEDDevice, CreateVibraDevice, CreateAudioPlayer,
 * CreateAudioRecorder, CreateMidiPlayer, CreateListener, Create3DGroup,
 * CreateOutputMix, CreateMetadataExtractor, CreateExtensionObject,
 * QueryNumSupportedInterfaces, QuerySupportedInterfaces,
 * QueryNumSupportedExtensions, QuerySupportedExtension, IsExtensionSupported. */
void build_engine_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t player =
        make_guest_callback("SL.Engine.CreateAudioPlayer", engine_create_audio_player);
    const std::uint32_t mix =
        make_guest_callback("SL.Engine.CreateOutputMix", engine_create_output_mix);
    const std::uint32_t nop = make_guest_callback("SL.Engine.unsupported", sl_unsupported);
    const std::uint32_t slots[15] = {
        nop, nop, player, nop, nop, nop, nop, mix, nop, nop, nop, nop, nop, nop, nop,
    };
    for (int i = 0; i < 15; ++i) c.write32(addr + 4u * i, slots[i]);
}

/* --- SLPlayItf ------------------------------------------------------------- */

void play_set_state(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr) { c.set_result(kParameterInvalid); return; }
    o->play_state = c.arg(1);
    if (o == g_active_player) audio::set_playing(o->play_state == kPlayStatePlaying);
    c.set_result(kSuccess);
}

void play_get_state(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr || c.arg(1) == 0) { c.set_result(kParameterInvalid); return; }
    c.write32(c.arg(1), o->play_state);
    c.set_result(kSuccess);
}

/* Spec order: SetPlayState, GetPlayState, GetDuration, GetPosition,
 * RegisterCallback, SetCallbackEventsMask, GetCallbackEventsMask,
 * SetMarkerPosition, ClearMarkerPosition, GetMarkerPosition,
 * SetPositionUpdatePeriod, GetPositionUpdatePeriod. */
void build_play_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t set = make_guest_callback("SL.Play.SetPlayState", play_set_state);
    const std::uint32_t get = make_guest_callback("SL.Play.GetPlayState", play_get_state);
    /* The mask/marker setters must SUCCEED, not report unsupported: the game
     * configures them during setup and treats a failure as a fatal audio init. */
    const std::uint32_t ok = make_guest_callback("SL.Play.ok", [](GuestCall &g) {
        g.set_result(kSuccess);
    });
    const std::uint32_t nop = make_guest_callback("SL.Play.unsupported", sl_unsupported);
    const std::uint32_t slots[12] = {
        set, get, nop, nop, ok, ok, ok, ok, ok, nop, ok, nop,
    };
    for (int i = 0; i < 12; ++i) c.write32(addr + 4u * i, slots[i]);
}

/* --- SLBufferQueueItf ------------------------------------------------------ */

void bq_enqueue(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    const std::uint32_t buf = c.arg(1);
    const std::uint32_t size = c.arg(2);
    if (o == nullptr || buf == 0 || size == 0) { c.set_result(kParameterInvalid); return; }
    if (o != g_active_player) { c.set_result(kSuccess); return; } /* inert player */

    /* Copy out of guest memory now: the audio thread must never read it, and
     * OpenSL lets the game refill the buffer as soon as we report it played. */
    const void *p = c.ptr(buf, size);
    if (p == nullptr) { c.set_result(kParameterInvalid); return; }
    c.set_result(audio::enqueue(p, size) ? kSuccess : kMemoryFailure);
}

void bq_clear(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    if (o == nullptr) { c.set_result(kParameterInvalid); return; }
    if (o == g_active_player) audio::clear();
    c.set_result(kSuccess);
}

/* SLBufferQueueState { SLuint32 count; SLuint32 playIndex; } */
void bq_get_state(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    SlObject *o = find(c.arg(0));
    const std::uint32_t out = c.arg(1);
    if (o == nullptr || out == 0) { c.set_result(kParameterInvalid); return; }
    c.write32(out + 0, o == g_active_player ? audio::queued_count() : 0);
    c.write32(out + 4, 0);
    c.set_result(kSuccess);
}

/* Runs on a guest thread of its own for the rest of the session: block until a
 * buffer has finished playing, hand that completion to the game, repeat.
 *
 * A thread rather than a call from the frame loop because the game's sink both
 * refills the buffer queue and wakes Wwise's audio thread from inside this
 * callback -- see the note on audio::wait_for_completion(). Delivering it only
 * between frames made the audio engine's progress depend on onDrawFrame
 * returning, and AK::SoundEngine::UnloadBank (called from onDrawFrame, blocking
 * until the bank's last voice dies) closed that loop into a hard deadlock.
 * Real Android delivers this on OpenSL's own thread for the same reason. */
void bq_pump_thread(GuestCall &c) {
    c.log("[audio] buffer-queue callback thread running (guest tid %u)", guest_tls::self_id);
    std::uint32_t delivered = 0;
    for (;;) {
        if (!audio::wait_for_completion()) break; /* session shutting down */

        /* Reset the tick budget on every iteration. The pump thread is a
         * well-behaved long-lived worker (not a stuck loop), so it must not
         * trip kTickBudget after a few minutes of audio processing. Without
         * this, Wwise's audio thread accumulates ticks from the nested JIT
         * calls inside call_guest, eventually exceeding the 20B budget and
         * halting -- the "tick budget exhausted" crash. */
        c.note_blocked();

        std::uint32_t cb, self, ctx;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            if (g_active_player == nullptr) continue;
            cb = g_active_player->bq_callback;
            self = g_active_player->handle[(int)Iface::BufferQueue];
            ctx = g_active_player->bq_context;
        }
        if (cb == 0) continue;
        /* g_lock is deliberately NOT held here: the callback calls straight back
         * into Enqueue and GetState, which take it. */
        const std::uint32_t args[2] = {self, ctx};
        c.call_guest(cb, args, 2);
        ++delivered;
    }
    c.log("[audio] buffer-queue callback thread exiting after %u callbacks", delivered);
    c.set_result(0);
}

void bq_register_callback(GuestCall &c) {
    bool start_pump = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        SlObject *o = find(c.arg(0));
        if (o == nullptr) { c.set_result(kParameterInvalid); return; }
        o->bq_callback = c.arg(1);
        o->bq_context = c.arg(2);
        c.log("[audio] BufferQueue.RegisterCallback(fn=0x%08x ctx=0x%08x)", c.arg(1), c.arg(2));
        start_pump = o == g_active_player && c.arg(1) != 0 && g_pump_tid == 0 &&
                     g_pump_trampoline != 0;
    }
    /* Spawned with g_lock dropped -- the new thread wants it immediately. */
    if (start_pump) {
        const std::uint32_t tid = c.spawn_thread(g_pump_trampoline, 0);
        std::lock_guard<std::mutex> lk(g_lock);
        g_pump_tid = tid;
        if (tid == 0) {
            c.log("[audio] could not start the buffer-queue callback thread -- falling back to "
                  "the frame loop, which cannot survive a synchronous UnloadBank");
        }
    }
    c.set_result(kSuccess);
}

/* Spec order: Enqueue, Clear, GetState, RegisterCallback. */
void build_bq_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t slots[4] = {
        make_guest_callback("SL.BufferQueue.Enqueue", bq_enqueue),
        make_guest_callback("SL.BufferQueue.Clear", bq_clear),
        make_guest_callback("SL.BufferQueue.GetState", bq_get_state),
        make_guest_callback("SL.BufferQueue.RegisterCallback", bq_register_callback),
    };
    for (int i = 0; i < 4; ++i) c.write32(addr + 4u * i, slots[i]);
    /* Minted here with the rest: make_guest_callback sizes a table that is read
     * without a lock once guest threads are running, so every stub has to exist
     * before then. This one is never put in a vtable -- it is the entry point
     * the callback thread is spawned at. */
    g_pump_trampoline = make_guest_callback("SL.BufferQueue.pump", bq_pump_thread);
}

/* --- the remaining two interfaces ------------------------------------------
 *
 * AndroidConfiguration only ever receives stream-type hints, and
 * AudioIODeviceCapabilities is queried for devices we do not model. Both answer
 * successfully/emptily rather than failing, because the game treats a failed
 * configuration as a failed audio init. */
void build_config_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t ok = make_guest_callback("SL.AndroidConfig.ok", [](GuestCall &g) {
        g.set_result(kSuccess);
    });
    c.write32(addr + 0, ok); /* SetConfiguration */
    c.write32(addr + 4, ok); /* GetConfiguration */
}

void build_caps_vtable(GuestCall &c, std::uint32_t addr) {
    const std::uint32_t nop = make_guest_callback("SL.IODeviceCaps.unsupported", sl_unsupported);
    for (int i = 0; i < 11; ++i) c.write32(addr + 4u * i, nop);
}

/* --- construction ---------------------------------------------------------- */

bool build_vtables(GuestCall &c) {
    if (g_vtables_built) return true;
    struct { Iface kind; int slots; void (*build)(GuestCall &, std::uint32_t); } kTables[] = {
        {Iface::Object, 10, build_object_vtable},
        {Iface::Engine, 15, build_engine_vtable},
        {Iface::Play, 12, build_play_vtable},
        {Iface::BufferQueue, 4, build_bq_vtable},
        {Iface::AndroidConfig, 2, build_config_vtable},
        {Iface::IODeviceCaps, 11, build_caps_vtable},
    };
    for (const auto &t : kTables) {
        const std::uint32_t addr = galloc(c, (std::uint32_t)t.slots * 4u);
        if (addr == 0) return false;
        t.build(c, addr);
        g_vtable[(int)t.kind] = addr;
    }
    g_vtables_built = true;
    return true;
}

/* slCreateEngine(pEngine, numOptions, pEngineOptions, numInterfaces,
 *                pInterfaceIds, pInterfaceRequired) */
void sl_create_engine(GuestCall &c) {
    std::lock_guard<std::mutex> lk(g_lock);
    const std::uint32_t out = c.arg(0);
    if (out == 0) { c.set_result(kParameterInvalid); return; }
    if (!build_vtables(c)) {
        c.log("[audio] out of guest heap building the OpenSL vtables");
        c.write32(out, 0);
        c.set_result(kMemoryFailure);
        return;
    }
    SlObject *o = new_object(c, SlObject::Kind::Engine, {Iface::Engine, Iface::IODeviceCaps});
    if (o == nullptr) {
        c.write32(out, 0);
        c.set_result(kMemoryFailure);
        return;
    }
    c.write32(out, o->handle[(int)Iface::Object]);
    c.log("[audio] slCreateEngine -> engine object 0x%08x", o->handle[(int)Iface::Object]);
    c.set_result(kSuccess);
}

}  // namespace

/* Gives one imported SL_IID_* symbol a distinct, non-zero value. The slot holds
 * a pointer to a tagged 16-byte UUID inside itself, so the guest sees a real
 * SLInterfaceID and GetInterface can tell the five apart. Called from
 * initialize_data_imports(). */
bool initialize_sl_interface_id(pvz2_elf_image_t *img, const char *name, std::uint32_t addr) {
    for (const auto &e : kIidNames) {
        if (std::strcmp(name, e.name) != 0) continue;
        const std::uint32_t uuid = addr + 8;
        std::uint32_t ptr = uuid;
        std::memcpy(&img->mem[addr], &ptr, 4);
        /* A recognisable, distinct UUID. Nothing dereferences it, but a zeroed
         * one would make two ids compare equal if the guest ever did. */
        std::uint32_t tag[4] = {0x534C4944u, (std::uint32_t)e.kind, 0, 0};
        std::memcpy(&img->mem[uuid], tag, sizeof(tag));
        g_iids[ptr] = e.kind;
        return true;
    }
    return false;
}

/* Fallback only, for the case where the callback thread could not be started.
 *
 * This used to be the whole delivery mechanism, and it cannot be: the callback
 * is what lets Wwise's audio engine make progress at all, so tying it to the
 * frame loop meant a synchronous UnloadBank inside onDrawFrame deadlocked the
 * game outright (see bq_pump_thread). Left in place because losing audio
 * entirely when a thread slot runs out is worse than delivering it late. */
void audio_pump_callbacks() {
    std::uint32_t cb = 0, self = 0, ctx = 0;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_pump_tid != 0) return; /* the callback thread owns this */
        if (g_active_player == nullptr) return;
        cb = g_active_player->bq_callback;
        self = g_active_player->handle[(int)Iface::BufferQueue];
        ctx = g_active_player->bq_context;
    }
    if (cb == 0) return;
    /* Bounded so a callback that enqueues nothing cannot spin the frame. */
    for (int i = 0; i < 32 && audio::take_completion(); ++i) {
        call_guest_between_frames(cb, self, ctx);
    }
}

void register_libopensles(ImportTable &t) {
    t.add("slCreateEngine", sl_create_engine);
    /* The five SL_IID_* symbols are STT_OBJECT -- data, not calls -- so they are
     * not registered here; initialize_sl_interface_id() fills them instead. */
}

}  // namespace sprout

#ifndef SPROUT_LOG_LOG_H
#define SPROUT_LOG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Log channels — each maps to a subsystem. Togglable at runtime. */
enum {
    LOG_CHAN_DEX,      /* JNI, Java hooks, class loading          */
    LOG_CHAN_JIT,      /* dynarmic, runtime, guest threads        */
    LOG_CHAN_GFX,      /* OpenGL ES → GL translation, shaders    */
    LOG_CHAN_AUDIO,    /* Audio engine, OpenSL ES, sound repo     */
    LOG_CHAN_INPUT,    /* SDL input, gamepad, touch mapping       */
    LOG_CHAN_ENGINE,   /* Boot, lifecycle, frame loop            */
    LOG_CHAN_VFS,      /* File system, RSB index, OBB loading    */
    LOG_CHAN_IAP,      /* In-app purchase emulation              */
    LOG_CHAN_VERSION,  /* Version detection, symbol tables        */
    LOG_CHAN_COUNT
};

enum { LOG_LEVEL_DEBUG, LOG_LEVEL_INFO, LOG_LEVEL_WARN, LOG_LEVEL_ERROR };

void log_init(int to_file);
void log_shutdown(void);
void log_write(int channel, int level, const char *fmt, ...);
void log_set_channel_enabled(int channel, int enabled);
int  log_get_channel_enabled(int channel);

/* Per-channel DEBUG-level macros — only print if channel is enabled */
#define LOG_DEX(...)     do { if (log_get_channel_enabled(LOG_CHAN_DEX))     log_write(LOG_CHAN_DEX,     LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_JIT(...)     do { if (log_get_channel_enabled(LOG_CHAN_JIT))     log_write(LOG_CHAN_JIT,     LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_GFX(...)     do { if (log_get_channel_enabled(LOG_CHAN_GFX))     log_write(LOG_CHAN_GFX,     LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_AUDIO(...)   do { if (log_get_channel_enabled(LOG_CHAN_AUDIO))   log_write(LOG_CHAN_AUDIO,   LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_INPUT(...)   do { if (log_get_channel_enabled(LOG_CHAN_INPUT))   log_write(LOG_CHAN_INPUT,   LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_ENGINE(...)  do { if (log_get_channel_enabled(LOG_CHAN_ENGINE))  log_write(LOG_CHAN_ENGINE,  LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_VFS(...)     do { if (log_get_channel_enabled(LOG_CHAN_VFS))     log_write(LOG_CHAN_VFS,     LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_IAP(...)     do { if (log_get_channel_enabled(LOG_CHAN_IAP))     log_write(LOG_CHAN_IAP,     LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define LOG_VERSION(...) do { if (log_get_channel_enabled(LOG_CHAN_VERSION)) log_write(LOG_CHAN_VERSION, LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)

/* Backward-compatible level-based macros (always print, channel=GENERAL) */
#define log_info(...)  log_write(-1, LOG_LEVEL_INFO,  __VA_ARGS__)
#define log_warn(...)  log_write(-1, LOG_LEVEL_WARN,  __VA_ARGS__)
#define log_error(...) log_write(-1, LOG_LEVEL_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif

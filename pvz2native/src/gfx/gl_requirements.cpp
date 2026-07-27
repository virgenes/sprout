#include <sprout/gfx/gl_requirements.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>
#include <glad/gl.h>

static int g_glsl_target = 130;

int pvz2_gl_check_requirements(struct SDL_Window *window) {
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *vendor = (const char *)glGetString(GL_VENDOR);

    std::printf("GL: %s | %s | %s\n",
                vendor ? vendor : "?",
                renderer ? renderer : "?",
                version ? version : "?");

    int major = 0, minor = 0;
    if (version) {
        major = (int)strtol(version, nullptr, 10);
        const char *dot = strchr(version, '.');
        if (dot) minor = (int)strtol(dot + 1, nullptr, 10);
    }

    int ok = 1;
    if (major < PVZ2_REQUIRED_GL_MAJOR || (major == PVZ2_REQUIRED_GL_MAJOR && minor < PVZ2_REQUIRED_GL_MINOR)) {
        ok = 0;
    }

    if (!ok) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "Your GPU does not meet the OpenGL requirements.\n\n"
            "  Detected: OpenGL %d.%d\n"
            "  Required: OpenGL %d.%d\n\n"
            "  Renderer: %s\n  Vendor: %s\n\n"
            "The game cannot run without OpenGL 2.0 or higher.",
            major, minor,
            PVZ2_REQUIRED_GL_MAJOR, PVZ2_REQUIRED_GL_MINOR,
            renderer ? renderer : "?", vendor ? vendor : "?");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Sprout - GL Requirements", msg, window);
        return 0;
    }

    if (major >= 3) {
        g_glsl_target = 130;
    } else {
        g_glsl_target = 120;
    }

    return 1;
}

int pvz2_gl_target_glsl(void) {
    return g_glsl_target;
}

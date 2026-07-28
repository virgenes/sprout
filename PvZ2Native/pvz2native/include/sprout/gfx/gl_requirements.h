#ifndef SPROUT_GFX_GL_REQUIREMENTS_H
#define SPROUT_GFX_GL_REQUIREMENTS_H

#define PVZ2_REQUIRED_GL_MAJOR 2
#define PVZ2_REQUIRED_GL_MINOR 0

struct SDL_Window;

#ifdef __cplusplus
extern "C" {
#endif

int pvz2_gl_check_requirements(struct SDL_Window *window);
int pvz2_gl_target_glsl(void);

#ifdef __cplusplus
}
#endif

#endif

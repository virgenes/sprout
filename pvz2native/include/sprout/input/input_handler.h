#ifndef SPROUT_INPUT_INPUT_HANDLER_H
#define SPROUT_INPUT_INPUT_HANDLER_H

#include <SDL.h>

/* Must be called once before processing events. */
void input_init(void);

/* Process a single SDL event (returns 1 if the event was consumed). */
int input_handle_event(const SDL_Event *e);

/* Call every frame to sync SDL text-input state with the game. */
void input_sync_text(void);

/* Current mouse position in render coordinates. */
int input_mouse_rx(void);
int input_mouse_ry(void);

/* Quit flag (set by double-press pause or SDL_QUIT). */
int input_quit_flag(void);
void input_reset_quit_flag(void);

#endif

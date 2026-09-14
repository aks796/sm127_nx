/* nx_input.h -- Switch touchscreen -> Godot 3 touch events. See nx_input.c.
 * MIT license; see LICENSE. */

#ifndef __NX_INPUT_H__
#define __NX_INPUT_H__

#include <stdint.h>

void nx_input_init(void);          // reads config.touch
void nx_input_update(void);        // per frame, engine thread, after step 1
void nx_input_set_screen(int w, int h);
// 1 while a system applet is up: every finger is lifted and nothing is sent.
// 0 when it closes: fingers already on the panel are ignored until it is clear.
void nx_input_suspend(int on);

// No-ops since the virtual-cursor layer was removed; main.c still calls them.
void nx_input_draw(void);
void nx_input_shutdown(void);
int nx_input_cursor_visible(void);
uint64_t nx_input_masked_buttons(void);
int nx_input_left_stick_masked(void);

#endif

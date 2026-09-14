/* clocks.h -- CPU/GPU clocks for the session. See clocks.c.
 * MIT license; see LICENSE. */

#ifndef __CLOCKS_H__
#define __CLOCKS_H__

void clocks_init(void);             // open clkrst/pcv, record stock rates
void clocks_apply(int focused);     // focused: config targets; else stock
void clocks_tick(int focused);      // ~1 Hz: re-apply if the system reset them
void clocks_loading_done(void);     // boot clock -> gameplay clock (applied on the next tick)
void clocks_frame(int missed_vblank); // game thread, per present: feeds the governor
// A frame running far longer than a frame should (a level load). Puts the CPU
// on the loading clock, which the frame-based governor cannot do by itself.
void clocks_set_stalled(int stalled);
int  clocks_cpu_mhz(void);          // last CPU rate set, MHz (for logs)
void clocks_restore(void);          // on exit

#endif

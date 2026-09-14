/* clocks.c -- CPU/GPU clocks: boosted while loading; while playing, stock
 * unless frames are being missed.
 *
 * sts2_nx boosted the CPU with appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)
 * during loads and after every slow frame. That mode is the system's LOADING
 * profile: CPU 1785 MHz, but GPU 76.8 MHz, and it throttled the GPU through
 * ordinary gameplay. Here the CPU is set directly through clkrst (pcv before
 * 8.0.0) and the GPU is left alone:
 *
 *   boot (until main.c calls clocks_loading_done)  config.boot_cpu_mhz (1785)
 *   gameplay, cpu_mhz -1 (default)                 automatic, see below
 *   gameplay, cpu_mhz 0                            stock (1020)
 *   gameplay, cpu_mhz N                            N
 *   unfocused (HOME, sleep) and exit               stock
 *
 * Automatic: measured on hardware, this Godot 3 build is CPU-bound at the
 * stock clock -- 47-55 fps with hundreds of missed vblanks per 30 s at 1020
 * MHz, 58-60 fps at 1785 (GPU at 307 MHz both times). So the governor starts
 * at stock and steps up one official rate whenever a second of play misses 3+
 * vblanks, and back down after 10 clean seconds. A level where the game keeps
 * up at stock stays at stock.
 *
 * The system resets clocks on dock/undock and sleep, so clocks_tick()
 * re-applies about once a second while focused.
 *
 * MIT license; see LICENSE. */

#include <switch.h>

#include "config.h"
#include "util.h"
#include "clocks.h"

#define MHZ 1000000u
#define CPU_STOCK_HZ (1020 * MHZ)

static const u32 k_cpu_rates[] = {
  612 * MHZ, 714 * MHZ, 816 * MHZ, 918 * MHZ, 1020 * MHZ, 1122 * MHZ,
  1224 * MHZ, 1326 * MHZ, 1428 * MHZ, 1581 * MHZ, 1683 * MHZ, 1785 * MHZ,
};
static const u32 k_gpu_rates[] = {
  307200000u, 384000000u, 460800000u, 537600000u, 614400000u, 691200000u, 768000000u,
};
// The governor's ladder: stock, then official rates up to the boost rate.
static const int k_gov_mhz[] = { 1020, 1224, 1428, 1581, 1785 };
#define GOV_LEVELS (int)(sizeof(k_gov_mhz) / sizeof(*k_gov_mhz))
#define GOV_UP_MISSED 3      // missed vblanks in one second that step up
#define GOV_DOWN_CALM 10     // consecutive clean seconds that step down
#define GOV_UP_SECONDS 2     // consecutive bad seconds before stepping up
#define GOV_MIN_FRAMES 20    // fewer presents than this in a second: see governor_step

static int s_clkrst, s_have_cpu, s_have_gpu;
static int s_cpu_touched, s_gpu_touched;
static volatile int s_loading = 1;
static ClkrstSession s_cpu, s_gpu;
static u32 s_cpu_applied;    // last rate we set, for the pacing log

// Frame counters, written by the game thread, read by the 1 Hz tick.
static volatile u32 s_frames, s_missed;
static u32 s_last_frames, s_last_missed;
static int s_gov_level, s_gov_calm, s_gov_hot, s_gov_slow;
static int s_stalled;   // see clocks_set_stalled

// Largest official rate <= mhz, never above cap_hz.
static u32 pick(const u32 *rates, int n, int mhz, u32 cap_hz) {
  u32 best = rates[0];
  for (int i = 0; i < n; i++)
    if (rates[i] <= (u32)mhz * MHZ && rates[i] <= cap_hz) best = rates[i];
  return best;
}

static Result get_rate(int gpu, u32 *hz) {
  if (s_clkrst) return clkrstGetClockRate(gpu ? &s_gpu : &s_cpu, hz);
  return pcvGetClockRate(gpu ? PcvModule_GPU : PcvModule_CpuBus, hz);
}

static Result set_rate(int gpu, u32 hz) {
  if (s_clkrst) return clkrstSetClockRate(gpu ? &s_gpu : &s_cpu, hz);
  return pcvSetClockRate(gpu ? PcvModule_GPU : PcvModule_CpuBus, hz);
}

static int docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}

// A frame that runs for seconds is a level load: Godot builds the whole scene
// inside one iteration of the main loop. The governor only ever sees COMPLETED
// frames, so it cannot react to that at all -- it is still sitting at whatever
// rate the last second of menu navigation justified while the longest, most
// CPU-bound work of the session runs. One hardware run spent 13.8 s in a single
// frame that way. main.c's stall watchdog now reports it, and it is treated
// exactly like boot: the loading clock, until frames come back.

// 0 = no preference (stock).
static u32 cpu_target(void) {
  int mhz;
  if (s_loading || s_stalled) mhz = config.boot_cpu_mhz;
  else if (config.cpu_mhz < 0) mhz = k_gov_mhz[s_gov_level];
  else                      mhz = config.cpu_mhz;
  if (mhz <= 0) return 0;
  return pick(k_cpu_rates, sizeof(k_cpu_rates) / sizeof(*k_cpu_rates), mhz, 1785 * MHZ);
}

static u32 gpu_target(void) {
  if (config.gpu_mhz <= 0) return 0;
  return pick(k_gpu_rates, sizeof(k_gpu_rates) / sizeof(*k_gpu_rates), config.gpu_mhz,
              docked() ? 768000000u : 460800000u);
}

static u32 gpu_stock(void) { return docked() ? 768000000u : 384000000u; }

void clocks_init(void) {
  if (hosversionAtLeast(8, 0, 0)) {
    if (R_SUCCEEDED(clkrstInitialize())) {
      s_clkrst = 1;
      s_have_cpu = R_SUCCEEDED(clkrstOpenSession(&s_cpu, PcvModuleId_CpuBus, 3));
      s_have_gpu = R_SUCCEEDED(clkrstOpenSession(&s_gpu, PcvModuleId_GPU, 3));
    }
  } else if (R_SUCCEEDED(pcvInitialize())) {
    s_have_cpu = s_have_gpu = 1;
  }

  u32 cpu = 0, gpu = 0;
  if (s_have_cpu) get_rate(0, &cpu);
  if (s_have_gpu) get_rate(1, &gpu);
  s_cpu_applied = cpu;
  debugPrintf("[clk] %s: cpu %s (%u MHz), gpu %s (%u MHz); boot cpu %d, play cpu %s, gpu %d MHz\n",
              s_clkrst ? "clkrst" : "pcv",
              s_have_cpu ? "ok" : "UNAVAILABLE", cpu / MHZ,
              s_have_gpu ? "ok" : "UNAVAILABLE", gpu / MHZ,
              config.boot_cpu_mhz,
              config.cpu_mhz < 0 ? "auto" : config.cpu_mhz == 0 ? "stock" : "fixed",
              config.gpu_mhz);
}

static void apply_one(int gpu, u32 want) {
  u32 cur = 0;
  if (!want || R_FAILED(get_rate(gpu, &cur)) || cur == want) return;
  Result rc = set_rate(gpu, want);
  if (!gpu && R_SUCCEEDED(rc)) s_cpu_applied = want;
  debugPrintf("[clk] %s %u -> %u MHz%s\n", gpu ? "gpu" : "cpu", cur / MHZ, want / MHZ,
              R_SUCCEEDED(rc) ? "" : " FAILED");
}

void clocks_apply(int focused) {
  if (s_have_cpu) {
    u32 want = focused ? cpu_target() : 0;
    if (want) s_cpu_touched = 1;
    // Nothing requested: hand back the stock clock, but only if we ever
    // changed it -- otherwise the system's own setting is left untouched.
    else if (s_cpu_touched) want = CPU_STOCK_HZ;
    apply_one(0, want);
  }
  if (s_have_gpu) {
    u32 want = focused ? gpu_target() : 0;
    if (want) s_gpu_touched = 1;
    else if (s_gpu_touched) want = gpu_stock();
    apply_one(1, want);
  }
}

void clocks_frame(int missed_vblank) {
  s_frames++;
  if (missed_vblank) s_missed++;
}

static void governor_step(void) {
  const u32 f = s_frames - s_last_frames, m = s_missed - s_last_missed;
  s_last_frames += f;
  s_last_missed += m;
  if (s_loading || config.cpu_mhz >= 0) return;

  const int before = s_gov_level;

  // Fewer than GOV_MIN_FRAMES presents in a second used to mean "a load or a
  // pause: no decision". So did a level that simply ran at 7 fps: on hardware
  // a large community level spent 33 s at 6.6 fps on the stock clock and the
  // governor never moved, because no second of it held 20 frames. A load is
  // told apart by the stall flag (no frame for 500 ms, main.c), and a pause
  // menu still presents at 60 Hz. A second that presented frames without
  // stalling is gameplay far below the refresh rate: straight to the top rate,
  // after the same two-second confirmation as any other step up.
  if (f < GOV_MIN_FRAMES) {
    if (f == 0 || s_stalled) {
      s_gov_slow = 0;
      return;
    }
    s_gov_calm = 0;
    s_gov_hot = 0;
    if (++s_gov_slow >= GOV_UP_SECONDS && s_gov_level < GOV_LEVELS - 1) {
      s_gov_level = GOV_LEVELS - 1;
      s_gov_slow = 0;
      debugPrintf("[clk] auto: %d -> %d MHz (%u frames in a second: far below 60 fps)\n",
                  k_gov_mhz[before], k_gov_mhz[s_gov_level], f);
    }
    return;
  }
  s_gov_slow = 0;

  // Up only after GOV_UP_SECONDS bad seconds in a row. A single second with a
  // few missed vblanks is a spike -- a spawn, a sound starting, a GC -- and a
  // faster clock does not remove spikes; it just runs hotter. On hardware the
  // windows the old rule spent at 1785 MHz were no smoother than the ones at
  // 1428 (59.5 fps with 21 misses vs 59.8 with 5-8), while the console warmed.
  if (m >= GOV_UP_MISSED) {
    if (++s_gov_hot >= GOV_UP_SECONDS && s_gov_level < GOV_LEVELS - 1) {
      s_gov_level++;
      s_gov_hot = 0;
    }
    s_gov_calm = 0;
  } else if (m == 0) {
    s_gov_hot = 0;
    if (++s_gov_calm >= GOV_DOWN_CALM && s_gov_level > 0) {
      s_gov_level--;
      s_gov_calm = 0;
    }
  } else {
    s_gov_calm = 0;
    s_gov_hot = 0;
  }
  if (s_gov_level != before)
    debugPrintf("[clk] auto: %d -> %d MHz (%u missed vblank(s) in %u frames)\n",
                k_gov_mhz[before], k_gov_mhz[s_gov_level], m, f);
}

void clocks_tick(int focused) {
  if (!focused) return;
  governor_step();
  clocks_apply(1);
}

void clocks_loading_done(void) {
  s_loading = 0;
}

void clocks_set_stalled(int stalled) {
  stalled = !!stalled;
  if (s_stalled == stalled) return;
  s_stalled = stalled;
  if (!s_have_cpu) return;
  debugPrintf("[clk] %s\n", stalled
              ? "long frame: CPU to the loading clock"
              : "frames resumed: governor back in charge");
  clocks_apply(1);   // immediately, not at the next ~1 Hz tick
}

int clocks_cpu_mhz(void) {
  return (int)(s_cpu_applied / MHZ);
}

void clocks_restore(void) {
  if (s_have_cpu && s_cpu_touched) set_rate(0, CPU_STOCK_HZ);
  if (s_have_gpu && s_gpu_touched) set_rate(1, gpu_stock());
  if (s_clkrst) {
    if (s_have_cpu) clkrstCloseSession(&s_cpu);
    if (s_have_gpu) clkrstCloseSession(&s_gpu);
    clkrstExit();
  } else if (s_have_cpu) {
    pcvExit();
  }
}

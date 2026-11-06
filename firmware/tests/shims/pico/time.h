// Host-test stand-in for pico/time.h: a monotonic fake clock the test moves.
#ifndef SHIM_PICO_TIME_H_
#define SHIM_PICO_TIME_H_
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t absolute_time_t;
extern uint64_t shim_now_us;
static inline absolute_time_t get_absolute_time(void) { return shim_now_us; }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return shim_now_us + (uint64_t)ms * 1000u; }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return (int64_t)(to - from); }
// The wait "times out" at once: the fake clock jumps to the deadline.
static inline bool best_effort_wfe_or_timeout(absolute_time_t timeout) {
    if (timeout > shim_now_us) shim_now_us = timeout;
    return true;
}
#endif

#include "fake_clock.h"
static uint32_t s_now;
void fake_clock_set(uint32_t ms) { s_now = ms; }
void fake_clock_add(uint32_t ms) { s_now += ms; }
uint32_t fake_clock_now(void) { return s_now; }

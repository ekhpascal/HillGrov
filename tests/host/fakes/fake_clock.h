#pragma once
#include <stdint.h>
void     fake_clock_set(uint32_t ms);
void     fake_clock_add(uint32_t ms);
uint32_t fake_clock_now(void);

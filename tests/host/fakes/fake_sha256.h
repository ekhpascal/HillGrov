#pragma once
#include <stdint.h>
#include <stddef.h>

/* Matches wa_sha256_fn's shape exactly, so it can be passed straight into
 * web_auth_init(). */
void fake_sha256(const uint8_t *in, size_t n, uint8_t out[32]);

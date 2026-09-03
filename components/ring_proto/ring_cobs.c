#include "ring_proto.h"

size_t ring_cobs_encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t out_idx = 0;
    size_t block_start = 0;

    for (size_t i = 0; i <= n; i++) {
        if (i == n || in[i] == 0) {
            size_t block_len = i - block_start;

            if (block_len > 254) {
                block_len = 254;
                i = block_start + 254 - 1;  /* Adjust: will be incremented by loop */
            }

            out[out_idx++] = (uint8_t)(block_len + 1);

            for (size_t j = 0; j < block_len; j++) {
                out[out_idx++] = in[block_start + j];
            }

            block_start = i + 1;
        }
    }

    return out_idx;
}

int ring_cobs_decode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t out_idx = 0;
    size_t in_idx = 0;

    while (in_idx < n) {
        uint8_t code = in[in_idx++];

        if (code == 0) {
            return -1;
        }

        uint8_t block_len = code - 1;

        if (in_idx + block_len > n) {
            return -1;
        }

        for (uint8_t j = 0; j < block_len; j++) {
            uint8_t byte = in[in_idx++];
            if (byte == 0) {
                return -1;
            }
            out[out_idx++] = byte;
        }

        /* Add zero if more input follows (another code byte after this block's data) */
        if (code < 0xFF && in_idx < n) {
            out[out_idx++] = 0;
        }
    }

    return (int)out_idx;
}

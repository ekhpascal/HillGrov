#include "ring_proto.h"

size_t ring_cobs_encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t out_idx = 0;
    size_t block_start = 0;

    for (size_t i = 0; i <= n; i++) {
        /* Emit a block when we hit a zero or end of input */
        if (i == n || in[i] == 0) {
            size_t block_len = i - block_start;

            /* Cap block at 254 bytes per COBS spec */
            if (block_len > 254) {
                block_len = 254;
                i = block_start + 254;  /* Backtrack to process remainder */
            }

            /* Write code byte: length + 1 (to distinguish from zero) */
            out[out_idx++] = (uint8_t)(block_len + 1);

            /* Write block data */
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

        /* Code byte must not be zero (malformed) */
        if (code == 0) {
            return -1;
        }

        /* code byte value minus 1 = number of data bytes */
        uint8_t block_len = code - 1;

        /* Validate we have enough input */
        if (in_idx + block_len > n) {
            return -1;
        }

        /* Copy block data and check for embedded zeros */
        for (uint8_t j = 0; j < block_len; j++) {
            uint8_t byte = in[in_idx++];
            if (byte == 0) {
                return -1;  /* Embedded zero is malformed */
            }
            out[out_idx++] = byte;
        }

        /* If code < 0xFF, add a zero to output (unless we're at end of input) */
        if (code < 0xFF && in_idx < n) {
            out[out_idx++] = 0;
        }
    }

    return (int)out_idx;
}

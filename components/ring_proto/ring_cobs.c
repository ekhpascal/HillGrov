#include "ring_proto.h"

size_t ring_cobs_encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t out_idx = 0;
    size_t code_pos = out_idx++;  /* Reserve space for first code byte */
    uint8_t count = 0;

    for (size_t i = 0; i < n; i++) {
        if (in[i] == 0) {
            /* Real zero: finalize current block and start new one */
            out[code_pos] = count + 1;
            code_pos = out_idx++;  /* Reserve space for next code byte */
            count = 0;
        } else {
            /* Non-zero byte: add to current block */
            out[out_idx++] = in[i];
            count++;
            if (count == 254) {
                /* Forced split at 254 bytes: finalize and start new block (do NOT consume input) */
                out[code_pos] = 0xFF;
                code_pos = out_idx++;  /* Reserve space for next code byte */
                count = 0;
            }
        }
    }

    /* Always finalize the last block (even if empty) */
    out[code_pos] = count + 1;

    return out_idx;
}

int ring_cobs_decode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t in_idx = 0;
    size_t out_idx = 0;

    while (in_idx < n) {
        uint8_t code = in[in_idx++];

        /* Code byte must not be zero (malformed) */
        if (code == 0) {
            return -1;
        }

        uint8_t block_len = code - 1;

        /* Validate we have enough input for this block's data */
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

        /* Add implicit zero after block if: code < 0xFF (not a continuation) AND more input follows */
        if (code < 0xFF && in_idx < n) {
            out[out_idx++] = 0;
        }
    }

    return (int)out_idx;
}

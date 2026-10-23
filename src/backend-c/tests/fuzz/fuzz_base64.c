/* Capacity/canary invariants for both real decoders, including failed input. */
#include "magitrickle/hash.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!size) { return 0; }
    size_t cap = data[0] % 65u;
    for (unsigned mode = 0; mode < 2; mode++) {
        uint8_t out[66]; memset(out, 0xa5, sizeof(out));
        size_t n = 99;
        int rc = mode ? mt_base64url_decode((const char *)data + 1, size - 1, out + 1, cap, &n) :
                        mt_base64_decode((const char *)data + 1, size - 1, out + 1, cap, &n);
        if (n > cap || (rc != 0 && n != 0) || out[0] != 0xa5) { abort(); }
        for (size_t i = cap + 1; i < sizeof(out); i++) { if (out[i] != 0xa5) { abort(); } }
        if (rc != 0) { for (size_t i = 1; i <= cap; i++) { if (out[i] != 0xa5) { abort(); } } }
    }
    return 0;
}

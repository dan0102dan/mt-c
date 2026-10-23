/* Base64 (RFC 4648 §4 standard-with-padding, §5 URL-safe-no-padding).
 * See hash.h.
 */
#include "magitrickle/hash.h"

#include <stdbool.h>
#include <string.h>

static const char std_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char url_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

size_t mt_base64_encoded_len(size_t in_len) {
    return ((in_len + 2) / 3) * 4;
}

size_t mt_base64url_encoded_len(size_t in_len) {
    size_t full = in_len / 3;
    size_t rem = in_len % 3;
    size_t len = full * 4;
    if (rem == 1) { len += 2; }
    if (rem == 2) { len += 3; }
    return len;
}

static void encode_core(const uint8_t *data, size_t len, const char *alphabet, bool pad,
                        char *out) {
    size_t i = 0;
    size_t o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out[o++] = alphabet[(v >> 18) & 0x3f];
        out[o++] = alphabet[(v >> 12) & 0x3f];
        out[o++] = alphabet[(v >> 6) & 0x3f];
        out[o++] = alphabet[v & 0x3f];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out[o++] = alphabet[(v >> 18) & 0x3f];
        out[o++] = alphabet[(v >> 12) & 0x3f];
        if (pad) {
            out[o++] = '=';
            out[o++] = '=';
        }
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out[o++] = alphabet[(v >> 18) & 0x3f];
        out[o++] = alphabet[(v >> 12) & 0x3f];
        out[o++] = alphabet[(v >> 6) & 0x3f];
        if (pad) {
            out[o++] = '=';
        }
    }
    out[o] = '\0';
}

void mt_base64_encode(const uint8_t *data, size_t len, char *out) {
    encode_core(data, len, std_alphabet, true, out);
}

void mt_base64url_encode(const uint8_t *data, size_t len, char *out) {
    encode_core(data, len, url_alphabet, false, out);
}

static int decode_char(char c, const char *alphabet, uint8_t *val) {
    for (uint8_t i = 0; i < 64; i++) {
        if (alphabet[i] == c) {
            *val = i;
            return 0;
        }
    }
    return -1;
}

/* Check the complete input and capacity before touching the output. This API
 * is also used for on-disk secrets, whose contents are not necessarily valid. */
static int decode_core(const char *in, size_t in_len, const char *alphabet, bool padded,
                       uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!out_len) { return -1; }
    *out_len = 0;
    if ((!in && in_len) || (!out && out_cap)) { return -1; }
    size_t data_len = in_len;
    while (data_len && in[data_len - 1] == '=') { data_len--; }
    size_t padding = in_len - data_len;
    size_t rem = data_len % 4;
    if (rem == 1 || (padded ? (in_len % 4 != 0 || padding > 2 ||
        padding != (rem ? 4 - rem : 0)) : padding != 0)) { return -1; }
    /* Division before multiplication keeps this bounded even at SIZE_MAX. */
    size_t needed = (data_len / 4) * 3 + (rem ? rem - 1 : 0);
    if (needed > out_cap || (needed && !out)) { return -1; }
    uint8_t last = 0;
    for (size_t i = 0; i < data_len; i++) {
        if (decode_char(in[i], alphabet, &last) != 0) { return -1; }
    }
    if ((rem == 2 && (last & 15u)) || (rem == 3 && (last & 3u))) { return -1; }
    uint32_t acc = 0;
    unsigned bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < data_len; i++) {
        uint8_t v = 0;
        (void)decode_char(in[i], alphabet, &v);
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    *out_len = o;
    return 0;
}

int mt_base64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                     size_t *out_len) {
    return decode_core(in, in_len, std_alphabet, true, out, out_cap, out_len);
}

int mt_base64url_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap,
                        size_t *out_len) {
    return decode_core(in, in_len, url_alphabet, false, out, out_cap, out_len);
}

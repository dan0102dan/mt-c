#include "magitrickle/quic.h"

#include <string.h>

static bool varint(const uint8_t *bytes, size_t length, size_t *offset, uint64_t *out)
{
    if (*offset >= length) { return false; }
    size_t count = (size_t)1u << (bytes[*offset] >> 6);
    if (count > length - *offset) { return false; }
    uint64_t value = bytes[*offset] & 0x3fu;
    for (size_t i = 1; i < count; i++) { value = (value << 8) | bytes[*offset + i]; }
    *offset += count;
    *out = value;
    return true;
}

static bool cid(const uint8_t *bytes, size_t length, size_t *offset,
                  uint8_t *count, uint8_t out[MT_QUIC_MAX_CID])
{
    if (*offset >= length) { return false; }
    *count = bytes[(*offset)++];
    if (*count > MT_QUIC_MAX_CID || *count > length - *offset) { return false; }
    memcpy(out, bytes + *offset, *count);
    *offset += *count;
    return true;
}

static bool same_ids(const mt_quic_packet_t *a, const mt_quic_packet_t *b)
{
    return a->version == b->version && a->dcid_len == b->dcid_len &&
        a->scid_len == b->scid_len && memcmp(a->dcid, b->dcid, a->dcid_len) == 0 &&
        memcmp(a->scid, b->scid, a->scid_len) == 0;
}

bool mt_quic_parse(const uint8_t *bytes, size_t length, mt_quic_datagram_t *out)
{
    if (!out) { return false; }
    memset(out, 0, sizeof(*out));
    if (!bytes || !length) { return false; }
    bool initial_only = length >= 1200;
    size_t offset = 0;
    while (offset < length) {
        if (out->count == MT_QUIC_MAX_PACKETS) { goto invalid; }
        mt_quic_packet_t *packet = &out->packets[out->count];
        packet->offset = offset;
        uint8_t flags = bytes[offset++];
        if (!(flags & 0x80u)) {
            /* Short header has no length/CID-length field and must be last.
             * The caller additionally checks the known destination CID and
             * the corresponding packet-number/header-protection sample. */
            if (!(flags & 0x40u) || length - packet->offset < 21) { goto invalid; }
            packet->type = MT_QUIC_SHORT;
            packet->length = length - packet->offset;
            out->count++;
            initial_only = false;
            break;
        }
        if (length - offset < 4) { goto invalid; }
        packet->version = ((uint32_t)bytes[offset] << 24) |
            ((uint32_t)bytes[offset + 1] << 16) |
            ((uint32_t)bytes[offset + 2] << 8) | bytes[offset + 3];
        offset += 4;
        if (!cid(bytes, length, &offset, &packet->dcid_len, packet->dcid) ||
            !cid(bytes, length, &offset, &packet->scid_len, packet->scid)) { goto invalid; }
        if (!packet->version) {
            /* Version Negotiation has no Length field. Its random unused
             * bits do not have the ordinary fixed-bit requirement. */
            if (out->count || length - offset < 4 || (length - offset) % 4 != 0) { goto invalid; }
            packet->type = MT_QUIC_VERSION_NEGOTIATION;
            packet->length = length - packet->offset;
            out->count++;
            initial_only = false;
            break;
        }
        if (!(flags & 0x40u) || (packet->version != MT_QUIC_VERSION_1 &&
            packet->version != MT_QUIC_VERSION_2)) { goto invalid; }
        unsigned type = (flags >> 4) & 3u;
        if (packet->version == MT_QUIC_VERSION_2) {
            /* RFC 9369: Retry=00, Initial=01, 0-RTT=10, Handshake=11. */
            type = (type + 3u) & 3u;
        }
        static const mt_quic_packet_type_t types[] = {
            MT_QUIC_INITIAL, MT_QUIC_ZERO_RTT, MT_QUIC_HANDSHAKE, MT_QUIC_RETRY};
        packet->type = types[type];
        if (packet->type == MT_QUIC_RETRY) {
            if (out->count || length - offset < 16) { goto invalid; }
            packet->has_token = length - offset > 16;
            packet->length = length - packet->offset;
            out->count++;
            initial_only = false;
            break;
        }
        if (packet->type == MT_QUIC_INITIAL) {
            uint64_t token_length;
            if (!varint(bytes, length, &offset, &token_length) ||
                token_length > length - offset) { goto invalid; }
            packet->has_token = token_length != 0;
            offset += (size_t)token_length;
        }
        uint64_t payload_length;
        if (!varint(bytes, length, &offset, &payload_length) || payload_length < 20 ||
            payload_length > length - offset) { goto invalid; }
        offset += (size_t)payload_length;
        packet->length = offset - packet->offset;
        if (packet->type != MT_QUIC_INITIAL || packet->has_token || packet->dcid_len < 8 ||
            (out->count && !same_ids(&out->packets[0], packet))) { initial_only = false; }
        out->count++;
    }
    out->initial_only = initial_only;
    return out->count != 0;
invalid:
    memset(out, 0, sizeof(*out));
    return false;
}

#include "greatest.h"
#include "magitrickle/quic.h"

#include <string.h>

/* Framing fixtures intentionally have opaque, unauthenticated payloads.
 * Real encrypted Initial/Handshake packets are covered by the kernel H3 lab. */
static size_t header(uint8_t *out, uint32_t version, unsigned type,
                      unsigned dcid_length, unsigned scid_length)
{
    out[0] = (uint8_t)(0xc0u | (type << 4));
    out[1] = (uint8_t)(version >> 24); out[2] = (uint8_t)(version >> 16);
    out[3] = (uint8_t)(version >> 8); out[4] = (uint8_t)version;
    size_t offset = 5;
    out[offset++] = (uint8_t)dcid_length;
    for (unsigned i = 0; i < dcid_length; i++) { out[offset++] = (uint8_t)(0x30u + i); }
    out[offset++] = (uint8_t)scid_length;
    for (unsigned i = 0; i < scid_length; i++) { out[offset++] = (uint8_t)(0x60u + i); }
    return offset;
}

static size_t packet(uint8_t *out, uint32_t version, unsigned type,
                      bool initial, bool token, size_t size)
{
    size_t offset = header(out, version, type, 8, 8);
    if (initial) {
        out[offset++] = token ? 1 : 0;
        if (token) { out[offset++] = 0xa5; }
    }
    size_t payload = size - offset - 2;
    out[offset++] = (uint8_t)(0x40u | (payload >> 8));
    out[offset++] = (uint8_t)payload;
    memset(out + offset, 0xaa, payload);
    return size;
}

TEST initial_versions_and_cids(void)
{
    uint8_t bytes[1200]; mt_quic_datagram_t parsed;
    const uint32_t versions[] = {MT_QUIC_VERSION_1, MT_QUIC_VERSION_2};
    for (size_t i = 0; i < 2; i++) {
        packet(bytes, versions[i], (unsigned)i, true, false, sizeof(bytes));
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT(parsed.initial_only); ASSERT_EQ(1u, parsed.count);
        ASSERT_EQ(MT_QUIC_INITIAL, parsed.packets[0].type);
        ASSERT_EQ(versions[i], parsed.packets[0].version);
        ASSERT_EQ(8, parsed.packets[0].dcid_len); ASSERT_EQ(8, parsed.packets[0].scid_len);
        ASSERT_EQ(0x30, parsed.packets[0].dcid[0]); ASSERT_EQ(0x67, parsed.packets[0].scid[7]);
        ASSERT_EQ(0u, parsed.packets[0].offset); ASSERT_EQ(sizeof(bytes), parsed.packets[0].length);
    }
    PASS();
}

TEST every_truncated_prefix_is_rejected(void)
{
    uint8_t bytes[1200]; mt_quic_datagram_t parsed;
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); i++) {
        memset(&parsed, 0xff, sizeof(parsed));
        ASSERT_FALSE(mt_quic_parse(bytes, i, &parsed));
        ASSERT_EQ(0u, parsed.count); ASSERT_FALSE(parsed.initial_only);
    }
    ASSERT_FALSE(mt_quic_parse(NULL, sizeof(bytes), &parsed));
    ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), NULL));
    PASS();
}

TEST coalesced_packets_cannot_hide_early_data(void)
{
    uint8_t bytes[2400]; mt_quic_datagram_t parsed;
    for (unsigned v2 = 0; v2 < 2; v2++) {
        uint32_t version = v2 ? MT_QUIC_VERSION_2 : MT_QUIC_VERSION_1;
        packet(bytes, version, v2, true, false, 1200);
        packet(bytes + 1200, version, v2, true, false, 1200);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT(parsed.initial_only); ASSERT_EQ(2u, parsed.count);
        ASSERT_EQ(1200u, parsed.packets[1].offset);
        packet(bytes + 1200, version, v2 + 1, false, false, 1200);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_FALSE(parsed.initial_only); ASSERT_EQ(MT_QUIC_ZERO_RTT, parsed.packets[1].type);
        packet(bytes + 1200, version, v2 + 2, false, false, 1200);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_FALSE(parsed.initial_only); ASSERT_EQ(MT_QUIC_HANDSHAKE, parsed.packets[1].type);
        bytes[1200] = 0x40; memset(bytes + 1201, 0, 1199);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_FALSE(parsed.initial_only); ASSERT_EQ(MT_QUIC_SHORT, parsed.packets[1].type);
        bytes[1200] = 0x01;
        ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_EQ(0u, parsed.count);
    }
    PASS();
}

TEST replay_eligibility_is_conservative(void)
{
    uint8_t bytes[2400]; mt_quic_datagram_t parsed;
    packet(bytes, MT_QUIC_VERSION_1, 0, true, true, 1200);
    ASSERT(mt_quic_parse(bytes, 1200, &parsed));
    ASSERT_FALSE(parsed.initial_only); ASSERT(parsed.packets[0].has_token);
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, 1199);
    ASSERT(mt_quic_parse(bytes, 1199, &parsed)); ASSERT_FALSE(parsed.initial_only);
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, 1200);
    packet(bytes + 1200, MT_QUIC_VERSION_1, 0, true, false, 1200);
    bytes[1206] ^= 1;
    ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed)); ASSERT_FALSE(parsed.initial_only);
    packet(bytes + 1200, MT_QUIC_VERSION_2, 1, true, false, 1200);
    ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed)); ASSERT_FALSE(parsed.initial_only);
    size_t offset = header(bytes, MT_QUIC_VERSION_1, 0, 7, 0);
    bytes[offset++] = 0;
    size_t payload = 1200 - offset - 2;
    bytes[offset++] = (uint8_t)(0x40u | (payload >> 8)); bytes[offset++] = (uint8_t)payload;
    memset(bytes + offset, 0xaa, payload);
    ASSERT(mt_quic_parse(bytes, 1200, &parsed)); ASSERT_FALSE(parsed.initial_only);
    PASS();
}

TEST retry_and_version_negotiation_are_not_replayable(void)
{
    uint8_t bytes[128]; mt_quic_datagram_t parsed;
    for (unsigned v2 = 0; v2 < 2; v2++) {
        size_t n = header(bytes, v2 ? MT_QUIC_VERSION_2 : MT_QUIC_VERSION_1, v2 ? 0 : 3, 8, 8);
        memset(bytes + n, 0x5a, 17); n += 17;
        ASSERT(mt_quic_parse(bytes, n, &parsed));
        ASSERT_EQ(MT_QUIC_RETRY, parsed.packets[0].type); ASSERT_FALSE(parsed.initial_only);
        ASSERT(parsed.packets[0].has_token);
        ASSERT_FALSE(mt_quic_parse(bytes, n - 2, &parsed));
    }
    size_t n = header(bytes, 0, 0, 8, 8);
    bytes[0] = 0x80; bytes[n] = bytes[n + 1] = bytes[n + 2] = 0; bytes[n + 3] = 1;
    ASSERT(mt_quic_parse(bytes, n + 4, &parsed));
    ASSERT_EQ(MT_QUIC_VERSION_NEGOTIATION, parsed.packets[0].type); ASSERT_FALSE(parsed.initial_only);
    ASSERT_FALSE(mt_quic_parse(bytes, n + 3, &parsed));
    ASSERT_FALSE(mt_quic_parse(bytes, n, &parsed));
    PASS();
}

TEST unknown_versions_and_invalid_lengths_are_not_prefix_successes(void)
{
    uint8_t bytes[2400]; mt_quic_datagram_t parsed;
    packet(bytes, 0x12345678u, 0, true, false, 1200);
    ASSERT_FALSE(mt_quic_parse(bytes, 1200, &parsed));
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, 1200);
    bytes[0] &= (uint8_t)~0x40u;
    ASSERT_FALSE(mt_quic_parse(bytes, 1200, &parsed));
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, 1200);
    bytes[5] = 21;
    ASSERT_FALSE(mt_quic_parse(bytes, 1200, &parsed));
    size_t n = header(bytes, MT_QUIC_VERSION_1, 0, 8, 8);
    memset(bytes + n, 0xff, 8);
    ASSERT_FALSE(mt_quic_parse(bytes, n + 8, &parsed));
    bytes[n++] = 0;
    memset(bytes + n, 0xff, 8);
    ASSERT_FALSE(mt_quic_parse(bytes, n + 8, &parsed));
    bytes[n] = 19; memset(bytes + n + 1, 0xaa, 19);
    ASSERT_FALSE(mt_quic_parse(bytes, n + 20, &parsed));
    packet(bytes, MT_QUIC_VERSION_1, 0, true, false, 1200);
    bytes[1200] = 0xc0;
    ASSERT_FALSE(mt_quic_parse(bytes, 1201, &parsed)); ASSERT_EQ(0u, parsed.count);
    PASS();
}

TEST descriptor_budget_and_short_packet_sample(void)
{
    uint8_t bytes[17 * 100]; mt_quic_datagram_t parsed;
    for (size_t i = 0; i < 17; i++) { packet(bytes + i * 100, MT_QUIC_VERSION_1, 0, true, false, 100); }
    ASSERT(mt_quic_parse(bytes, 1600, &parsed)); ASSERT(parsed.initial_only);
    ASSERT_EQ(MT_QUIC_MAX_PACKETS, parsed.count);
    ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), &parsed)); ASSERT_EQ(0u, parsed.count);
    memset(bytes, 0, 21); bytes[0] = 0x40;
    ASSERT_FALSE(mt_quic_parse(bytes, 20, &parsed));
    ASSERT(mt_quic_parse(bytes, 21, &parsed)); ASSERT_FALSE(parsed.initial_only);
    ASSERT_EQ(MT_QUIC_SHORT, parsed.packets[0].type);
    PASS();
}

TEST datagram_padding_cannot_hide_another_packet(void)
{
    uint8_t bytes[1200]; mt_quic_datagram_t parsed;
    for (unsigned v2 = 0; v2 < 2; v2++) {
        uint32_t version = v2 ? MT_QUIC_VERSION_2 : MT_QUIC_VERSION_1;
        /* Real aioquic Initial flights include a partial final packet and
         * zero datagram padding outside its protected Length. */
        memset(bytes, 0, sizeof(bytes));
        packet(bytes, version, v2, true, false, 357);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT(parsed.initial_only); ASSERT_EQ(1u, parsed.count);
        ASSERT_EQ(357u, parsed.packets[0].length);
        ASSERT(mt_quic_parse(bytes, 1199, &parsed)); ASSERT_FALSE(parsed.initial_only);
        bytes[1199] = 1;
        ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_EQ(0u, parsed.count); ASSERT_FALSE(parsed.initial_only);
        bytes[1199] = 0;
        packet(bytes + 367, version, v2 + 1, false, false, 100);
        ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_EQ(0u, parsed.count); /* Do not skip padding between packets. */
        memset(bytes + 357, 0, sizeof(bytes) - 357);
        packet(bytes + 357, version, v2 + 1, false, false, 100);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed));
        ASSERT_EQ(2u, parsed.count); ASSERT_EQ(MT_QUIC_ZERO_RTT, parsed.packets[1].type);
        ASSERT_FALSE(parsed.initial_only);
        memset(bytes + 357, 0, sizeof(bytes) - 357);
        packet(bytes, version, v2, true, true, 357);
        ASSERT(mt_quic_parse(bytes, sizeof(bytes), &parsed)); ASSERT_FALSE(parsed.initial_only);
    }
    memset(bytes, 0, sizeof(bytes));
    ASSERT_FALSE(mt_quic_parse(bytes, sizeof(bytes), &parsed));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(initial_versions_and_cids);
    RUN_TEST(every_truncated_prefix_is_rejected);
    RUN_TEST(coalesced_packets_cannot_hide_early_data);
    RUN_TEST(replay_eligibility_is_conservative);
    RUN_TEST(retry_and_version_negotiation_are_not_replayable);
    RUN_TEST(unknown_versions_and_invalid_lengths_are_not_prefix_successes);
    RUN_TEST(descriptor_budget_and_short_packet_sample);
    RUN_TEST(datagram_padding_cannot_hide_another_packet);
    GREATEST_MAIN_END();
}

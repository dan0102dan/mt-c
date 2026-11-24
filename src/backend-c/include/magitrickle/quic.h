/* Bounded QUIC v1/v2 wire classification for transparent routing.
 * This parses visible framing only: packets are NOT authenticated, decrypted
 * or accepted as evidence that a TLS handshake/application request succeeded. */
#ifndef MAGITRICKLE_QUIC_H
#define MAGITRICKLE_QUIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MT_QUIC_MAX_PACKETS 16
#define MT_QUIC_MAX_CID 20
#define MT_QUIC_VERSION_1 UINT32_C(0x00000001)
#define MT_QUIC_VERSION_2 UINT32_C(0x6b3343cf)

typedef enum mt_quic_packet_type {
    MT_QUIC_INITIAL,
    MT_QUIC_ZERO_RTT,
    MT_QUIC_HANDSHAKE,
    MT_QUIC_RETRY,
    MT_QUIC_VERSION_NEGOTIATION,
    MT_QUIC_SHORT
} mt_quic_packet_type_t;

typedef struct mt_quic_packet {
    mt_quic_packet_type_t type;
    uint32_t version;
    uint8_t dcid_len, scid_len;
    uint8_t dcid[MT_QUIC_MAX_CID], scid[MT_QUIC_MAX_CID];
    bool has_token;
    size_t offset, length;
} mt_quic_packet_t;

typedef struct mt_quic_datagram {
    size_t count;
    /* All packets are supported, token-free Initial packets with consistent
     * visible CIDs/version; first DCID >=8 bytes and whole UDP payload >=1200.
     * Transport must also correlate IDs between successive datagrams. */
    bool initial_only;
    mt_quic_packet_t packets[MT_QUIC_MAX_PACKETS];
} mt_quic_datagram_t;

/* False for incomplete/unknown framing or too many packets; clears *out on
 * failure, never exposes a successfully parsed prefix as a safe datagram.
 * Short-header descriptors consume the remainder; their CID length depends
 * on the connection context, so only offset/length/type are reported. */
bool mt_quic_parse(const uint8_t *bytes, size_t length, mt_quic_datagram_t *out);

#endif

/* Regression: compression must survive response repacking (not only the
 * unchanged-UDP fast path). Enforce DNS semantics and wire-size budgets.
 */
#include "greatest.h"
#include "magitrickle/dnswire.h"

#include <stdlib.h>
#include <string.h>

static size_t put_name(uint8_t *out, const char *name)
{
    size_t used = 0;
    while (*name) {
        const char *end = strchr(name, '.');
        size_t n = end ? (size_t)(end - name) : strlen(name);
        out[used++] = (uint8_t)n;
        memcpy(out + used, name, n);
        used += n;
        if (!end) { break; }
        name = end + 1;
    }
    out[used++] = 0;
    return used;
}

static size_t repeated_reply(uint8_t *wire, unsigned answers)
{
    size_t off = 0;
    const uint8_t header[12] = {0x12, 0x34, 0x81, 0x80, 0, 1,
                                (uint8_t)(answers >> 8), (uint8_t)answers,
                                0, 0, 0, 0};
    memcpy(wire + off, header, sizeof(header)); off += sizeof(header);
    off += put_name(wire + off, "long-service-label.example.com");
    wire[off++] = 0; wire[off++] = 1; /* A */
    wire[off++] = 0; wire[off++] = 1; /* IN */
    for (unsigned i = 0; i < answers; i++) {
        static const uint8_t rr[16] = {
            0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 0, 0
        };
        memcpy(wire + off, rr, sizeof(rr));
        wire[off + 15] = (uint8_t)i;
        off += sizeof(rr);
    }
    return off;
}

TEST twenty_a_records_fit_classic_udp(void)
{
    uint8_t wire[512];
    size_t len = repeated_reply(wire, 20);
    ASSERT(len < sizeof(wire));
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(wire, len, &msg));
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    ASSERT_EQ(MT_OK, mt_dns_msg_pack(msg, &packed, &packed_len));
    ASSERT(packed_len <= 512); /* never expand to 1.1 KB without TC */
    ASSERT_EQ(len, packed_len); /* repeated owner names must be pointers */
    mt_dns_msg_t *again = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(packed, packed_len, &again));
    ASSERT_EQ(20u, again->n_answers);
    ASSERT_EQ(0, again->flags & MT_DNS_FLAG_TC);
    for (size_t i = 0; i < again->n_answers; i++) {
        ASSERT_EQ(4u, again->answers[i].rdata_len);
        ASSERT_EQ((uint8_t)i, again->answers[i].rdata[3]);
    }
    free(packed);
    mt_dns_msg_free(again);
    mt_dns_msg_free(msg);
    PASS();
}

TEST two_thousand_a_records_repack_over_tcp(void)
{
    uint8_t *wire = malloc(33000);
    ASSERT(wire != NULL);
    size_t len = repeated_reply(wire, 2000);
    ASSERT(len < 33000);
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(wire, len, &msg));
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    ASSERT_EQ(MT_OK, mt_dns_msg_pack(msg, &packed, &packed_len));
    ASSERT_EQ(len, packed_len);
    ASSERT(packed_len < MT_DNS_MAX_MSG);
    mt_dns_msg_t *again = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(packed, packed_len, &again));
    ASSERT_EQ(2000u, again->n_answers);
    ASSERT_EQ(msg->answers[1999].rdata[3], again->answers[1999].rdata[3]);
    free(packed);
    free(wire);
    mt_dns_msg_free(again);
    mt_dns_msg_free(msg);
    PASS();
}

static bool make_rr(mt_dns_rr_t *rr, const mt_dns_question_t *q,
                    uint16_t type, const uint8_t *rdata, size_t n)
{
    memset(rr, 0, sizeof(*rr));
    memcpy(rr->name, q->name, q->name_len);
    rr->name_len = q->name_len;
    rr->rtype = type;
    rr->rclass = 1;
    rr->ttl = 60;
    rr->rdata = malloc(n);
    if (!rr->rdata) { return false; }
    memcpy(rr->rdata, rdata, n);
    rr->rdata_len = n;
    return true;
}

TEST compress_eligible_rdata_but_not_srv_or_dname(void)
{
    uint8_t query[128] = {0};
    query[5] = 1;
    size_t query_len = 12 + put_name(query + 12, "example.com");
    query[query_len++] = 0; query[query_len++] = 1;
    query[query_len++] = 0; query[query_len++] = 1;
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(query, query_len, &msg));

    msg->answers = calloc(3, sizeof(*msg->answers));
    msg->authority = calloc(1, sizeof(*msg->authority));
    msg->additional = calloc(1, sizeof(*msg->additional));
    ASSERT(msg->answers && msg->authority && msg->additional);

    uint8_t cname[100], mx[100], soa[200], srv[100], dname[100];
    size_t cn = put_name(cname, "alias.example.com");
    mx[0] = 0; mx[1] = 10;
    size_t mn = 2 + put_name(mx + 2, "mail.example.com");
    size_t sn = put_name(soa, "ns.example.com");
    sn += put_name(soa + sn, "hostmaster.example.com");
    memset(soa + sn, 0, 20); sn += 20;
    memset(srv, 0, 6); srv[5] = 80;
    size_t vn = 6 + put_name(srv + 6, "service.example.com");
    size_t dn = put_name(dname, "redirect.example.com");

    ASSERT(make_rr(&msg->answers[0], &msg->questions[0], MT_DNS_TYPE_CNAME, cname, cn));
    msg->n_answers++;
    ASSERT(make_rr(&msg->answers[1], &msg->questions[0], MT_DNS_TYPE_MX, mx, mn));
    msg->n_answers++;
    ASSERT(make_rr(&msg->answers[2], &msg->questions[0], MT_DNS_TYPE_SOA, soa, sn));
    msg->n_answers++;
    ASSERT(make_rr(&msg->authority[0], &msg->questions[0], MT_DNS_TYPE_SRV, srv, vn));
    msg->n_authority++;
    ASSERT(make_rr(&msg->additional[0], &msg->questions[0], 39 /* DNAME */, dname, dn));
    msg->n_additional++;

    uint8_t *packed = NULL;
    size_t packed_len = 0;
    ASSERT_EQ(MT_OK, mt_dns_msg_pack(msg, &packed, &packed_len));
    size_t off = query_len;
    size_t rd_lengths[5];
    for (size_t i = 0; i < 5; i++) {
        ASSERT_EQ(0xc0, packed[off]); /* compressed RR owner */
        ASSERT_EQ(0x0c, packed[off + 1]); /* points at question */
        size_t rdlen = (size_t)((unsigned)packed[off + 10] << 8 | packed[off + 11]);
        rd_lengths[i] = rdlen;
        off += 12 + rdlen;
        ASSERT(off <= packed_len);
    }
    ASSERT_EQ(off, packed_len);
    ASSERT(rd_lengths[0] < cn);
    ASSERT(rd_lengths[1] < mn);
    ASSERT(rd_lengths[2] < sn);
    ASSERT_EQ(vn, rd_lengths[3]); /* SRV target MUST NOT be compressed */
    ASSERT_EQ(dn, rd_lengths[4]); /* DNAME target MUST NOT be compressed */

    mt_dns_msg_t *again = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(packed, packed_len, &again));
    for (size_t i = 0; i < 3; i++) {
        ASSERT_EQ(msg->answers[i].rdata_len, again->answers[i].rdata_len);
        ASSERT_EQ(0, memcmp(msg->answers[i].rdata, again->answers[i].rdata,
                            msg->answers[i].rdata_len));
    }
    ASSERT_EQ(0, memcmp(srv, again->authority[0].rdata, vn));
    ASSERT_EQ(0, memcmp(dname, again->additional[0].rdata, dn));
    mt_dns_msg_free(again);
    mt_dns_msg_free(msg);
    free(packed);
    PASS();
}

TEST compression_pointer_chain_is_parser_safe(void)
{
    uint8_t query[20] = {0};
    query[5] = 1;
    query[12] = 0;
    query[13] = 0; query[14] = 1;
    query[15] = 0; query[16] = 1;
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(query, 17, &msg));
    msg->answers = calloc(60, sizeof(*msg->answers));
    ASSERT(msg->answers != NULL);
    for (size_t i = 0; i < 60; i++) {
        mt_dns_rr_t *rr = &msg->answers[i];
        rr->rtype = MT_DNS_TYPE_A;
        rr->rclass = 1;
        rr->ttl = 60;
        for (size_t j = 0; j <= i; j++) {
            rr->name[2*j] = 1;
            rr->name[2*j + 1] = 'a';
        }
        rr->name[2 * (i + 1)] = 0;
        rr->name_len = 2 * (i + 1) + 1;
        rr->rdata = malloc(4);
        ASSERT(rr->rdata);
        memcpy(rr->rdata, "\x01\x02\x03\x04", 4);
        rr->rdata_len = 4;
        msg->n_answers++;
    }
    uint8_t *packed = NULL;
    size_t len = 0;
    ASSERT_EQ(MT_OK, mt_dns_msg_pack(msg, &packed, &len));
    mt_dns_msg_t *again = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(packed, len, &again));
    ASSERT_EQ(60u, again->n_answers);
    for (size_t i = 0; i < 60; i++) {
        ASSERT_EQ(msg->answers[i].name_len, again->answers[i].name_len);
        ASSERT_EQ(0, memcmp(msg->answers[i].name, again->answers[i].name,
                            msg->answers[i].name_len));
    }
    mt_dns_msg_free(again);
    mt_dns_msg_free(msg);
    free(packed);
    PASS();
}

TEST still_rejects_packets_above_dns_wire_limit(void)
{
    uint8_t *wire = malloc(65000);
    ASSERT(wire);
    size_t len = repeated_reply(wire, 3500);
    ASSERT(len < 65000);
    mt_dns_msg_t *msg = NULL;
    ASSERT_EQ(MT_OK, mt_dns_msg_parse(wire, len, &msg));
    /* Expand raw records into an enormous answer with long distinct owners
     * rather than relying on a thousand identical records to hit the limit. */
    for (size_t i = 0; i < msg->n_answers; i++) {
        size_t n = put_name(msg->answers[i].name, "not-the-query.example.com");
        msg->answers[i].name_len = n;
        msg->answers[i].rdata_len = 16;
        free(msg->answers[i].rdata);
        msg->answers[i].rdata = calloc(16, 1);
        ASSERT(msg->answers[i].rdata);
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT_EQ(MT_ERR_LIMIT, mt_dns_msg_pack(msg, &out, &out_len));
    ASSERT(out == NULL);
    free(wire);
    mt_dns_msg_free(msg);
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(twenty_a_records_fit_classic_udp);
    RUN_TEST(two_thousand_a_records_repack_over_tcp);
    RUN_TEST(compress_eligible_rdata_but_not_srv_or_dname);
    RUN_TEST(compression_pointer_chain_is_parser_safe);
    RUN_TEST(still_rejects_packets_above_dns_wire_limit);
    GREATEST_MAIN_END();
}

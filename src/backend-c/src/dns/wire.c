#include "magitrickle/dnswire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POINTER_JUMPS 32

/* ---- primitive readers (explicit byte order, bounds-checked) ---- */

typedef struct reader {
    const uint8_t *buf;
    size_t len;
    size_t pos;
} reader_t;

static bool rd_u16(reader_t *r, uint16_t *v)
{
    if (r->pos + 2 > r->len) {
        return false;
    }
    *v = (uint16_t)((uint16_t)r->buf[r->pos] << 8 |
                    (uint16_t)r->buf[r->pos + 1]);
    r->pos += 2;
    return true;
}

static bool rd_u32(reader_t *r, uint32_t *v)
{
    if (r->pos + 4 > r->len) {
        return false;
    }
    *v = (uint32_t)r->buf[r->pos] << 24 | (uint32_t)r->buf[r->pos + 1] << 16 |
         (uint32_t)r->buf[r->pos + 2] << 8 | (uint32_t)r->buf[r->pos + 3];
    r->pos += 4;
    return true;
}

/* Decompress a name starting at r->pos; advances r->pos past the name's
 * in-place bytes (stopping after the first pointer). Output: wire-form
 * labels + root, max 255 octets. */
static bool rd_name(reader_t *r, uint8_t *out, size_t *out_len)
{
    size_t pos = r->pos;
    size_t written = 0;
    int jumps = 0;
    size_t after_first_pointer = 0; /* resume position, 0 = none yet */

    for (;;) {
        if (pos >= r->len) {
            return false;
        }
        uint8_t len8 = r->buf[pos];
        if ((len8 & 0xC0) == 0xC0) {
            /* compression pointer */
            if (pos + 2 > r->len) {
                return false;
            }
            if (++jumps > MAX_POINTER_JUMPS) {
                return false;
            }
            size_t target = ((size_t)(len8 & 0x3F) << 8) | r->buf[pos + 1];
            if (after_first_pointer == 0) {
                after_first_pointer = pos + 2;
            }
            if (target >= r->len) {
                return false;
            }
            pos = target;
            continue;
        }
        if ((len8 & 0xC0) != 0) {
            return false; /* 0x40/0x80 label types unsupported (like miekg) */
        }
        if (len8 == 0) {
            if (written + 1 > MT_DNS_MAX_NAME) {
                return false;
            }
            out[written++] = 0;
            *out_len = written;
            r->pos = after_first_pointer != 0 ? after_first_pointer : pos + 1;
            return true;
        }
        /* regular label */
        if (pos + 1 + len8 > r->len) {
            return false;
        }
        if (written + 1 + len8 + 1 > MT_DNS_MAX_NAME + 1) {
            return false; /* name too long */
        }
        out[written++] = len8;
        memcpy(out + written, r->buf + pos + 1, len8);
        written += len8;
        pos += 1 + (size_t)len8;
    }
}

/* ---- rdata canonicalization ---- */

static bool type_has_compressible_rdata(uint16_t t)
{
    switch (t) {
    case MT_DNS_TYPE_NS:
    case 3:  /* MD */
    case 4:  /* MF */
    case MT_DNS_TYPE_CNAME:
    case MT_DNS_TYPE_SOA:
    case 7:  /* MB */
    case 8:  /* MG */
    case 9:  /* MR */
    case MT_DNS_TYPE_PTR:
    case 14: /* MINFO */
    case MT_DNS_TYPE_MX:
    case 17: /* RP */
    case 18: /* AFSDB */
    case 21: /* RT */
    case MT_DNS_TYPE_SRV:
    case 36: /* KX */
    case 39: /* DNAME */
        return true;
    default:
        return false;
    }
}

/* Parse rdata of a compressible type into canonical (decompressed) bytes.
 * r is positioned at rdata start; rd_end = r->pos + rdlength. */
static bool canonicalize_rdata(reader_t *r, size_t rd_end, uint16_t rtype,
                               uint8_t **out, size_t *out_len)
{
    uint8_t tmp[2 * (MT_DNS_MAX_NAME + 1) + 20];
    size_t w = 0;

    /* leading fixed part before the first name */
    size_t fixed = 0;
    switch (rtype) {
    case MT_DNS_TYPE_MX:
    case 18: /* AFSDB */
    case 21: /* RT */
    case 36: /* KX */
        fixed = 2;
        break;
    case MT_DNS_TYPE_SRV:
        fixed = 6;
        break;
    default:
        fixed = 0;
        break;
    }
    if (r->pos + fixed > rd_end) {
        return false;
    }
    memcpy(tmp + w, r->buf + r->pos, fixed);
    w += fixed;
    r->pos += fixed;

    /* one or two names */
    int names = 1;
    if (rtype == MT_DNS_TYPE_SOA || rtype == 14 /*MINFO*/ ||
        rtype == 17 /*RP*/) {
        names = 2;
    }
    for (int i = 0; i < names; i++) {
        uint8_t name[MT_DNS_MAX_NAME + 1];
        size_t name_len = 0;
        if (!rd_name(r, name, &name_len) || r->pos > rd_end) {
            return false;
        }
        memcpy(tmp + w, name, name_len);
        w += name_len;
    }

    /* trailing fixed part (SOA: 20 bytes of counters) */
    if (rtype == MT_DNS_TYPE_SOA) {
        if (r->pos + 20 > rd_end) {
            return false;
        }
        memcpy(tmp + w, r->buf + r->pos, 20);
        w += 20;
        r->pos += 20;
    }

    if (r->pos != rd_end) {
        return false; /* trailing junk inside rdata */
    }

    *out = malloc(w > 0 ? w : 1);
    if (*out == NULL) {
        return false;
    }
    memcpy(*out, tmp, w);
    *out_len = w;
    return true;
}

static bool parse_rr(reader_t *r, mt_dns_rr_t *rr)
{
    memset(rr, 0, sizeof(*rr));
    if (!rd_name(r, rr->name, &rr->name_len)) {
        return false;
    }
    uint16_t rdlength;
    if (!rd_u16(r, &rr->rtype) || !rd_u16(r, &rr->rclass) ||
        !rd_u32(r, &rr->ttl) || !rd_u16(r, &rdlength)) {
        return false;
    }
    if (r->pos + rdlength > r->len) {
        return false;
    }
    size_t rd_end = r->pos + rdlength;

    if (type_has_compressible_rdata(rr->rtype)) {
        if (!canonicalize_rdata(r, rd_end, rr->rtype, &rr->rdata,
                                &rr->rdata_len)) {
            return false;
        }
    } else {
        rr->rdata = malloc(rdlength > 0 ? rdlength : 1);
        if (rr->rdata == NULL) {
            return false;
        }
        memcpy(rr->rdata, r->buf + r->pos, rdlength);
        rr->rdata_len = rdlength;
        r->pos = rd_end;
    }
    return true;
}

static bool parse_rr_section(reader_t *r, size_t count, mt_dns_rr_t **out)
{
    if (count == 0) {
        *out = NULL;
        return true;
    }
    mt_dns_rr_t *rrs = calloc(count, sizeof(mt_dns_rr_t));
    if (rrs == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!parse_rr(r, &rrs[i])) {
            for (size_t j = 0; j <= i; j++) {
                free(rrs[j].rdata);
            }
            free(rrs);
            return false;
        }
    }
    *out = rrs;
    return true;
}

mt_err_t mt_dns_msg_parse(const uint8_t *buf, size_t len, mt_dns_msg_t **out)
{
    if (len < MT_DNS_HEADER_LEN || len > MT_DNS_MAX_MSG) {
        return MT_ERR_PROTO;
    }
    reader_t r = {buf, len, 0};

    mt_dns_msg_t *msg = calloc(1, sizeof(*msg));
    if (msg == NULL) {
        return MT_ERR_NOMEM;
    }

    uint16_t qd, an, ns, ar;
    if (!rd_u16(&r, &msg->id) || !rd_u16(&r, &msg->flags) ||
        !rd_u16(&r, &qd) || !rd_u16(&r, &an) || !rd_u16(&r, &ns) ||
        !rd_u16(&r, &ar)) {
        free(msg);
        return MT_ERR_PROTO;
    }

    /* miekg/dns leniency: "if we are at the end of the message we should
     * return *just* the header" — a header-only message (some servers send
     * these on REFUSED) parses successfully with empty sections regardless
     * of the counts. */
    if (r.pos == len) {
        *out = msg;
        return MT_OK;
    }

    mt_err_t err = MT_ERR_PROTO;
    if (qd > 0) {
        msg->questions = calloc(qd, sizeof(mt_dns_question_t));
        if (msg->questions == NULL) {
            err = MT_ERR_NOMEM;
            goto fail;
        }
        for (size_t i = 0; i < qd; i++) {
            mt_dns_question_t *q = &msg->questions[i];
            if (!rd_name(&r, q->name, &q->name_len) ||
                !rd_u16(&r, &q->qtype) || !rd_u16(&r, &q->qclass)) {
                goto fail;
            }
            msg->n_questions = i + 1;
        }
    }

    if (!parse_rr_section(&r, an, &msg->answers)) {
        goto fail;
    }
    msg->n_answers = an;
    if (!parse_rr_section(&r, ns, &msg->authority)) {
        goto fail;
    }
    msg->n_authority = ns;
    if (!parse_rr_section(&r, ar, &msg->additional)) {
        goto fail;
    }
    msg->n_additional = ar;

    /* miekg tolerates trailing bytes after the last section; so do we. */
    *out = msg;
    return MT_OK;

fail:
    mt_dns_msg_free(msg);
    return err;
}

void mt_dns_msg_free(mt_dns_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }
    free(msg->questions);
    for (size_t i = 0; i < msg->n_answers; i++) {
        free(msg->answers[i].rdata);
    }
    free(msg->answers);
    for (size_t i = 0; i < msg->n_authority; i++) {
        free(msg->authority[i].rdata);
    }
    free(msg->authority);
    for (size_t i = 0; i < msg->n_additional; i++) {
        free(msg->additional[i].rdata);
    }
    free(msg->additional);
    free(msg);
}

/* ---- packing with RFC 1035 name compression ---- */

/* A dictionary entry references the caller-owned canonical (decompressed)
 * message, not a reallocatable output buffer. Its offset is a 14-bit DNS
 * pointer target and its jump count bounds nested compression to the parser's
 * MAX_POINTER_JUMPS. One open-addressing table is shared by all sections. */
typedef struct dns_suffix {
    const uint8_t *key;
    uint16_t key_len;
    uint16_t offset;
    uint8_t jumps;
} dns_suffix_t;

typedef struct writer {
    uint8_t *buf;
    size_t len;
    size_t cap;
    dns_suffix_t *suffixes;
    size_t suffix_cap, n_suffixes;
    mt_err_t error;
} writer_t;

static bool wr_bytes(writer_t *w, const uint8_t *data, size_t n)
{
    if (n > MT_DNS_MAX_MSG - w->len) {
        w->error = MT_ERR_LIMIT;
        return false;
    }
    if (n == 0) { return true; }
    if (data == NULL) { w->error = MT_ERR_PROTO; return false; }
    size_t required = w->len + n;
    if (required > w->cap) {
        size_t cap = w->cap == 0 ? 512 : w->cap;
        while (cap < required) {
            cap = cap > MT_DNS_MAX_MSG / 2 ? MT_DNS_MAX_MSG : cap * 2;
        }
        uint8_t *grown = realloc(w->buf, cap);
        if (grown == NULL) { w->error = MT_ERR_NOMEM; return false; }
        w->buf = grown;
        w->cap = cap;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
    return true;
}

static bool wr_u16(writer_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return wr_bytes(w, b, sizeof(b));
}

static bool wr_u32(writer_t *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    return wr_bytes(w, b, sizeof(b));
}

static uint32_t suffix_hash(const uint8_t *name, size_t len)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < len; i++) {
        hash = (hash ^ name[i]) * UINT32_C(16777619);
    }
    return hash;
}

static const dns_suffix_t *suffix_find(const writer_t *w,
                                       const uint8_t *name, size_t len)
{
    if (w->suffix_cap == 0) { return NULL; }
    size_t pos = (size_t)suffix_hash(name, len) & (w->suffix_cap - 1);
    while (w->suffixes[pos].key != NULL) {
        const dns_suffix_t *entry = &w->suffixes[pos];
        if (entry->key_len == len && memcmp(entry->key, name, len) == 0) {
            return entry;
        }
        pos = (pos + 1) & (w->suffix_cap - 1);
    }
    return NULL;
}

static bool suffix_grow(writer_t *w)
{
    size_t cap = w->suffix_cap ? w->suffix_cap * 2 : 64;
    if (cap < w->suffix_cap || cap > SIZE_MAX / sizeof(*w->suffixes)) {
        w->error = MT_ERR_LIMIT;
        return false;
    }
    dns_suffix_t *entries = calloc(cap, sizeof(*entries));
    if (entries == NULL) { w->error = MT_ERR_NOMEM; return false; }
    for (size_t i = 0; i < w->suffix_cap; i++) {
        const dns_suffix_t *old = &w->suffixes[i];
        if (old->key == NULL) { continue; }
        size_t pos = (size_t)suffix_hash(old->key, old->key_len) & (cap - 1);
        while (entries[pos].key != NULL) { pos = (pos + 1) & (cap - 1); }
        entries[pos] = *old;
    }
    free(w->suffixes);
    w->suffixes = entries;
    w->suffix_cap = cap;
    return true;
}

static bool suffix_add(writer_t *w, const uint8_t *name, size_t len,
                       size_t offset, uint8_t jumps)
{
    /* Pointers can address only offsets 0..16383. Root is cheaper as a
     * literal zero byte and is never worth a two-byte pointer. */
    if (offset >= 0x4000 || len <= 1) { return true; }
    if (suffix_find(w, name, len) != NULL) { return true; }
    if (w->suffix_cap == 0 || w->n_suffixes >= w->suffix_cap / 2) {
        if (!suffix_grow(w)) { return false; }
    }
    size_t pos = (size_t)suffix_hash(name, len) & (w->suffix_cap - 1);
    while (w->suffixes[pos].key != NULL) {
        pos = (pos + 1) & (w->suffix_cap - 1);
    }
    w->suffixes[pos] = (dns_suffix_t){.key = name, .key_len = (uint16_t)len,
                                    .offset = (uint16_t)offset, .jumps = jumps};
    w->n_suffixes++;
    return true;
}

/* Decompressed wire-form name length, or zero for an invalid name. The
 * caller may provide more bytes after this name (e.g. SOA RDATA). */
static size_t canonical_name_len(const uint8_t *name, size_t available)
{
    if (!name || available == 0) { return 0; }
    size_t pos = 0;
    while (pos < available && pos < MT_DNS_MAX_NAME) {
        uint8_t label = name[pos++];
        if (label == 0) { return pos; }
        if (label > 63 || label > available - pos ||
            label > MT_DNS_MAX_NAME - pos) { return 0; }
        pos += label;
    }
    return 0;
}

static bool wr_name(writer_t *w, const uint8_t *name, size_t len,
                    bool allow_compression)
{
    if (len > MT_DNS_MAX_NAME || canonical_name_len(name, len) != len) {
        w->error = MT_ERR_PROTO;
        return false;
    }

    size_t pointer_at = len;
    uint16_t pointer_target = 0;
    uint8_t jumps = 0;
    if (allow_compression) {
        /* Try suffixes longest-first. Use only offsets already written and
         * never create a pointer chain the decoder would reject. */
        for (size_t pos = 0; pos < len && name[pos] != 0;
             pos += (size_t)name[pos] + 1) {
            const dns_suffix_t *entry = suffix_find(w, name + pos, len - pos);
            if (entry != NULL && entry->jumps < MAX_POINTER_JUMPS) {
                pointer_at = pos;
                pointer_target = entry->offset;
                jumps = (uint8_t)(entry->jumps + 1);
                break;
            }
        }
    }

    size_t pos = 0;
    while (pos < pointer_at) {
        size_t segment = (size_t)name[pos] + 1;
        size_t output_offset = w->len;
        if (!wr_bytes(w, name + pos, segment) ||
            !suffix_add(w, name + pos, len - pos, output_offset, jumps)) {
            return false;
        }
        pos += segment;
    }
    if (pointer_at < len) {
        return wr_u16(w, (uint16_t)(UINT16_C(0xc000) | pointer_target));
    }
    return true;
}

/* RFC 3597 permits compression in the RDATA of RFC 1035 record types,
 * while e.g. SRV (RFC 2782) and DNAME (RFC 6672) forbid it. The parser
 * still accepts historical compressed RDATA of those types and normalizes
 * it, but the writer never generates forbidden RDATA pointers. */
static bool rdata_name_compression_allowed(uint16_t rtype)
{
    switch (rtype) {
    case MT_DNS_TYPE_NS:
    case 3:  /* MD */
    case 4:  /* MF */
    case MT_DNS_TYPE_CNAME:
    case MT_DNS_TYPE_SOA:
    case 7:  /* MB */
    case 8:  /* MG */
    case 9:  /* MR */
    case MT_DNS_TYPE_PTR:
    case 14: /* MINFO */
    case MT_DNS_TYPE_MX:
        return true;
    default:
        return false;
    }
}

static bool wr_rdata(writer_t *w, const mt_dns_rr_t *rr)
{
    if (!type_has_compressible_rdata(rr->rtype)) {
        return wr_bytes(w, rr->rdata, rr->rdata_len);
    }
    size_t fixed = 0;
    switch (rr->rtype) {
    case MT_DNS_TYPE_MX:
    case 18: /* AFSDB */
    case 21: /* RT */
    case 36: /* KX */
        fixed = 2;
        break;
    case MT_DNS_TYPE_SRV:
        fixed = 6;
        break;
    default:
        break;
    }
    if (rr->rdata_len < fixed || rr->rdata == NULL) {
        w->error = MT_ERR_PROTO;
        return false;
    }
    if (!wr_bytes(w, rr->rdata, fixed)) { return false; }

    size_t pos = fixed;
    int names = rr->rtype == MT_DNS_TYPE_SOA || rr->rtype == 14 ||
                rr->rtype == 17 ? 2 : 1;
    for (int i = 0; i < names; i++) {
        size_t name_len = canonical_name_len(rr->rdata + pos, rr->rdata_len - pos);
        if (name_len == 0 ||
            !wr_name(w, rr->rdata + pos, name_len,
                     rdata_name_compression_allowed(rr->rtype))) {
            if (name_len == 0) { w->error = MT_ERR_PROTO; }
            return false;
        }
        pos += name_len;
    }
    size_t trailing = rr->rtype == MT_DNS_TYPE_SOA ? 20 : 0;
    if (rr->rdata_len - pos != trailing) {
        w->error = MT_ERR_PROTO;
        return false;
    }
    return wr_bytes(w, rr->rdata + pos, trailing);
}

static bool wr_rr(writer_t *w, const mt_dns_rr_t *rr)
{
    if (!wr_name(w, rr->name, rr->name_len, true) ||
        !wr_u16(w, rr->rtype) || !wr_u16(w, rr->rclass) ||
        !wr_u32(w, rr->ttl)) {
        return false;
    }
    size_t length_pos = w->len;
    if (!wr_u16(w, 0)) { return false; }
    size_t rdata_pos = w->len;
    if (!wr_rdata(w, rr)) { return false; }
    size_t length = w->len - rdata_pos;
    if (length > UINT16_MAX) { w->error = MT_ERR_LIMIT; return false; }
    w->buf[length_pos] = (uint8_t)(length >> 8);
    w->buf[length_pos + 1] = (uint8_t)length;
    return true;
}

mt_err_t mt_dns_msg_pack(const mt_dns_msg_t *msg, uint8_t **out,
                         size_t *out_len)
{
    if (msg->n_questions > UINT16_MAX || msg->n_answers > UINT16_MAX ||
        msg->n_authority > UINT16_MAX || msg->n_additional > UINT16_MAX) {
        return MT_ERR_LIMIT;
    }
    writer_t w = {0};
    bool ok = wr_u16(&w, msg->id) && wr_u16(&w, msg->flags) &&
              wr_u16(&w, (uint16_t)msg->n_questions) &&
              wr_u16(&w, (uint16_t)msg->n_answers) &&
              wr_u16(&w, (uint16_t)msg->n_authority) &&
              wr_u16(&w, (uint16_t)msg->n_additional);
    for (size_t i = 0; ok && i < msg->n_questions; i++) {
        const mt_dns_question_t *q = &msg->questions[i];
        ok = wr_name(&w, q->name, q->name_len, true) &&
             wr_u16(&w, q->qtype) && wr_u16(&w, q->qclass);
    }
    for (size_t i = 0; ok && i < msg->n_answers; i++) {
        ok = wr_rr(&w, &msg->answers[i]);
    }
    for (size_t i = 0; ok && i < msg->n_authority; i++) {
        ok = wr_rr(&w, &msg->authority[i]);
    }
    for (size_t i = 0; ok && i < msg->n_additional; i++) {
        ok = wr_rr(&w, &msg->additional[i]);
    }
    free(w.suffixes);
    if (!ok) {
        free(w.buf);
        return w.error == MT_OK ? MT_ERR_PROTO : w.error;
    }
    *out = w.buf;
    *out_len = w.len;
    return MT_OK;
}

void mt_dns_msg_strip_aaaa(mt_dns_msg_t *msg)
{
    size_t kept = 0;
    for (size_t i = 0; i < msg->n_answers; i++) {
        if (msg->answers[i].rtype == MT_DNS_TYPE_AAAA) {
            free(msg->answers[i].rdata);
            continue;
        }
        if (kept != i) {
            msg->answers[kept] = msg->answers[i];
        }
        kept++;
    }
    msg->n_answers = kept;
}

/* ---- light raw-message helpers for the request hook ---- */

static bool skip_name(reader_t *r)
{
    uint8_t name[MT_DNS_MAX_NAME + 1];
    size_t name_len;
    return rd_name(r, name, &name_len);
}

bool mt_dns_is_single_ptr_query(const uint8_t *buf, size_t len)
{
    if (len < MT_DNS_HEADER_LEN) {
        return false;
    }
    uint16_t qd = (uint16_t)((uint16_t)buf[4] << 8 | buf[5]);
    if (qd != 1) {
        return false;
    }
    reader_t r = {buf, len, MT_DNS_HEADER_LEN};
    if (!skip_name(&r)) {
        return false;
    }
    uint16_t qtype;
    if (!rd_u16(&r, &qtype)) {
        return false;
    }
    return qtype == MT_DNS_TYPE_PTR;
}

mt_err_t mt_dns_make_fake_ptr_response(const uint8_t *req, size_t len,
                                       uint8_t **out, size_t *out_len)
{
    if (len < MT_DNS_HEADER_LEN) {
        return MT_ERR_PROTO;
    }
    /* validate + measure the question section */
    reader_t r = {req, len, MT_DNS_HEADER_LEN};
    uint16_t qd = (uint16_t)((uint16_t)req[4] << 8 | req[5]);
    uint8_t qname[MT_DNS_MAX_NAME + 1];
    size_t qname_len = 0;
    uint16_t qtype = 0, qclass = 0;
    for (uint16_t i = 0; i < qd; i++) {
        if (!rd_name(&r, qname, &qname_len) || !rd_u16(&r, &qtype) ||
            !rd_u16(&r, &qclass)) {
            return MT_ERR_PROTO;
        }
    }

    /* fresh header exactly like Go's fake-PTR reply: QR|RA, rcode
     * NXDOMAIN, id preserved, all other flag bits zero (RD from the
     * request is intentionally NOT copied — Go builds a new MsgHdr). */
    writer_t w = {0};
    uint16_t id = (uint16_t)((uint16_t)req[0] << 8 | req[1]);
    uint16_t flags =
        MT_DNS_FLAG_QR | MT_DNS_FLAG_RA | MT_DNS_RCODE_NXDOMAIN;
    bool ok = wr_u16(&w, id) && wr_u16(&w, flags) && wr_u16(&w, qd) &&
              wr_u16(&w, 0) && wr_u16(&w, 0) && wr_u16(&w, 0);
    /* question(s) echoed decompressed (queries are never compressed in
     * practice; decompressing is semantically identical) */
    reader_t r2 = {req, len, MT_DNS_HEADER_LEN};
    for (uint16_t i = 0; ok && i < qd; i++) {
        ok = rd_name(&r2, qname, &qname_len) && rd_u16(&r2, &qtype) &&
             rd_u16(&r2, &qclass) && wr_bytes(&w, qname, qname_len) &&
             wr_u16(&w, qtype) && wr_u16(&w, qclass);
    }
    if (!ok) {
        free(w.buf);
        return MT_ERR_NOMEM;
    }
    *out = w.buf;
    *out_len = w.len;
    return MT_OK;
}

/* ---- presentation names (miekg escaping) ---- */

mt_err_t mt_dns_name_to_string(const uint8_t *name, size_t name_len,
                               char *buf, size_t buf_len)
{
    size_t w = 0;
    size_t pos = 0;
    bool any_label = false;

#define PUTC(c)                          \
    do {                                 \
        if (w + 1 >= buf_len) {          \
            return MT_ERR_LIMIT;         \
        }                                \
        buf[w++] = (c);                  \
    } while (0)

    while (pos < name_len) {
        uint8_t len8 = name[pos++];
        if (len8 == 0) {
            break;
        }
        if (pos + len8 > name_len) {
            return MT_ERR_PROTO;
        }
        any_label = true;
        for (size_t i = 0; i < len8; i++) {
            uint8_t b = name[pos + i];
            if (b == '.' || b == '(' || b == ')' || b == ';' || b == ' ' ||
                b == '@' || b == '"' || b == '\\') {
                PUTC('\\');
                PUTC((char)b);
            } else if (b < 32 || b >= 127) {
                char esc[5];
                snprintf(esc, sizeof(esc), "\\%03u", b);
                for (int k = 0; k < 4; k++) {
                    PUTC(esc[k]);
                }
            } else {
                PUTC((char)b);
            }
        }
        pos += len8;
        PUTC('.');
    }
    if (!any_label) {
        PUTC('.');
    }
    buf[w] = '\0';
    return MT_OK;
#undef PUTC
}

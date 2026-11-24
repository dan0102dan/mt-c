/* Exercise the production rtnetlink exchange through its public readers.
 * Only send/recv are wrapped; libmnl builds/parses real wire messages.
 * No privileged operation or production-only test hook is needed. */
#include "greatest.h"
#include "magitrickle/rtnl.h"

#include <errno.h>
#include <limits.h>
#include <libmnl/libmnl.h>
#include <linux/fib_rules.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len);
ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len);
static struct {
    uint32_t seq;
    unsigned part, reads;
    int status, recv_error;
    size_t status_size;
    bool interrupted, early_ack, stale_done, extack;
    uint16_t request_type;
    unsigned mutations;
    int mutation_error;
    bool rule_present, rule_constrained;
    uint32_t rule_mark, rule_mask, rule_table, rule_priority;
    _Alignas(struct nlmsghdr) uint8_t request[1024];
} reply;

ssize_t __wrap_mnl_socket_sendto(const struct mnl_socket *nl, const void *data, size_t len)
{
    (void)nl;
    if (len < sizeof(struct nlmsghdr)) { errno = EINVAL; return -1; }
    const struct nlmsghdr *h = data;
    if (len > sizeof(reply.request)) { errno = EMSGSIZE; return -1; }
    if (h->nlmsg_type != RTM_GETROUTE && h->nlmsg_type != RTM_GETRULE &&
        h->nlmsg_type != RTM_NEWRULE && h->nlmsg_type != RTM_DELRULE &&
        h->nlmsg_type != RTM_NEWROUTE && h->nlmsg_type != RTM_DELROUTE) { errno = EINVAL; return -1; }
    memcpy(reply.request, data, len);
    reply.request_type = h->nlmsg_type;
    if ((h->nlmsg_flags & NLM_F_DUMP) != NLM_F_DUMP) { reply.mutations++; }
    reply.seq = h->nlmsg_seq;
    reply.part = 0;
    return (ssize_t)len;
}

ssize_t __wrap_mnl_socket_recvfrom(const struct mnl_socket *nl, void *data, size_t len)
{
    (void)nl;
    reply.reads++;
    if (reply.recv_error) {
        errno = reply.recv_error;
        reply.recv_error = 0;
        return -1;
    }
    if (len < 256) { errno = EMSGSIZE; return -1; }
    memset(data, 0, len);
    struct nlmsghdr *h = mnl_nlmsg_put_header(data);
    h->nlmsg_seq = reply.seq;
    h->nlmsg_flags = NLM_F_MULTI;
    if (reply.request_type != RTM_GETROUTE && reply.request_type != RTM_GETRULE) {
        h->nlmsg_type = NLMSG_ERROR;
        struct nlmsgerr *ack = mnl_nlmsg_put_extra_header(h, sizeof(*ack));
        ack->error = reply.mutation_error;
    } else if (reply.stale_done) {
        reply.stale_done = false;
        h->nlmsg_seq--;
        h->nlmsg_type = NLMSG_DONE;
        int status = -EIO;
        memcpy(mnl_nlmsg_put_extra_header(h, sizeof(status)), &status, sizeof(status));
    } else if (reply.early_ack) {
        reply.early_ack = false;
        h->nlmsg_type = NLMSG_ERROR;
        struct nlmsgerr *ack = mnl_nlmsg_put_extra_header(h, sizeof(*ack));
        ack->error = 0;
    } else if (reply.part++ == 0 && (reply.request_type != RTM_GETRULE || reply.rule_present)) {
        if (reply.request_type == RTM_GETRULE) {
            h->nlmsg_type = RTM_NEWRULE;
            struct fib_rule_hdr *rule = mnl_nlmsg_put_extra_header(h, sizeof(*rule));
            rule->family = AF_INET; rule->action = FR_ACT_TO_TBL;
            mnl_attr_put_u32(h, FRA_FWMARK, reply.rule_mark);
            mnl_attr_put_u32(h, FRA_FWMASK, reply.rule_mask);
            mnl_attr_put_u32(h, FRA_TABLE, reply.rule_table);
            mnl_attr_put_u32(h, FRA_PRIORITY, reply.rule_priority);
            if (reply.rule_constrained) { mnl_attr_put_strz(h, FRA_IIFNAME, "other0"); }
        } else {
            h->nlmsg_type = RTM_NEWROUTE;
            struct rtmsg *rt = mnl_nlmsg_put_extra_header(h, sizeof(*rt));
            rt->rtm_family = AF_INET;
            rt->rtm_type = RTN_UNICAST;
            mnl_attr_put_u32(h, RTA_TABLE, 1000);
            mnl_attr_put_u32(h, RTA_PRIORITY, 10);
            mnl_attr_put_u32(h, RTA_OIF, 7);
        }
    } else {
        h->nlmsg_type = NLMSG_DONE;
        if (reply.interrupted) { h->nlmsg_flags |= NLM_F_DUMP_INTR; }
        if (reply.status_size) {
            /* put_extra_header aligns the length: override it to also test
             * deliberately truncated, unaligned DONE payloads. */
            void *payload = mnl_nlmsg_put_extra_header(h, reply.status_size);
            memcpy(payload, &reply.status, reply.status_size);
            h->nlmsg_len = (uint32_t)(sizeof(*h) + reply.status_size);
        }
        if (reply.extack) { mnl_attr_put_strz(h, 1, "test completion detail"); }
    }
    return (ssize_t)h->nlmsg_len;
}

static void reset_reply(void)
{
    memset(&reply, 0, sizeof(reply));
    reply.status_size = sizeof(int);
    reply.rule_present = true;
    reply.rule_mark = reply.rule_table = 1000;
    reply.rule_mask = UINT32_MAX; reply.rule_priority = 1;
}

static int copy_u32_attr(const struct nlattr *attr, void *ud)
{
    uint32_t *values = ud;
    unsigned type = mnl_attr_get_type(attr);
    if (type < 32 && mnl_attr_validate(attr, MNL_TYPE_U32) == 0) { values[type] = mnl_attr_get_u32(attr); }
    return MNL_CB_OK;
}

TEST transparent_rule_priority_mask_and_idempotent_cleanup(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    const int families[] = {AF_INET, AF_INET6};
    for (size_t i = 0; i < 2; i++) {
        reset_reply();
        ASSERT_EQ(MT_OK, mt_rtnl_rule_add_at(r, families[i], 1000, 1000, 1));
        const struct nlmsghdr *h = (const struct nlmsghdr *)reply.request;
        const struct fib_rule_hdr *rule = mnl_nlmsg_get_payload(h);
        ASSERT_EQ(RTM_NEWRULE, h->nlmsg_type); ASSERT_EQ(families[i], rule->family);
        ASSERT_EQ(FR_ACT_TO_TBL, rule->action);
        uint32_t values[32] = {0};
        ASSERT_EQ(MNL_CB_OK, mnl_attr_parse(h, sizeof(*rule), copy_u32_attr, values));
        ASSERT_EQ(1000u, values[FRA_FWMARK]); ASSERT_EQ(UINT32_MAX, values[FRA_FWMASK]);
        ASSERT_EQ(1000u, values[FRA_TABLE]); ASSERT_EQ(1u, values[FRA_PRIORITY]);
        reset_reply(); reply.mutation_error = -ENOENT;
        ASSERT_EQ(MT_OK, mt_rtnl_rule_del_at(r, families[i], 1000, 1000, 1));
        reset_reply(); reply.mutation_error = -ESRCH;
        ASSERT_EQ(MT_OK, mt_rtnl_rule_del_at(r, families[i], 1000, 1000, 1));
    }
    mt_rtnl_close(r); PASS();
}

TEST transparent_rule_recovery_requires_exact_unconstrained_match(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply();
    ASSERT_EQ(MT_OK, mt_rtnl_rule_ensure_at(r, AF_INET, 1000, 1000, 1));
    ASSERT_EQ(0u, reply.mutations);
    for (unsigned condition = 0; condition < 5; condition++) {
        reset_reply();
        if (!condition) { reply.rule_present = false; }
        if (condition == 1) { reply.rule_priority = 100; }
        if (condition == 2) { reply.rule_mask = 0xffff; }
        if (condition == 3) { reply.rule_constrained = true; }
        if (condition == 4) { reply.rule_table = 1001; }
        ASSERT_EQ(MT_OK, mt_rtnl_rule_ensure_at(r, AF_INET, 1000, 1000, 1));
        ASSERT_EQ(1u, reply.mutations);
    }
    reset_reply(); reply.rule_present = false; reply.status = -EIO;
    ASSERT_EQ(MT_ERR_IO, mt_rtnl_rule_ensure_at(r, AF_INET, 1000, 1000, 1));
    ASSERT_EQ(0u, reply.mutations);
    mt_rtnl_close(r); PASS();
}

TEST local_delivery_route_uses_host_scope_and_exact_table(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    const int families[] = {AF_INET, AF_INET6};
    for (size_t i = 0; i < 2; i++) {
        reset_reply();
        ASSERT_EQ(MT_OK, mt_rtnl_route_add_local(r, families[i], 1000, 10, 1));
        const struct nlmsghdr *h = (const struct nlmsghdr *)reply.request;
        const struct rtmsg *route = mnl_nlmsg_get_payload(h);
        ASSERT_EQ(families[i], route->rtm_family); ASSERT_EQ(RTN_LOCAL, route->rtm_type);
        ASSERT_EQ(RT_SCOPE_HOST, route->rtm_scope); ASSERT_EQ(0, route->rtm_dst_len);
        ASSERT(h->nlmsg_flags & NLM_F_EXCL); ASSERT_FALSE(h->nlmsg_flags & NLM_F_REPLACE);
        uint32_t values[32] = {0};
        ASSERT_EQ(MNL_CB_OK, mnl_attr_parse(h, sizeof(*route), copy_u32_attr, values));
        ASSERT_EQ(1000u, values[RTA_TABLE]); ASSERT_EQ(10u, values[RTA_PRIORITY]); ASSERT_EQ(1u, values[RTA_OIF]);
        reset_reply(); reply.mutation_error = -ESRCH;
        ASSERT_EQ(MT_OK, mt_rtnl_route_del_local(r, families[i], 1000, 10, 1));
    }
    mt_rtnl_close(r); PASS();
}

TEST done_status_is_required_before_publishing_snapshot(void)
{
    const int statuses[] = {0, -EIO, -EAGAIN, -EINTR, -ENOMEM, INT_MIN};
    const mt_err_t expected[] = {MT_OK, MT_ERR_IO, MT_ERR_AGAIN, MT_ERR_AGAIN,
                                MT_ERR_NOMEM, MT_ERR_PROTO};
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        reset_reply(); reply.status = statuses[i];
        mt_rtnl_default_route_t out = {.found = true, .ifindex = 99};
        ASSERT_EQ(expected[i], mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
        ASSERT_EQ(2u, reply.reads);
        ASSERT_EQ(expected[i] == MT_OK, out.found);
        ASSERT_EQ(expected[i] == MT_OK ? 7 : 0, out.ifindex);
    }
    mt_rtnl_close(r); PASS();
}

TEST truncated_done_is_not_an_empty_or_successful_dump(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    for (size_t size = 0; size < sizeof(int); size++) {
        reset_reply(); reply.status_size = size;
        mt_rtnl_default_route_t out;
        ASSERT_EQ(MT_ERR_PROTO, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
        ASSERT(!out.found); ASSERT_EQ(0, out.ifindex);
    }
    mt_rtnl_close(r); PASS();
}

TEST interrupted_dump_recovers_with_a_fresh_sequence(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.interrupted = true;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_ERR_AGAIN, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT(!out.found);
    uint32_t old_seq = reply.seq;
    reset_reply(); reply.stale_done = true;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(old_seq + 1u, reply.seq);
    ASSERT_EQ(3u, reply.reads);
    ASSERT(out.found); ASSERT_EQ(7, out.ifindex);
    mt_rtnl_close(r); PASS();
}

TEST ack_and_extack_do_not_hide_completion_error(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.early_ack = true; reply.extack = true; reply.status = -EIO;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_ERR_IO, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(3u, reply.reads); ASSERT(!out.found);
    reset_reply(); reply.extack = true;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT(out.found);
    mt_rtnl_close(r); PASS();
}

TEST receive_interrupt_retries_but_receive_loss_is_an_error(void)
{
    mt_rtnl_t *r = mt_rtnl_open(); ASSERT(r);
    reset_reply(); reply.recv_error = EINTR;
    mt_rtnl_default_route_t out;
    ASSERT_EQ(MT_OK, mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out));
    ASSERT_EQ(3u, reply.reads); ASSERT(out.found);
    reset_reply(); reply.recv_error = ENOBUFS;
    ASSERT(mt_rtnl_get_default_route(r, AF_INET, 1000, 10, &out) != MT_OK);
    ASSERT(!out.found);
    mt_rtnl_close(r); PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(done_status_is_required_before_publishing_snapshot);
    RUN_TEST(truncated_done_is_not_an_empty_or_successful_dump);
    RUN_TEST(interrupted_dump_recovers_with_a_fresh_sequence);
    RUN_TEST(ack_and_extack_do_not_hide_completion_error);
    RUN_TEST(receive_interrupt_retries_but_receive_loss_is_an_error);
    RUN_TEST(transparent_rule_priority_mask_and_idempotent_cleanup);
    RUN_TEST(transparent_rule_recovery_requires_exact_unconstrained_match);
    RUN_TEST(local_delivery_route_uses_host_scope_and_exact_table);
    GREATEST_MAIN_END();
}

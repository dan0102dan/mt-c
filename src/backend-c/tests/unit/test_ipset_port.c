/* Serialize production tuple-set requests through libmnl, without requiring
 * a privileged NETLINK_NETFILTER socket or a loaded ip_set kernel module. */
#include "greatest.h"
#include <assert.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static struct mnl_socket *test_socket_open(int bus);
static int test_socket_bind(struct mnl_socket *socket, unsigned int groups, pid_t pid);
static int test_socket_close(struct mnl_socket *socket);
static ssize_t test_socket_sendto(const struct mnl_socket *socket, const void *data, size_t length);
static ssize_t test_socket_recvfrom(const struct mnl_socket *socket, void *data, size_t length);
#define mnl_socket_open test_socket_open
#define mnl_socket_bind test_socket_bind
#define mnl_socket_close test_socket_close
#define mnl_socket_sendto test_socket_sendto
#define mnl_socket_recvfrom test_socket_recvfrom
#include "../../src/netfilter/ipset_nl_real.c"
#undef mnl_socket_open
#undef mnl_socket_bind
#undef mnl_socket_close
#undef mnl_socket_sendto
#undef mnl_socket_recvfrom

static char socket_token;
static struct {
    _Alignas(struct nlmsghdr) uint8_t bytes[1024];
    unsigned requests;
    int ack_error;
} wire;
static mt_ipset_nl_t *transport;

static struct mnl_socket *test_socket_open(int bus)
{
    assert(bus == NETLINK_NETFILTER);
    return (struct mnl_socket *)&socket_token;
}
static int test_socket_bind(struct mnl_socket *socket, unsigned int groups, pid_t pid)
{
    (void)socket; (void)groups; (void)pid; return 0;
}
static int test_socket_close(struct mnl_socket *socket)
{
    (void)socket; return 0;
}
static ssize_t test_socket_sendto(const struct mnl_socket *socket, const void *data, size_t length)
{
    (void)socket;
    assert(length <= sizeof(wire.bytes));
    memcpy(wire.bytes, data, length); wire.requests++;
    return (ssize_t)length;
}
static ssize_t test_socket_recvfrom(const struct mnl_socket *socket, void *data, size_t length)
{
    (void)socket;
    assert(length >= NLMSG_SPACE(sizeof(struct nlmsgerr)));
    memset(data, 0, length);
    struct nlmsghdr *h = mnl_nlmsg_put_header(data);
    h->nlmsg_type = NLMSG_ERROR;
    h->nlmsg_seq = ((const struct nlmsghdr *)wire.bytes)->nlmsg_seq;
    struct nlmsgerr *ack = mnl_nlmsg_put_extra_header(h, sizeof(*ack));
    ack->error = -wire.ack_error;
    return (ssize_t)h->nlmsg_len;
}

typedef struct attribute_find { unsigned type; const struct nlattr *found; } attribute_find_t;
static int find_attribute(const struct nlattr *attribute, void *context)
{
    attribute_find_t *find = context;
    if (mnl_attr_get_type(attribute) == find->type) { find->found = attribute; }
    return MNL_CB_OK;
}
static const struct nlattr *attribute(unsigned type)
{
    attribute_find_t find = {.type = type};
    (void)mnl_attr_parse((const struct nlmsghdr *)wire.bytes, sizeof(struct nfgenmsg), find_attribute, &find);
    return find.found;
}
static const struct nlattr *nested(const struct nlattr *parent, unsigned type)
{
    attribute_find_t find = {.type = type};
    if (parent) { (void)mnl_attr_parse_nested(parent, find_attribute, &find); }
    return find.found;
}
static void setup(void *unused)
{
    (void)unused; memset(&wire, 0, sizeof(wire));
    transport = mt_ipset_nl_real_new(); assert(transport);
}
static void teardown(void *unused)
{
    (void)unused; mt_ipset_nl_free(transport);
}

TEST exact_set_types_have_exclusive_creation_and_bounds(void)
{
    for (unsigned variant = 0; variant < 4; variant++) {
        bool pair = (variant & 1u) != 0;
        int family = (variant & 2u) ? AF_INET6 : AF_INET;
        uint32_t timeout = pair ? 0u : 600u, maximum = pair ? 128u : 2048u;
        ASSERT_EQ(MT_OK, transport->ops->create_portset(transport, "tuple_test", family, pair, timeout, maximum));
        const struct nlmsghdr *h = (const struct nlmsghdr *)wire.bytes;
        ASSERT_EQ(IPSET_CMD_CREATE, h->nlmsg_type & 0xffu); ASSERT(h->nlmsg_flags & NLM_F_EXCL);
        ASSERT_EQ(6u, mnl_attr_get_u8(attribute(IPSET_ATTR_PROTOCOL)));
        /* Revision 0 works in the kernel but breaks ipset list/save. Both
         * tuple types must remain readable by the userspace type handlers. */
        ASSERT_EQ(1u, mnl_attr_get_u8(attribute(IPSET_ATTR_REVISION)));
        ASSERT_EQ(family, mnl_attr_get_u8(attribute(IPSET_ATTR_FAMILY)));
        ASSERT_STR_EQ(pair ? "hash:ip,port,ip" : "hash:ip,port", mnl_attr_get_str(attribute(IPSET_ATTR_TYPENAME)));
        const struct nlattr *data = attribute(IPSET_ATTR_DATA);
        const struct nlattr *limit = nested(data, IPSET_ATTR_MAXELEM), *ttl = nested(data, IPSET_ATTR_TIMEOUT);
        ASSERT(limit && ttl); ASSERT(limit->nla_type & NLA_F_NET_BYTEORDER); ASSERT(ttl->nla_type & NLA_F_NET_BYTEORDER);
        ASSERT_EQ(maximum, ntohl(mnl_attr_get_u32(limit))); ASSERT_EQ(timeout, ntohl(mnl_attr_get_u32(ttl)));
    }
    wire.ack_error = EEXIST;
    ASSERT_EQ(MT_ERR_EXIST, transport->ops->create_portset(transport, "foreign", AF_INET, false, 30, 4));
    PASS();
}

TEST protocol_ports_and_addresses_are_serialized_without_prefix_widening(void)
{
    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 42};
    for (unsigned pair = 0; pair < 2; pair++) {
        mt_ipset_port_key_t key = {.iplen = pair ? 16u : 4u, .protocol = pair ? IPPROTO_UDP : IPPROTO_TCP,
            .port = 8080, .has_ip2 = pair != 0, .ip = {203, 0, 113, 42}};
        if (pair) { memcpy(key.ip, v6, sizeof(v6)); memcpy(key.ip2, v6, sizeof(v6)); key.ip2[15] = 43; }
        ASSERT_EQ(MT_OK, transport->ops->add_port(transport, "tuple_test", &key, pair ? 0u : 123u));
        const struct nlmsghdr *h = (const struct nlmsghdr *)wire.bytes;
        ASSERT_EQ(IPSET_CMD_ADD, h->nlmsg_type & 0xffu); ASSERT_FALSE(h->nlmsg_flags & NLM_F_EXCL);
        const struct nlattr *data = attribute(IPSET_ATTR_DATA);
        ASSERT_EQ(key.protocol, mnl_attr_get_u8(nested(data, IPSET_ATTR_PROTO)));
        const struct nlattr *port = nested(data, IPSET_ATTR_PORT);
        ASSERT(port && (port->nla_type & NLA_F_NET_BYTEORDER));
        const uint8_t *encoded = mnl_attr_get_payload(port);
        ASSERT_EQ(0x1f, encoded[0]); ASSERT_EQ(0x90, encoded[1]);
        unsigned address_type = pair ? IPSET_ATTR_IPADDR_IPV6 : IPSET_ATTR_IPADDR_IPV4;
        const struct nlattr *ip = nested(nested(data, IPSET_ATTR_IP), address_type);
        ASSERT(ip && (ip->nla_type & NLA_F_NET_BYTEORDER)); ASSERT_EQ(key.iplen, mnl_attr_get_payload_len(ip));
        ASSERT_EQ(0, memcmp(key.ip, mnl_attr_get_payload(ip), key.iplen));
        ASSERT_FALSE(nested(data, IPSET_ATTR_CIDR));
        ASSERT_EQ(pair ? 0u : 123u, ntohl(mnl_attr_get_u32(nested(data, IPSET_ATTR_TIMEOUT))));
        const struct nlattr *ip2 = nested(data, IPSET_ATTR_IP2);
        if (pair) {
            ASSERT(ip2); ip2 = nested(ip2, IPSET_ATTR_IPADDR_IPV6); ASSERT(ip2);
            ASSERT_EQ(0, memcmp(key.ip2, mnl_attr_get_payload(ip2), key.iplen));
        } else { ASSERT_FALSE(ip2); }
        ASSERT_EQ(MT_OK, transport->ops->del_port(transport, "tuple_test", &key));
        data = attribute(IPSET_ATTR_DATA);
        ASSERT_FALSE(nested(data, IPSET_ATTR_TIMEOUT));
        ASSERT_EQ(key.protocol, mnl_attr_get_u8(nested(data, IPSET_ATTR_PROTO)));
        ASSERT_EQ(key.port, ntohs(mnl_attr_get_u16(nested(data, IPSET_ATTR_PORT))));
    }
    PASS();
}

TEST invalid_keys_and_kernel_errors_do_not_look_successful(void)
{
    mt_ipset_port_key_t key = {.iplen = 4, .protocol = IPPROTO_UDP, .port = 443, .ip = {203, 0, 113, 5}};
    wire.ack_error = EPERM;
    ASSERT_EQ(mt_err_from_errno(EPERM), transport->ops->add_port(transport, "tuple_test", &key, 30));
    wire.ack_error = IPSET_ERR_EXIST;
    ASSERT_EQ(MT_OK, transport->ops->del_port(transport, "tuple_test", &key));
    unsigned before = wire.requests;
    key.port = 0; ASSERT_EQ(MT_ERR_INVAL, transport->ops->add_port(transport, "tuple_test", &key, 30));
    key.port = 443; key.protocol = IPPROTO_ICMP;
    ASSERT_EQ(MT_ERR_INVAL, transport->ops->add_port(transport, "tuple_test", &key, 30));
    key.protocol = IPPROTO_UDP; key.iplen = 3;
    ASSERT_EQ(MT_ERR_INVAL, transport->ops->del_port(transport, "tuple_test", &key));
    ASSERT_EQ(MT_ERR_INVAL, transport->ops->create_portset(transport, "tuple_test", AF_UNSPEC, false, 30, 4));
    ASSERT_EQ(MT_ERR_INVAL, transport->ops->create_portset(transport, "tuple_test", AF_INET, false, 30, 0));
    ASSERT_EQ(before, wire.requests);
    PASS();
}

TEST legacy_subnet_operations_keep_their_wire_type(void)
{
    ASSERT_EQ(MT_OK, transport->ops->create(transport, "legacy", AF_INET, 300));
    ASSERT_STR_EQ("hash:net", mnl_attr_get_str(attribute(IPSET_ATTR_TYPENAME)));
    ASSERT_EQ(0u, mnl_attr_get_u8(attribute(IPSET_ATTR_REVISION)));
    const uint8_t address[] = {192, 0, 2, 0};
    ASSERT_EQ(MT_OK, transport->ops->add(transport, "legacy", address, 4, 24, true, 300, true));
    const struct nlattr *data = attribute(IPSET_ATTR_DATA);
    ASSERT_EQ(24u, mnl_attr_get_u8(nested(data, IPSET_ATTR_CIDR)));
    ASSERT_FALSE(nested(data, IPSET_ATTR_PORT)); ASSERT_FALSE(nested(data, IPSET_ATTR_PROTO));
    PASS();
}

GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    SET_SETUP(setup, NULL); SET_TEARDOWN(teardown, NULL);
    RUN_TEST(exact_set_types_have_exclusive_creation_and_bounds);
    RUN_TEST(protocol_ports_and_addresses_are_serialized_without_prefix_widening);
    RUN_TEST(invalid_keys_and_kernel_errors_do_not_look_successful);
    RUN_TEST(legacy_subnet_operations_keep_their_wire_type);
    GREATEST_MAIN_END();
}

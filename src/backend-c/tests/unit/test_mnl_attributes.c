/* Regression for the old GCC8 macro/pragma failure: all traversal happens
 * inside libmnl functions. Keep this test cross-compilable with target SDKs. */
#include "greatest.h"
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>
#include <stdint.h>
#include <string.h>

typedef struct { unsigned n; uint16_t types[8]; uint32_t values[8]; } seen_t;
static int capture(const struct nlattr *attr, void *ud) {
    seen_t *s = ud;
    if (s->n >= 8 || mnl_attr_validate(attr, MNL_TYPE_U32)) { return MNL_CB_ERROR; }
    s->types[s->n] = mnl_attr_get_type(attr);
    s->values[s->n++] = mnl_attr_get_u32(attr);
    return MNL_CB_OK;
}
TEST parses_every_top_level_and_nested_attribute(void) {
    uint8_t buf[256] = {0};
    struct nlmsghdr *h = mnl_nlmsg_put_header(buf);
    (void)mnl_nlmsg_put_extra_header(h, sizeof(struct rtmsg));
    mnl_attr_put_u32(h, RTA_OIF, 17);
    mnl_attr_put_u32(h, RTA_TABLE, 1298229097u);
    mnl_attr_put_u32(h, RTA_PRIORITY, 1234);
    seen_t seen = {0};
    ASSERT_EQ(MNL_CB_OK, mnl_attr_parse(h, sizeof(struct rtmsg), capture, &seen));
    ASSERT_EQ(3u, seen.n);
    ASSERT_EQ(RTA_OIF, seen.types[0]); ASSERT_EQ(17u, seen.values[0]);
    ASSERT_EQ(RTA_TABLE, seen.types[1]); ASSERT_EQ(1298229097u, seen.values[1]);
    ASSERT_EQ(RTA_PRIORITY, seen.types[2]); ASSERT_EQ(1234u, seen.values[2]);
    h = mnl_nlmsg_put_header(buf);
    struct nlattr *nest = mnl_attr_nest_start(h, 100);
    for (uint16_t i = 1; i <= 3; i++) { mnl_attr_put_u32(h, i, (uint32_t)i * 11u); }
    mnl_attr_nest_end(h, nest);
    memset(&seen, 0, sizeof(seen));
    ASSERT_EQ(MNL_CB_OK, mnl_attr_parse_nested(nest, capture, &seen));
    ASSERT_EQ(3u, seen.n);
    for (unsigned i = 0; i < 3; i++) { ASSERT_EQ(i + 1, seen.types[i]); ASSERT_EQ((i + 1) * 11, seen.values[i]); }
    PASS();
}
TEST never_passes_a_truncated_attribute_to_callback(void) {
    struct nlattr attr = {.nla_len = sizeof(struct nlattr) + 1u, .nla_type = 1};
    seen_t seen = {0};
    (void)mnl_attr_parse_payload(&attr, sizeof(attr), capture, &seen);
    ASSERT_EQ(0u, seen.n);
    uint8_t buf[64] = {0};
    struct nlmsghdr *h = mnl_nlmsg_put_header(buf);
    mnl_attr_put_u8(h, 1, 7);
    ASSERT_EQ(MNL_CB_ERROR, mnl_attr_parse(h, 0, capture, &seen));
    ASSERT_EQ(0u, seen.n);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_every_top_level_and_nested_attribute);
    RUN_TEST(never_passes_a_truncated_attribute_to_callback);
    GREATEST_MAIN_END();
}

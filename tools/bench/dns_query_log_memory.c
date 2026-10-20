/* Compile with src/dns/query_log.c + src/dns/wire.c and -Iinclude.
 * Production byte budget, no synthetic row quota. Counts vary within a page
 * as the oldest pages are recycled. All raw timestamps/questions survive. */
#include "magitrickle/dns_query_log.h"
#include <stdio.h>
#include <string.h>

static bool measure(const char *scenario, bool long_names, bool unique, bool unique_clients, bool ipv6) {
    mt_dns_query_log_t *log = mt_dns_query_log_create();
    if (!log || !mt_dns_query_log_set_enabled(log, true)) {
        fprintf(stderr, "cannot allocate journal\n"); mt_dns_query_log_destroy(log); return false;
    }
    for (unsigned i = 0; i < 300000; i++) {
        unsigned key = unique ? i : i % 200;
        uint8_t name[255], ip[16] = {192, 0, 2, 1}; size_t at = 0;
        if (long_names) {
            for (size_t j = 0; j < 4; j++) {
                size_t n = j == 3 ? 61 : 63;
                name[at++] = (uint8_t)n; memset(name + at, 'x', n); at += n;
            }
        } else {
            name[at++] = 27; memset(name + at, 'x', 27); at += 27;
            name[at++] = 3; memcpy(name + at, "org", 3); at += 3;
        }
        name[1] = (uint8_t)key; name[2] = (uint8_t)(key >> 8);
        name[3] = (uint8_t)(key >> 16); name[4] = (uint8_t)(key >> 24); name[at++] = 0;
        unsigned client = unique_clients ? i : i % 20;
        if (ipv6) { ip[0] = 0x20; ip[1] = 1; ip[2] = 0x0d; ip[3] = 0xb8; }
        size_t end = ipv6 ? 16 : 4;
        ip[end - 1] = (uint8_t)client; ip[end - 2] = (uint8_t)(client >> 8);
        ip[end - 3] = (uint8_t)(client >> 16);
        if (!mt_dns_query_log_append_wire(log, ip, end, name, at, i % 2 ? 28 : 1, i % 2 != 0, (int64_t)i)) {
            fprintf(stderr, "append failed\n"); mt_dns_query_log_destroy(log); return false;
        }
    }
    mt_dns_query_log_stats_t s; mt_dns_query_log_stats(log, &s);
    printf("%s,%zu,%zu,%zu,%zu,%zu,%zu\n", scenario, s.count, s.allocated_bytes,
           s.entry_bytes, s.index_bytes, s.pool_bytes, s.domains);
    mt_dns_query_log_destroy(log);
    return true;
}
int main(void) {
    puts("scenario,retained,allocated,record_pages,index,slab_pages,domains");
    bool ok = measure("200-short-domains-20-ipv4", false, false, false, false);
    ok = measure("unique-short-domains-20-ipv4", false, true, false, false) && ok;
    ok = measure("unique-max-domains-20-ipv4", true, true, false, false) && ok;
    ok = measure("200-short-domains-20-ipv6", false, false, false, true) && ok;
    ok = measure("200-short-domains-unique-ipv6", false, false, true, true) && ok;
    ok = measure("unique-max-domains-unique-ipv6", true, true, true, true) && ok;
    return ok ? 0 : 1;
}

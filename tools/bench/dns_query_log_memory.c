/* Synthetic, reproducible storage benchmark. Not a router RSS measurement.
 * From repo root: cc -O2 -Isrc/backend-c/include tools/bench/dns_query_log_memory.c \
 * src/backend-c/src/dns/query_log.c src/backend-c/src/dns/wire.c -o /tmp/dns-log-memory
 * /tmp/dns-log-memory
 */
#include "magitrickle/dns_query_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void sample(size_t capacity, size_t domains, size_t clients, bool longest) {
    mt_dns_query_log_t *log = mt_dns_query_log_create_sized(capacity);
    assert(log);
    for (size_t i = 0; i < capacity; i++) {
        if (longest) {
            uint8_t name[255]; size_t p = 0;
            for (int n = 0; n < 4; n++) {
                size_t len = n == 3 ? 61 : 63;
                name[p++] = (uint8_t)len; memset(name + p, 200, len); p += len;
            }
            name[p++] = 0;
            size_t key = i % domains;
            name[1] = (uint8_t)(key >> 8); name[2] = (uint8_t)key;
            uint8_t addr[16] = {0x20, 1, 0x0d, 0xb8};
            addr[14] = (uint8_t)((i % clients) >> 8); addr[15] = (uint8_t)(i % clients);
            assert(mt_dns_query_log_append_wire(log, addr, sizeof(addr), name, p, 28, false, (int64_t)i));
        } else {
            char domain[64], client[48];
            snprintf(domain, sizeof(domain), "d%05zu.abcdefghijklmnop.example", i % domains);
            snprintf(client, sizeof(client), "10.0.%zu.%zu", (i % clients) / 256, (i % clients) % 256);
            mt_dns_query_log_append(log, client, domain, 1, false, (int64_t)i);
        }
    }
    mt_dns_query_log_stats_t s;
    mt_dns_query_log_stats(log, &s);
    assert(s.count == capacity && s.domains == domains && s.clients == clients);
    printf("%zu,%zu,%zu,%s,%zu,%zu,%zu,%zu\n", capacity, domains, clients, longest ? "wire255" : "text31",
        s.entry_bytes, s.index_bytes, s.pool_bytes, s.allocated_bytes);
    mt_dns_query_log_destroy(log);
}
int main(void) {
    puts("rows,domains,clients,names,ring_bytes,index_bytes,pool_reserved_bytes,total_allocated_bytes");
    sample(1000, 200, 20, false);
    sample(1000, 1000, 20, false);
    sample(1000, 1000, 1000, true);
    sample(10000, 2000, 20, false);
    sample(10000, 10000, 20, false);
    return 0;
}

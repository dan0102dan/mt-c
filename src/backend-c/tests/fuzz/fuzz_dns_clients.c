/* Fuzz the actual RCI parsers, including the optional neighbour snapshot.
 * Seed pairs contain two compact JSON documents separated by a newline. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "magitrickle/dns_clients.h"

static void check_snapshot(const char *hotspot, size_t hotspot_len,
                            const char *neighbors, size_t neighbors_len)
{
    mt_dns_client_t *clients = NULL;
    size_t count = 0;
    mt_err_t err = mt_dns_clients_parse_keenetic(hotspot, hotspot_len,
        neighbors, neighbors_len, &clients, &count);
    if (err != MT_OK) {
        if (clients || count) { abort(); }
        return;
    }
    if (count > MT_DNS_CLIENT_LIMIT || (count && !clients)) { abort(); }
    for (size_t i = 0; i < count; i++) {
        char ip[MT_DNS_CLIENT_IP_LEN], name[MT_DNS_CLIENT_NAME_LEN];
        if (!memchr(clients[i].ip, '\0', sizeof(clients[i].ip)) ||
            !memchr(clients[i].name, '\0', sizeof(clients[i].name)) ||
            !mt_dns_client_address(clients[i].ip, ip, sizeof(ip)) ||
            !mt_dns_client_label(clients[i].name, name) ||
            strcmp(ip, clients[i].ip) != 0 || strcmp(name, clients[i].name) != 0 ||
            (i && strcmp(clients[i - 1].ip, clients[i].ip) >= 0)) { abort(); }
    }
    free(clients);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *text = (const char *)data;
    const char *separator = memchr(text, '\n', size);
    size_t hotspot_len = separator ? (size_t)(separator - text) : size;
    check_snapshot(text, hotspot_len, separator ? separator + 1 : NULL,
        separator ? size - hotspot_len - 1 : 0);

    /* Reach neighbour validation even if mutation breaks the hotspot JSON. */
    static const char hotspot[] = "{\"host\":[{\"ip\":\"192.0.2.1\","
        "\"mac\":\"02:00:00:00:00:01\",\"name\":\"Client\","
        "\"active\":true,\"interface\":{\"id\":\"Bridge0\"}}]}";
    check_snapshot(hotspot, sizeof(hotspot) - 1, text, size);
    return 0;
}

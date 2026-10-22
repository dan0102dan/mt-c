#include "greatest.h"
#include "magitrickle/dns_neighbors.h"
#ifdef MT_ENTWARE_KN
#include <curl/curl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static size_t (*receive_body)(char *, size_t, size_t, void *);
static void *receive_ud;
static unsigned requests;
static bool fail_http;
static mt_err_t discovery_error;
static mt_dns_neighbor_t observation;
static const char *inventory = "{\"host\":[{\"ip\":\"192.168.1.2\",\"mac\":\"02:03:04:05:06:07\",\"name\":\"Phone\"}]}";
CURL *__wrap_curl_easy_init(void);
CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode __wrap_curl_easy_perform(CURL *curl);
CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
void __wrap_curl_easy_cleanup(CURL *curl);
mt_err_t __wrap_mt_dns_neighbors_read(mt_dns_neighbor_t **out, size_t *count, const atomic_bool *stop);
CURL *__wrap_curl_easy_init(void) { return (CURL *)&requests; }
CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    (void)curl;
    va_list args; va_start(args, option);
    if (option == CURLOPT_WRITEFUNCTION) { receive_body = va_arg(args, size_t (*)(char *, size_t, size_t, void *)); }
    if (option == CURLOPT_WRITEDATA) { receive_ud = va_arg(args, void *); }
    va_end(args);
    return CURLE_OK;
}
CURLcode __wrap_curl_easy_perform(CURL *curl)
{
    (void)curl; requests++;
    if (fail_http) { return CURLE_COULDNT_CONNECT; }
    size_t len = strlen(inventory);
    return receive_body((char *)inventory, 1, len, receive_ud) == len ? CURLE_OK : CURLE_WRITE_ERROR;
}
CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
    (void)curl; (void)info;
    va_list args; va_start(args, info); *va_arg(args, long *) = 200; va_end(args);
    return CURLE_OK;
}
void __wrap_curl_easy_cleanup(CURL *curl) { (void)curl; }
mt_err_t __wrap_mt_dns_neighbors_read(mt_dns_neighbor_t **out, size_t *count, const atomic_bool *stop)
{
    *out = NULL; *count = 0;
    if (stop && atomic_load(stop)) { return MT_ERR_CANCELED; }
    if (discovery_error != MT_OK) { return discovery_error; }
    *out = malloc(sizeof(**out));
    if (!*out) { return MT_ERR_NOMEM; }
    **out = observation; *count = 1;
    return MT_OK;
}
TEST cached_inventory_reuses_names_and_bounds_unknown_mac_requests(void)
{
    mt_dns_hotspot_cache_t cache = {0};
    mt_dns_client_t *clients = NULL; size_t count = 0;
    observation = (mt_dns_neighbor_t){.ip = "fe80::1", .mac = {2,3,4,5,6,7}, .ifindex = 1};
    requests = 0; fail_http = false; discovery_error = MT_OK;
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(1u, requests); ASSERT_EQ(2u, count); free(clients);
    strcpy(observation.ip, "fe80::2");
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(1u, requests); ASSERT_EQ(2u, count); free(clients);
    observation.mac[5]++;
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT(cache.retry_pending); ASSERT_EQ(1u, requests); free(clients);
    cache.retry_after = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(2u, requests); ASSERT_EQ(1u, count); free(clients);
    cache.retry_after = 0;
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(2u, requests); ASSERT(!cache.retry_pending); free(clients);
    /* Another event for an unknown but already queried MAC does not poll RCI. */
    strcpy(observation.ip, "fe80::3");
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(2u, requests); free(clients);
    /* Full reconciliation observes renames and failed discovery drops enrichment. */
    mt_dns_hotspot_cache_clear(&cache);
    inventory = "{\"host\":{\"mac\":\"02:03:04:05:06:08\",\"name\":\"Renamed\"}}";
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(3u, requests); ASSERT_EQ(1u, count); ASSERT_STR_EQ("Renamed", clients[0].name); free(clients);
    discovery_error = MT_ERR_IO;
    ASSERT_EQ(MT_OK, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(0u, count); free(clients);
    mt_dns_hotspot_cache_clear(&cache);
    fail_http = true;
    ASSERT_EQ(MT_ERR_UPSTREAM, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT(clients == NULL); ASSERT_EQ(0u, count); ASSERT(cache.json == NULL);
    unsigned before = requests;
    ASSERT_EQ(MT_ERR_AGAIN, mt_dns_clients_fetch_keenetic_cached(&clients, &count, NULL, &cache));
    ASSERT_EQ(before, requests);
    atomic_bool stop; atomic_init(&stop, true);
    ASSERT_EQ(MT_ERR_CANCELED, mt_dns_clients_fetch_keenetic_cached(&clients, &count, &stop, &cache));
    mt_dns_hotspot_cache_clear(&cache);
    PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
#ifdef MT_ENTWARE_KN
    RUN_TEST(cached_inventory_reuses_names_and_bounds_unknown_mac_requests);
#endif
    GREATEST_MAIN_END();
}

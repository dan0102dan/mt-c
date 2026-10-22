#include "greatest.h"
#ifdef MT_ENTWARE_KN
#include <curl/curl.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

/* Keep the production URLs and actual libcurl transfer, redirecting only the
 * connection to an ephemeral loopback fixture. No router or privileged port. */
static struct curl_slist *fixture_route;
static CURL *fixture_curl_init(void)
{
    CURL *curl = curl_easy_init();
    if (curl && curl_easy_setopt(curl, CURLOPT_CONNECT_TO, fixture_route) != CURLE_OK) {
        curl_easy_cleanup(curl); return NULL;
    }
    return curl;
}
#define curl_easy_init fixture_curl_init
#define mt_dns_clients_parse_hotspot fixture_parse_hotspot
#define mt_dns_clients_parse_keenetic fixture_parse_keenetic
#define mt_dns_clients_fetch_keenetic fixture_fetch_keenetic
#include "../../src/interfaces/keenetic_dns_clients.c"
#undef curl_easy_init
#undef mt_dns_clients_parse_hotspot
#undef mt_dns_clients_parse_keenetic
#undef mt_dns_clients_fetch_keenetic

typedef struct { unsigned status; const char *body; } reply_t;
typedef struct {
    int listener;
    const reply_t *replies;
    size_t count, handled;
    char request[2][2048];
    atomic_bool *cancel;
} server_t;
static void *serve(void *ud)
{
    server_t *server = ud;
    for (size_t i = 0; i < server->count; i++) {
        struct pollfd pending = {.fd = server->listener, .events = POLLIN};
        if (poll(&pending, 1, 4000) <= 0) { break; }
        int fd = accept(server->listener, NULL, NULL);
        if (fd < 0) { break; }
        size_t used = 0;
        while (used < sizeof(server->request[i]) - 1) {
            struct pollfd readable = {.fd = fd, .events = POLLIN};
            if (poll(&readable, 1, 3000) <= 0) { break; }
            ssize_t received = read(fd, server->request[i] + used, sizeof(server->request[i]) - used - 1);
            if (received <= 0) { break; }
            used += (size_t)received;
            server->request[i][used] = '\0';
            if (strstr(server->request[i], "\r\n\r\n")) { break; }
        }
        if (server->cancel) { atomic_store(server->cancel, true); }
        const reply_t *reply = &server->replies[i];
        char header[256];
        int length = snprintf(header, sizeof(header), "HTTP/1.1 %u Fixture\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
            reply->status, strlen(reply->body), reply->status == 302 ? "Location: http://127.0.0.1:1/never\r\n" : "");
        if (length > 0 && (size_t)length < sizeof(header)) {
            (void)send(fd, header, (size_t)length, MSG_NOSIGNAL);
            size_t sent = 0, body_length = strlen(reply->body);
            while (sent < body_length) {
                ssize_t n = send(fd, reply->body + sent, body_length - sent, MSG_NOSIGNAL);
                if (n <= 0) { break; }
                sent += (size_t)n;
            }
        }
        close(fd); server->handled++;
    }
    return NULL;
}
static bool start_server(server_t *server, pthread_t *thread)
{
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0) { return false; }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    if (bind(server->listener, (void *)&address, sizeof(address)) || listen(server->listener, 2) ||
        getsockname(server->listener, (void *)&address, &size)) {
        close(server->listener); return false;
    }
    char route[96];
    (void)snprintf(route, sizeof(route), "127.0.0.1:79:127.0.0.1:%u", (unsigned)ntohs(address.sin_port));
    fixture_route = curl_slist_append(NULL, route);
    if (!fixture_route || pthread_create(thread, NULL, serve, server)) {
        curl_slist_free_all(fixture_route); fixture_route = NULL; close(server->listener); return false;
    }
    return true;
}
static void stop_server(server_t *server, pthread_t thread)
{
    pthread_join(thread, NULL); close(server->listener);
    curl_slist_free_all(fixture_route); fixture_route = NULL;
}

TEST transport_refreshes_both_documents_and_never_reuses_stale_joins(void)
{
    const char *old_host = "{\"host\":{\"ip\":\"192.168.5.125\",\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"Lenovo\"}}";
    const char *new_host = "{\"host\":{\"ip\":\"192.168.5.125\",\"mac\":\"14:5a:fc:2f:64:d3\",\"name\":\"Renamed\"}}";
    const char *neighbor = "{\"8\":{\"mac\":\"14:5a:fc:2f:64:d3\",\"address-family\":\"ipv6\",\"interface\":\"Bridge0\",\"expired\":false,"
        "\"addresses\":{\"address\":[{\"address\":\"fe80::4a17:e738:40c9:6a7\",\"status\":\"active\"}]}}}";
    const reply_t cases[][2] = {
        {{200, old_host}, {200, neighbor}},
        {{200, new_host}, {200, neighbor}},
        {{200, new_host}, {503, ""}},
        {{200, new_host}, {200, "[]truncated"}},
        {{200, new_host}, {302, ""}},
        {{200, "{\"status\":[{\"status\":\"error\"}]}"}, {200, neighbor}}
    };
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    ASSERT_EQ(0, setenv("http_proxy", "http://127.0.0.1:1", 1));
    ASSERT_EQ(0, setenv("no_proxy", "", 1));
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        server_t server = {.replies = cases[i], .count = 2};
        pthread_t thread; ASSERT(start_server(&server, &thread));
        mt_dns_client_t *clients = NULL; size_t count = 0;
        mt_err_t err = fixture_fetch_keenetic(&clients, &count, NULL, NULL);
        stop_server(&server, thread);
        ASSERT_EQ(2u, (unsigned)server.handled);
        ASSERT(strstr(server.request[0], "GET /rci/show/ip/hotspot HTTP/"));
        ASSERT(strstr(server.request[1], "GET /rci/show/ip/neighbour HTTP/"));
        ASSERT_EQ(i == 5 ? MT_ERR_UPSTREAM : MT_OK, err);
        ASSERT_EQ(i == 5 ? 0u : i < 2 ? 2u : 1u, (unsigned)count);
        for (size_t n = 0; n < count; n++) { ASSERT_STR_EQ(i ? "Renamed" : "Lenovo", clients[n].name); }
        if (i >= 2 && i < 5) { ASSERT_STR_EQ("192.168.5.125", clients[0].ip); }
        free(clients);
    }
    unsetenv("http_proxy"); unsetenv("no_proxy"); curl_global_cleanup(); PASS();
}
TEST hotspot_http_errors_and_cancellation_publish_no_partial_result(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    const reply_t replies[] = {{401, ""}, {302, ""}, {200, "{\"host\":[{\"ip\":\"192.168.1.2\",\"name\":\"Phone\"}]}"}};
    for (size_t i = 0; i < sizeof(replies) / sizeof(*replies); i++) {
        atomic_bool cancel = ATOMIC_VAR_INIT(false);
        server_t server = {.replies = &replies[i], .count = 1, .cancel = i == 2 ? &cancel : NULL};
        pthread_t thread; ASSERT(start_server(&server, &thread));
        mt_dns_client_t *clients = NULL; size_t count = 0;
        mt_err_t err = fixture_fetch_keenetic(&clients, &count, &cancel, NULL);
        stop_server(&server, thread);
        ASSERT_EQ(1u, (unsigned)server.handled);
        ASSERT_EQ(i == 2 ? MT_ERR_CANCELED : MT_ERR_UPSTREAM, err);
        ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    }
    atomic_bool cancel = ATOMIC_VAR_INIT(true);
    mt_dns_client_t *clients = NULL; size_t count = 0;
    ASSERT_EQ(MT_ERR_CANCELED, fixture_fetch_keenetic(&clients, &count, &cancel, NULL));
    ASSERT_EQ(0u, (unsigned)count); ASSERT(clients == NULL);
    curl_global_cleanup(); PASS();
}
TEST response_size_and_progress_callbacks_remain_bounded(void)
{
    client_response_t response = {0};
    ASSERT_EQ(3u, receive_clients("abc", 1, 3, &response));
    ASSERT_STR_EQ("abc", response.data);
    ASSERT_EQ(0u, receive_clients("", SIZE_MAX, 2, &response));
    ASSERT_EQ(MT_ERR_LIMIT, response.error); free(response.data);
    response = (client_response_t){.len = MT_DNS_CLIENT_BODY_LIMIT};
    ASSERT_EQ(0u, receive_clients("x", 1, 1, &response));
    ASSERT_EQ(MT_ERR_LIMIT, response.error);
    atomic_bool cancel = ATOMIC_VAR_INIT(false);
    client_progress_t context = {.stop = &cancel};
    ASSERT_EQ(0, fetch_progress(&context, 0, 0, 0, 0));
    atomic_store(&cancel, true);
    ASSERT_EQ(1, fetch_progress(&context, 0, 0, 0, 0));
    PASS();
}
#endif
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
#ifdef MT_ENTWARE_KN
    RUN_TEST(transport_refreshes_both_documents_and_never_reuses_stale_joins);
    RUN_TEST(hotspot_http_errors_and_cancellation_publish_no_partial_result);
    RUN_TEST(response_size_and_progress_callbacks_remain_bounded);
#endif
    GREATEST_MAIN_END();
}

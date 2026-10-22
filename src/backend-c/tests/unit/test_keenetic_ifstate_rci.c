/* Exercise the actual bounded transport and positional name decoder. Including
 * the source exposes only test-local internals, no production testing API. */
#include "greatest.h"
#include "../../src/interfaces/keenetic_ifstate_rci.c"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

TEST cancellation_before_io_is_bounded(void)
{
    char *body = NULL; size_t length = 0;
    atomic_bool stop = ATOMIC_VAR_INIT(true);
    ASSERT_EQ(MT_ERR_CANCELED, mt_kn_rci_request("http://127.0.0.1:1", NULL, 1024,
                                               &stop, &body, &length));
    ASSERT(body == NULL); ASSERT_EQ(0u, length);
    PASS();
}
TEST name_batch_is_positional_and_never_guesses_missing_names(void)
{
    mt_kn_ifinfo_t items[2] = {{.id="Wireguard0"}, {.id="Wireguard1"}};
    const char *json = "[{\"show\":{\"interface\":{\"system-name\":\"nwg0\"}}},{\"status\":[{\"status\":\"error\"}]}]";
    body_t body = {.data=(char *)json, .size=strlen(json)};
    ASSERT_EQ(MT_OK, fill_names(&body, items, 2));
    ASSERT_STR_EQ("nwg0", items[0].name); ASSERT_STR_EQ("", items[1].name);
    ASSERT_EQ(MT_ERR_PROTO, fill_names(&body, items, 1));
    const char *bad[] = {"[]junk", "{}", "[{},{}]junk",
        "[{\"show\":{\"interface\":{\"system-name\":\"two words\"}}},{}]"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        body.data = (char *)bad[i]; body.size = strlen(bad[i]);
        ASSERT_EQ(MT_ERR_PROTO, fill_names(&body, items, 2));
    }
    PASS();
}

typedef struct { int listen; const char *response; char request[2048]; } server_t;
static void *serve(void *ud)
{
    server_t *s = ud;
    int fd = accept(s->listen, NULL, NULL);
    if (fd < 0) { return NULL; }
    ssize_t n = read(fd, s->request, sizeof(s->request) - 1);
    if (n > 0) { s->request[n] = '\0'; }
    (void)send(fd, s->response, strlen(s->response), 0);
    close(fd); return NULL;
}
TEST transport_disables_proxy_and_redirects_and_preserves_post_body(void)
{
    ASSERT_EQ(CURLE_OK, curl_global_init(CURL_GLOBAL_DEFAULT));
    const char *responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
        "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/never\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    };
    ASSERT_EQ(0, setenv("http_proxy", "http://127.0.0.1:1", 1));
    ASSERT_EQ(0, setenv("no_proxy", "", 1));
    ASSERT_EQ(0, setenv("NO_PROXY", "", 1));
    for (size_t i = 0; i < 3; i++) {
        server_t s = {.listen = socket(AF_INET, SOCK_STREAM, 0), .response = responses[i]};
        ASSERT(s.listen >= 0);
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        ASSERT_EQ(0, bind(s.listen, (void *)&addr, sizeof(addr)));
        ASSERT_EQ(0, listen(s.listen, 1));
        socklen_t len = sizeof(addr); ASSERT_EQ(0, getsockname(s.listen, (void *)&addr, &len));
        char url[128]; snprintf(url, sizeof(url), "http://127.0.0.1:%u/rci/", (unsigned)ntohs(addr.sin_port));
        pthread_t thread; ASSERT_EQ(0, pthread_create(&thread, NULL, serve, &s));
        body_t body = {0};
        mt_err_t err = mt_kn_rci_request(url, i == 0 ? "[]" : NULL, 1024, NULL, &body.data, &body.size);
        pthread_join(thread, NULL); close(s.listen);
        ASSERT_EQ(i == 0 ? MT_OK : MT_ERR_UPSTREAM, err);
        ASSERT(strstr(s.request, i == 0 ? "POST /rci/ " : "GET /rci/ "));
        if (i == 0) { ASSERT_STR_EQ("{}", body.data); ASSERT(strstr(s.request, "Content-Length: 2")); }
        free(body.data);
    }
    unsetenv("http_proxy"); unsetenv("no_proxy"); unsetenv("NO_PROXY"); curl_global_cleanup(); PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();
    RUN_TEST(cancellation_before_io_is_bounded);
    RUN_TEST(name_batch_is_positional_and_never_guesses_missing_names);
    RUN_TEST(transport_disables_proxy_and_redirects_and_preserves_post_body);
    GREATEST_MAIN_END();
}

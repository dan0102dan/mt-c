/* Same production source with a 1s budget so the regression does not add 15s
 * to every test configuration. Neither individual response exceeds 1s. */
#define MT_SUB_FETCH_TIMEOUT_SECONDS 1
#include "greatest.h"
#include "../../src/subscriptions/fetch.c"
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>

typedef struct { int listener; unsigned port, handled; } fixture_t;
static void *serve_slow_chain(void *ud) {
    fixture_t *f = ud;
    for (unsigned i = 0; i < 2; i++) {
        struct pollfd p = {.fd = f->listener, .events = POLLIN};
        if (poll(&p, 1, 2000) <= 0) { break; }
        int fd = accept(f->listener, NULL, NULL); if (fd < 0) { break; }
        char request[1024]; (void)recv(fd, request, sizeof(request), 0);
        struct timespec pause = {.tv_nsec = 650000000}; nanosleep(&pause, NULL);
        char response[256];
        int n = i == 0 ? snprintf(response, sizeof(response),
            "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%u/final\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", f->port) :
            snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        if (n > 0) { (void)send(fd, response, (size_t)n, MSG_NOSIGNAL); }
        close(fd); f->handled++;
    }
    return NULL;
}
TEST redirect_chain_shares_one_timeout(void) {
    fixture_t f = {.listener = socket(AF_INET, SOCK_STREAM, 0)}; ASSERT(f.listener >= 0);
    struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    ASSERT_EQ(0, bind(f.listener, (void *)&address, sizeof(address)));
    ASSERT_EQ(0, listen(f.listener, 2));
    socklen_t length = sizeof(address); ASSERT_EQ(0, getsockname(f.listener, (void *)&address, &length));
    f.port = ntohs(address.sin_port);
    pthread_t thread; ASSERT_EQ(0, pthread_create(&thread, NULL, serve_slow_chain, &f));
    char url[128]; snprintf(url, sizeof(url), "http://127.0.0.1:%u/start", f.port);
    char *body = NULL; size_t len = 0;
    mt_err_t err = mt_sub_fetch_list(url, &body, &len);
    pthread_join(thread, NULL); close(f.listener);
    ASSERT_EQ(MT_ERR_IO, err); ASSERT(body == NULL); ASSERT_EQ(0u, len); ASSERT_EQ(2u, f.handled);
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    mt_sub_fetch_global_init();
    GREATEST_MAIN_BEGIN(); RUN_TEST(redirect_chain_shares_one_timeout); GREATEST_MAIN_END();
}

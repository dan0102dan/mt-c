/* Regression: iptables-restore can close stdin after poll(POLLOUT) but
 * before write. The real daemon must survive SIGPIPE with SIG_DFL and
 * propagate the child's failure instead of disappearing with status 141.
 *
 * A linker-only write wrapper delays the first write AFTER poll has already
 * indicated writable, making the race reproducible without privileges.
 */
#include "magitrickle/iptables.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n);

static bool delay_next_write;
static unsigned delayed_writes;

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (delay_next_write && n >= 65536) {
        delay_next_write = false;
        delayed_writes++;
        /* The child waits one second before exiting. We have already
         * returned from poll(POLLOUT) when this wrapper runs. */
        const struct timespec pause = {1, 500000000};
        (void)nanosleep(&pause, NULL);
    }
    return __real_write(fd, buf, n);
}

static bool check(bool ok, const char *why)
{
    if (!ok) { fprintf(stderr, "SIGPIPE regression: %s\n", why); }
    return ok;
}

static bool test_transport(mt_ipt_proto_t proto, bool initially_blocked)
{
    sigset_t pipe_set, before, after, pending;
    if (sigemptyset(&pipe_set) != 0 || sigaddset(&pipe_set, SIGPIPE) != 0) {
        return false;
    }
    if (pthread_sigmask(initially_blocked ? SIG_BLOCK : SIG_UNBLOCK,
                        &pipe_set, &before) != 0) {
        return false;
    }
    mt_ipt_executable_t *exe = mt_ipt_executable_real_new(proto);
    if (!check(exe != NULL, "create real executable")) { return false; }
    uint8_t *payload = malloc(131072);
    if (!check(payload != NULL, "allocate transcript")) {
        mt_ipt_executable_free(exe);
        return false;
    }
    memset(payload, 'x', 131072);

    delay_next_write = true;
    delayed_writes = 0;
    mt_err_t err = exe->ops->restore(exe, payload, 131072);
    delay_next_write = false;
    free(payload);
    mt_ipt_executable_free(exe);

    bool ok = check(err == MT_ERR_IO, "restore must report child failure") &&
              check(delayed_writes == 1, "write must occur after POLLOUT") &&
              check(pthread_sigmask(SIG_BLOCK, NULL, &after) == 0,
                    "read signal mask") &&
              check(sigismember(&after, SIGPIPE) == (initially_blocked ? 1 : 0),
                    "restore original SIGPIPE signal mask");
    if (ok) {
        ok = check(sigpending(&pending) == 0, "read pending signals") &&
             check(sigismember(&pending, SIGPIPE) == 0,
                   "no new pending SIGPIPE can kill us later");
    }
    (void)pthread_sigmask(SIG_SETMASK, &before, NULL);
    return ok;
}

int main(void)
{
    char dir[] = "/tmp/mt-ipt-sigpipe-XXXXXX";
    if (!check(mkdtemp(dir) != NULL, "create temporary PATH")) { return 1; }
    char path4[sizeof(dir) + 32];
    char path6[sizeof(dir) + 32];
    (void)snprintf(path4, sizeof(path4), "%s/iptables-restore", dir);
    (void)snprintf(path6, sizeof(path6), "%s/ip6tables-restore", dir);
    const char script[] = "#!/bin/sh\nsleep 1\nexit 17\n";
    bool setup = true;
    for (int i = 0; i < 2; i++) {
        const char *path = i == 0 ? path4 : path6;
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
        if (fd < 0) { setup = false; break; }
        if (write(fd, script, sizeof(script) - 1) != (ssize_t)(sizeof(script) - 1)) {
            setup = false;
        }
        if (close(fd) != 0 || chmod(path, 0700) != 0) { setup = false; }
    }
    const char *old_path = getenv("PATH");
    size_t path_len = strlen(dir) + (old_path ? strlen(old_path) : 0) + 2;
    char *path_env = malloc(path_len);
    if (!path_env) { setup = false; }
    if (path_env) {
        (void)snprintf(path_env, path_len, "%s:%s", dir, old_path ? old_path : "");
        if (setenv("PATH", path_env, 1) != 0) { setup = false; }
    }

    struct sigaction before_sigpipe, observed;
    if (sigaction(SIGPIPE, NULL, &before_sigpipe) != 0) { setup = false; }
    struct sigaction default_action = {.sa_handler = SIG_DFL};
    if (sigemptyset(&default_action.sa_mask) != 0 ||
        sigaction(SIGPIPE, &default_action, NULL) != 0) { setup = false; }
    bool ok = check(setup, "setup restore subprocess fixtures");
    if (ok) {
        ok = test_transport(MT_IPT_PROTO_IPV4, false);
        if (ok) { ok = test_transport(MT_IPT_PROTO_IPV6, true); }
    }
    if (sigaction(SIGPIPE, NULL, &observed) != 0 ||
        observed.sa_handler != SIG_DFL) {
        ok = check(false, "restore SIGPIPE disposition must remain default");
    }
    if (setup) { (void)sigaction(SIGPIPE, &before_sigpipe, NULL); }
    free(path_env);
    (void)unlink(path4);
    (void)unlink(path6);
    (void)rmdir(dir);
    printf("SIGPIPE restore regression: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* Real process handoff, no router, package-manager invocation or network access. */
#define _GNU_SOURCE
#include "../../src/platform/update.c"
#include <assert.h>

static void put_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    assert(fd >= 0);
    assert(write_all(fd, text, strlen(text)));
    assert(close(fd) == 0);
}

static void path_join(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    assert(n > 0 && (size_t)n < cap);
}

static bool wait_for_path(const char *path, bool exists) {
    for (unsigned i = 0; i < 300; i++) {
        if ((access(path, F_OK) == 0) == exists) { return true; }
        usleep(10000);
    }
    return false;
}

int main(void) {
    char root[] = "/tmp/mt-c-handoff-test-XXXXXX";
    assert(mkdtemp(root));
    char lock_path[512], bad_path[512], work[512], package[512];
    char manager[512], logger[512], started[512], finish[512], output[512];
    path_join(lock_path, sizeof(lock_path), root, "lock");
    path_join(bad_path, sizeof(bad_path), root, "symlink");
    path_join(work, sizeof(work), root, "job with spaces;'literal");
    path_join(package, sizeof(package), work, "package.ipk");
    path_join(manager, sizeof(manager), root, "fake-opkg");
    path_join(logger, sizeof(logger), root, "fake-logger");
    path_join(started, sizeof(started), root, "started");
    path_join(finish, sizeof(finish), root, "finish");
    path_join(output, sizeof(output), root, "system-output");

    int lock = lock_open(lock_path, geteuid(), true);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    assert(symlink(lock_path, bad_path) == 0);
    assert(lock_open(bad_path, geteuid(), false) < 0);
    assert(unlink(bad_path) == 0);
    assert(chmod(lock_path, 0666) == 0);
    assert(lock_open(lock_path, geteuid(), false) < 0);
    assert(chmod(lock_path, 0600) == 0);
    assert(mkdir(work, 0700) == 0);
    put_file(package, "verified test package", 0600);
    char script[4096];
    int n = snprintf(script, sizeof(script),
        "#!/bin/sh\n"
        "[ \"$1\" = install ] && [ -f \"$2\" ] || exit 91\n"
        "[ ! -e /proc/$$/fd/3 ] || exit 92\n"
        "printf started > '%s'\n"
        "while [ ! -e '%s' ]; do sleep 0.01; done\n"
        "printf 'fake package-manager failure\\n'\n"
        "exit 7\n", started, finish);
    assert(n > 0 && (size_t)n < sizeof(script));
    put_file(manager, script, 0700);
    n = snprintf(script, sizeof(script),
        "#!/bin/sh\n[ ! -e /proc/$$/fd/3 ] || exit 93\ncat > '%s'\n", output);
    assert(n > 0 && (size_t)n < sizeof(script));
    put_file(logger, script, 0700);

    /* Model the daemon terminating during a package hook. Only standard
     * utilities remain; the launcher does not wait for the fake package job. */
    pid_t launcher = fork();
    assert(launcher >= 0);
    if (launcher == 0) {
        bool ok = launch_install(lock, manager, logger, work, package, false);
        close(lock);
        _exit(ok ? 0 : 1);
    }
    close(lock);
    int status = 0;
    assert(waitpid(launcher, &status, 0) == launcher);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(wait_for_path(started, true));
    lock = lock_open(lock_path, geteuid(), false);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) < 0); /* New daemon cannot overlap. */
    put_file(finish, "finish", 0600);
    assert(wait_for_path(work, false));
    bool released = false;
    for (unsigned i = 0; i < 300; i++) {
        if (flock(lock, LOCK_EX | LOCK_NB) == 0) { released = true; break; }
        usleep(10000);
    }
    assert(released);
    close(lock);
    FILE *f = fopen(output, "r");
    assert(f);
    char line[128];
    assert(fgets(line, sizeof(line), f));
    assert(strcmp(line, "fake package-manager failure\n") == 0);
    fclose(f);
    /* No private install.log/status.json/request.json was created. */
    assert(unlink(started) == 0); assert(unlink(finish) == 0);
    assert(unlink(output) == 0); assert(unlink(manager) == 0);
    assert(unlink(logger) == 0); assert(unlink(lock_path) == 0);
    assert(rmdir(root) == 0);
    puts("single-binary package handoff, system output, lock and cleanup checks passed");
    return 0;
}

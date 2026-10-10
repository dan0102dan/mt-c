/* Exercise the real subprocess boundary, including descendants that retain stdout. */
#define _GNU_SOURCE
#include "../../src/platform/update.c"
#include <assert.h>

int main(void) {
    char path[] = "/tmp/mt-c-command-test-XXXXXX";
    assert(mkdtemp(path));
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dir >= 0);
    char *ok[] = {"/bin/sh", "-c", "printf 'package log'; exit 0", NULL};
    char *bad[] = {"/bin/sh", "-c", "exit 7", NULL};
    char *missing[] = {"/nonexistent-mt-c-test-program", NULL};
    assert(command(dir, ok));
    assert(!command(dir, bad));
    assert(!command(dir, missing));
    /* A successful intermediate fork is not enough: only a successful
     * grandchild execve() can make launch_executable() return true. */
    int lock = openat(dir, "launch.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    assert(lock >= 0);
    assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
    assert(launch_executable(lock, "/bin/true"));
    assert(!launch_executable(lock, "/nonexistent-mt-c-updater"));

    /* access(X_OK) succeeds for a corrupt binary but execve() fails with
     * ENOEXEC; the install endpoint must return 500, never a false 202. */
    char corrupt[512];
    assert(snprintf(corrupt, sizeof(corrupt), "%s/broken-updater", path) > 0);
    int bad_exec = open(corrupt, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0700);
    assert(bad_exec >= 0);
    assert(write_all(bad_exec, "not an executable", 17));
    assert(close(bad_exec) == 0);
    assert(access(corrupt, X_OK) == 0);
    assert(!launch_executable(lock, corrupt));
    assert(unlink(corrupt) == 0);
    assert(flock(lock, LOCK_UN) == 0);
    close(lock);
    assert(unlinkat(dir, "launch.lock", 0) == 0);
    char *inherited_pipe[] = {"/bin/sh", "-c", "sleep 4 & exit 0", NULL};
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(command(dir, inherited_pipe));
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    double elapsed = (double)(after.tv_sec - before.tv_sec) +
        (double)(after.tv_nsec - before.tv_nsec) / 1000000000.0;
    assert(elapsed < 3.0); /* Must not wait for the background descendant's EOF. */
    char *large[] = {"/bin/sh", "-c", "head -c 200000 /dev/zero", NULL};
    assert(command(dir, large));
    struct stat st;
    assert(fstatat(dir, "install.log", &st, 0) == 0);
    assert(st.st_size == LOG_LIMIT);
    assert(unlinkat(dir, "install.log", 0) == 0);
    close(dir);
    assert(rmdir(path) == 0);
    puts("update process boundary checks passed");
    return 0;
}

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

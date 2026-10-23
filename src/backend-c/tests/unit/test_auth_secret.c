/* Each child has a fresh process-lifetime secret cache. */
#include "greatest.h"
#include "magitrickle/auth.h"
#include "magitrickle/hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool check_secret(const char *text, size_t length, bool valid) {
    char dir[] = "/tmp/mt-auth-secret-XXXXXX";
    if (!mkdtemp(dir)) { return false; }
    char path[128];
    snprintf(path, sizeof(path), "%s/auth_secret", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { rmdir(dir); return false; }
    bool written = fwrite(text, 1, length, f) == length;
    written = fclose(f) == 0 && written;
    pid_t pid = written ? fork() : -1;
    if (pid == 0) {
        uint8_t secret[32]; size_t n = 0;
        mt_err_t err = mt_auth_load_secret(dir, secret, &n);
        _exit((valid ? err == MT_OK && n == sizeof(secret) : err != MT_OK) ? 0 : 1);
    }
    int status = 0;
    bool ok = pid > 0 && waitpid(pid, &status, 0) == pid &&
              WIFEXITED(status) && WEXITSTATUS(status) == 0;
    unlink(path); rmdir(dir);
    return ok;
}
TEST auth_secret_rejects_corrupt_lengths(void) {
    char text[400]; memset(text, 'A', sizeof(text));
    ASSERT(check_secret(text, 96, false));
    ASSERT(check_secret(text, sizeof(text), false));
    ASSERT(check_secret("Zg==", 4, false));
    ASSERT(check_secret("====", 4, false));
    ASSERT(check_secret("", 0, false));
    uint8_t bytes[32]; memset(bytes, 0xa5, sizeof(bytes));
    mt_base64_encode(bytes, sizeof(bytes), text);
    ASSERT(check_secret(text, strlen(text), true));
    size_t n = strlen(text); text[n] = '\0'; text[n + 1] = 'x';
    ASSERT(check_secret(text, n + 2, false));
    PASS();
}
GREATEST_MAIN_DEFS();
int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(auth_secret_rejects_corrupt_lengths);
    GREATEST_MAIN_END();
}

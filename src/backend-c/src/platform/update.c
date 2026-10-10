/* On-demand package updates: prepare in a daemon thread, then hand the verified
 * local package to the system package manager. No updater binary or job files. */
#define _GNU_SOURCE
#include "magitrickle/update.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>

#include "magitrickle/auth.h"
#include "magitrickle/hash.h"
#include "magitrickle/httpd.h"
#include "magitrickle/jwt.h"
#include "magitrickle/log.h"
#include "magitrickle/paths.h"
#include "magitrickle/rand.h"
#include "magitrickle/system.h"
#include "magitrickle/version.h"

#define UPDATE_LOCK MT_SOCK_PATH ".update-lock"
#if defined(MT_PLATFORM_ENTWARE)
#define INSTALL_ROOT "/opt"
#else
#define INSTALL_ROOT "/"
#endif

typedef struct update_state {
    char job_id[33], tag[80], target_version[64];
    uint32_t target_revision;
    const char *stage;
    const char *error;
} update_state_t;

typedef struct update_request {
    char tag[80], suffix[192];
    uint64_t release_id;
    bool preview;
    int lock;
    const char *manager;
    const char *logger;
} update_request_t;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static update_state_t state = {.stage = "idle", .error = ""};
static pthread_t preparation_thread;
/* Owned by the API loop, never by the preparation thread. */
static bool thread_joinable;
static atomic_bool canceled;

static const char *str(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void set_stage(const char *stage, const char *error) {
    pthread_mutex_lock(&state_mutex);
    state.stage = stage;
    state.error = error ? error : "";
    pthread_mutex_unlock(&state_mutex);
}

static bool write_all(int fd, const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { return false; }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

/* Only a runtime lock, not a saved job. Never unlink a flock inode: a process
 * which still has it open must serialize with a newly started daemon too. */
static int lock_open(const char *path, uid_t owner, bool create) {
    int flags = O_RDWR | O_NOFOLLOW | O_CLOEXEC | (create ? O_CREAT : 0);
    int fd = open(path, flags, 0600);
    struct stat st;
    if (fd < 0) { return -1; }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        st.st_nlink != 1 || (st.st_mode & 0077)) {
        close(fd); errno = EACCES; return -1;
    }
    return fd;
}

/* -1 = cannot determine, 0 = free, 1 = preparing/installing. */
static int lock_busy(void) {
    int fd = lock_open(UPDATE_LOCK, 0, false);
    if (fd < 0) { return errno == ENOENT ? 0 : -1; }
    int result = 0;
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        result = (errno == EWOULDBLOCK || errno == EAGAIN) ? 1 : -1;
    }
    close(fd);
    return result;
}

static const char *system_logger(void) {
    static const char *const paths[] = {"/opt/bin/logger", "/usr/bin/logger", "/bin/logger"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (access(paths[i], X_OK) == 0) { return paths[i]; }
    }
    return NULL;
}

/* Exact platform identity, not a guess based on uname. Unknown firmware fails
 * closed. Do not execute /etc/openwrt_release as a shell script. */
static bool identity(char *suffix, size_t cap, const char **manager) {
    if (!MT_BUILD_TARGET[0]) { return false; }
    for (const char *p = MT_BUILD_TARGET; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.')) { return false; }
    }
#if defined(MT_PLATFORM_ENTWARE)
    *manager = "/opt/bin/opkg";
    int n = snprintf(suffix, cap, "entware_%s.ipk", MT_BUILD_TARGET);
    return n > 0 && (size_t)n < cap;
#elif defined(MT_PLATFORM_OPENWRT)
    FILE *f = fopen("/etc/openwrt_release", "r");
    if (!f) { return false; }
    char line[256], release[64] = "";
    while (fgets(line, sizeof(line), f)) {
        const char *prefix = "DISTRIB_RELEASE=";
        if (strncmp(line, prefix, strlen(prefix))) { continue; }
        const char *p = line + strlen(prefix);
        char quote = (*p == '\'' || *p == '"') ? *p++ : '\0';
        size_t n = 0;
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (n + 1 == sizeof(release)) { n = 0; break; }
            release[n++] = *p++;
        }
        release[n] = '\0';
        if ((quote && *p++ != quote) || (*p && *p != '\n' && *p != '\r')) { release[0] = '\0'; }
        break;
    }
    fclose(f);
    mt_update_version_t os;
    if (!mt_update_version_parse(release, 1, &os) || os.development) { return false; }
    bool apk = os.parts[0] >= 25;
    *manager = apk ? "/sbin/apk" : "/bin/opkg";
    if (access(*manager, X_OK)) { *manager = apk ? "/usr/bin/apk" : "/usr/bin/opkg"; }
    int n = snprintf(suffix, cap, "openwrt-%s_%s.%s", release, MT_BUILD_TARGET, apk ? "apk" : "ipk");
    return n > 0 && (size_t)n < cap;
#else
    (void)suffix; (void)cap; (void)manager;
    return false;
#endif
}

static bool root_authorized(mt_http_req_t *req) {
    const char *auth = mt_http_req_header(req, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) || strlen(auth + 7) >= MT_JWT_MAX_TOKEN) { return false; }
    mt_jwt_claims_t claims;
    return mt_auth_verify_token(MT_APP_STATE_DIR, auth + 7) == MT_OK &&
           mt_jwt_parse_unverified(auth + 7, &claims) == MT_OK && strcmp(claims.sub, "root") == 0;
}

static const char *capability(mt_http_req_t *req, char *suffix, size_t cap) {
    const char *manager = NULL;
    mt_update_version_t version;
    if (!identity(suffix, cap, &manager)) { return "Unsupported build target"; }
    if (!mt_update_version_parse(MT_VERSION, MT_PACKAGE_REVISION, &version)) { return "Unknown installed version"; }
    if (geteuid() != 0 || access(manager, X_OK) || access("/bin/sh", X_OK) || !system_logger()) {
        return "Installer unavailable";
    }
    if (!root_authorized(req)) { return "Sign in as root to install updates"; }
    return NULL;
}

static void handle_info(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)ud;
    char suffix[192] = "";
    const char *reason = capability(req, suffix, sizeof(suffix));
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { mt_http_res_write_error(res, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(obj, "installed_version", MT_VERSION);
    cJSON_AddNumberToObject(obj, "installed_revision", MT_PACKAGE_REVISION);
    cJSON_AddStringToObject(obj, "asset_suffix", suffix);
    cJSON_AddBoolToObject(obj, "can_install", reason == NULL);
    cJSON_AddStringToObject(obj, "reason", reason ? reason : "");
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, obj);
}

static void handle_status(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    (void)req; (void)ud;
    pthread_mutex_lock(&state_mutex);
    update_state_t snapshot = state;
    pthread_mutex_unlock(&state_mutex);
    int busy = lock_busy();
    if (busy < 0) { mt_http_res_write_error(res, 500, "Cannot check package installation"); return; }
    /* A restarted daemon has no job history. The lock still prevents a second
     * install, but success is verified by WebUI against the running version. */
    if (busy && (!strcmp(snapshot.stage, "idle") || !strcmp(snapshot.stage, "failed"))) {
        memset(&snapshot, 0, sizeof(snapshot)); snapshot.stage = "installing";
    } else if (!busy && !strcmp(snapshot.stage, "installing")) {
        memset(&snapshot, 0, sizeof(snapshot)); snapshot.stage = "idle";
    }
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { mt_http_res_write_error(res, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(obj, "stage", snapshot.stage);
    if (snapshot.job_id[0]) {
        cJSON_AddStringToObject(obj, "job_id", snapshot.job_id);
        cJSON_AddStringToObject(obj, "tag", snapshot.tag);
        cJSON_AddStringToObject(obj, "target_version", snapshot.target_version);
        cJSON_AddNumberToObject(obj, "target_revision", snapshot.target_revision);
    }
    if (snapshot.error && snapshot.error[0]) { cJSON_AddStringToObject(obj, "error", snapshot.error); }
    mt_http_res_set_header(res, "Cache-Control", "no-store");
    mt_http_res_write_json(res, 200, obj);
}

/* The shell only connects standard utilities and removes the temporary package.
 * No second mt-c executable, updater mode, job journal or custom log file.
 * fd 3 belongs only to this shell: package hooks must not pass it to a new daemon.
 * Every argument is supplied separately, never interpolated into shell source. */
static const char install_script[] =
    "work=$1; package=$2; logger=$3; shift 3; "
    "\"$@\" \"$package\" 3>&- 2>&1 | \"$logger\" -t mt-c-install 3>&-; "
    "rm -f -- \"$package\"; rmdir -- \"$work\"";

static bool launch_install(int lock, const char *manager, const char *logger,
                           const char *work, const char *package, bool apk) {
    int ack[2];
    if (pipe2(ack, O_CLOEXEC)) { return false; }
    /* Reserve the handshake above lock fd 3 and all standard descriptors. */
    int write_fd = fcntl(ack[1], F_DUPFD_CLOEXEC, 5);
    close(ack[1]);
    if (write_fd < 0) { close(ack[0]); return false; }
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > INT32_MAX) { close(ack[0]); close(write_fd); return false; }
    char *const ipk_args[] = {"/bin/sh", "-c", (char *)install_script, "mt-c-install",
        (char *)work, (char *)package, (char *)logger, (char *)manager, "install", NULL};
    char *const apk_args[] = {"/bin/sh", "-c", (char *)install_script, "mt-c-install",
        (char *)work, (char *)package, (char *)logger, (char *)manager, "add", "--allow-untrusted", NULL};
    char *const env[] = {"PATH=/opt/sbin:/opt/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", "HOME=/root", NULL};
    pid_t child = fork();
    if (child < 0) { close(ack[0]); close(write_fd); return false; }
    if (child == 0) {
        close(ack[0]);
        if (setsid() < 0) { goto failed; }
        pid_t detached = fork();
        if (detached < 0) { goto failed; }
        if (detached > 0) { _exit(0); }
        if (lock != 3 && dup2(lock, 3) < 0) { goto failed; }
        if (fcntl(3, F_SETFD, 0) < 0) { goto failed; }
        /* write_fd is kept CLOEXEC; close every other inherited non-stdio fd. */
        for (int fd = 4; fd < max_fd; fd++) { if (fd != write_fd) { close(fd); } }
        int null = open("/dev/null", O_RDWR);
        if (null < 0) { goto failed; }
        if (dup2(null, 0) < 0 || dup2(null, 1) < 0 || dup2(null, 2) < 0) { goto failed; }
        if (null > 3) { close(null); }
        sigset_t empty;
        if (sigemptyset(&empty) || sigprocmask(SIG_SETMASK, &empty, NULL)) { goto failed; }
        /* A logging failure must not kill the package manager with SIGPIPE. */
        struct sigaction ignore = {.sa_handler = SIG_IGN};
        if (sigemptyset(&ignore.sa_mask) || sigaction(SIGPIPE, &ignore, NULL)) { goto failed; }
        execve("/bin/sh", apk ? apk_args : ipk_args, env);
failed: {
        int err = errno ? errno : EIO;
        ssize_t sent;
        do { sent = write(write_fd, &err, sizeof(err)); } while (sent < 0 && errno == EINTR);
        _exit(127);
    }
    }
    close(write_fd);
    struct pollfd pfd = {.fd = ack[0], .events = POLLIN | POLLHUP};
    int ready;
    do { ready = poll(&pfd, 1, 10000); } while (ready < 0 && errno == EINTR);
    int error = 0;
    ssize_t n = -1;
    if (ready > 0) {
        do { n = read(ack[0], &error, sizeof(error)); } while (n < 0 && errno == EINTR);
    }
    close(ack[0]);
    if (n != 0) { (void)kill(-child, SIGKILL); (void)kill(child, SIGKILL); }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return n == 0 && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

typedef struct transfer {
    char *data;
    size_t len, limit;
    int fd;
    mt_sha256_ctx_t hash;
} transfer_t;

static size_t receive(void *data, size_t size, size_t count, void *ud) {
    transfer_t *t = ud;
    if (atomic_load(&canceled) || (size && count > SIZE_MAX / size)) { return 0; }
    size_t len = size * count;
    if (len > t->limit - t->len) { return 0; }
    if (t->fd >= 0) {
        if (!write_all(t->fd, data, len)) { return 0; }
        mt_sha256_update(&t->hash, data, len);
    } else {
        char *next = realloc(t->data, t->len + len + 1);
        if (!next) { return 0; }
        t->data = next;
        memcpy(t->data + t->len, data, len);
        t->data[t->len + len] = '\0';
    }
    t->len += len;
    return len;
}

static int cancel_transfer(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud; (void)dt; (void)dn; (void)ut; (void)un;
    return atomic_load(&canceled) ? 1 : 0;
}

static CURL *http_client(transfer_t *t, const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) { return NULL; }
#define SET(option, value) do { if (curl_easy_setopt(curl, option, value) != CURLE_OK) { curl_easy_cleanup(curl); return NULL; } } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_USERAGENT, "mt-c/1");
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 180L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, t);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, cancel_transfer);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "https");
#else
    SET(CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
#undef SET
    return curl;
}

static bool fetch_metadata(transfer_t *t) {
    CURL *curl = http_client(t, MT_UPDATE_API "?per_page=100");
    if (!curl) { return false; }
    struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/vnd.github+json");
    if (!headers) { curl_easy_cleanup(curl); return false; }
    long status = 0;
    bool ok = curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
        curl_easy_perform(curl) == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

static bool download(const mt_update_asset_t *asset, transfer_t *t) {
    char url[4096];
    snprintf(url, sizeof(url), "%s", asset->url);
    for (unsigned i = 0; i < 4 && !atomic_load(&canceled); i++) {
        if (!mt_update_download_host_allowed(url) || ftruncate(t->fd, 0) || lseek(t->fd, 0, SEEK_SET) < 0) { return false; }
        t->len = 0;
        mt_sha256_init(&t->hash);
        CURL *curl = http_client(t, url);
        if (!curl) { return false; }
        CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (code == CURLE_OK && status == 200) { curl_easy_cleanup(curl); return true; }
        char *redirect = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
        bool next = code == CURLE_OK && status >= 300 && status < 400 && redirect &&
            strlen(redirect) < sizeof(url) && mt_update_download_host_allowed(redirect);
        if (next) { snprintf(url, sizeof(url), "%s", redirect); }
        curl_easy_cleanup(curl);
        if (!next) { return false; }
    }
    return false;
}

static bool space_for(const char *path, size_t size) {
    struct statvfs st;
    if (statvfs(path, &st)) { return false; }
    uint64_t need = (uint64_t)size * 6U + UINT64_C(8) * 1024U * 1024U;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize >= need;
}

/* libcurl global initialization is done in main before any threads start.
 * This thread never accesses application configuration, DNS state or the loop. */
static void *prepare_update(void *arg) {
    update_request_t *request = arg;
    const char *error = "Cannot contact GitHub; try again later";
    transfer_t metadata = {.fd = -1, .limit = MT_UPDATE_JSON_LIMIT};
    mt_update_asset_t asset;
    char temp[] = "/tmp/mt-c-update-XXXXXX", package[512] = "";
    bool temp_ready = false, handed_off = false;
    int fd = -1;
    set_stage("checking", NULL);
    if (!fetch_metadata(&metadata)) { goto done; }
    error = mt_update_resolve(metadata.data, metadata.len, request->preview, request->tag,
        request->release_id, request->suffix, MT_VERSION, MT_PACKAGE_REVISION, &asset);
    if (error) { goto done; }
    error = "Not enough free space";
    if (!space_for("/tmp", asset.size) || !space_for(INSTALL_ROOT, asset.size)) { goto done; }
    error = "Cannot prepare package download";
    if (!mkdtemp(temp)) { goto done; }
    temp_ready = true;
    bool apk = strlen(request->suffix) >= 4 &&
        !strcmp(request->suffix + strlen(request->suffix) - 4, ".apk");
    snprintf(package, sizeof(package), "%s/package.%s", temp, apk ? "apk" : "ipk");
    fd = open(package, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { goto done; }
    set_stage("downloading", NULL);
    transfer_t t = {.fd = fd, .limit = asset.size};
    error = "Package download failed";
    if (!download(&asset, &t) || fsync(fd)) { goto done; }
    close(fd); fd = -1;
    set_stage("verifying", NULL);
    uint8_t digest[32]; char hex[65];
    mt_sha256_final(&t.hash, digest);
    for (size_t i = 0; i < sizeof(digest); i++) { snprintf(hex + i * 2, 3, "%02x", digest[i]); }
    error = "Package checksum mismatch";
    if (t.len != asset.size || strcmp(hex, asset.sha256)) { goto done; }
    error = "Cannot start package manager";
    if (atomic_load(&canceled) || !launch_install(request->lock, request->manager,
                                                  request->logger, temp, package, apk)) { goto done; }
    handed_off = true;
    set_stage("installing", NULL);
    error = NULL;
done:
    if (fd >= 0) { close(fd); }
    if (!handed_off) {
        if (package[0]) { (void)unlink(package); }
        if (temp_ready) { (void)rmdir(temp); }
        set_stage("failed", atomic_load(&canceled) ? "Update preparation canceled" : error);
    }
    free(metadata.data);
    close(request->lock);
    free(request);
    return NULL;
}

void mt_update_shutdown(void) {
    atomic_store(&canceled, true);
    if (thread_joinable) {
        (void)pthread_join(preparation_thread, NULL);
        thread_joinable = false;
    }
    /* Never wait for or terminate the detached system package manager here. */
}

static void handle_install(mt_http_req_t *req, mt_http_res_t *res, void *ud) {
    mt_system_ctx_t *ctx = ud;
    char suffix[192] = "";
    const char *error = capability(req, suffix, sizeof(suffix));
    if (error) { mt_http_res_write_error(res, 403, error); return; }
    const char *site = mt_http_req_header(req, "Sec-Fetch-Site");
    const char *type = mt_http_req_header(req, "Content-Type");
    if ((site && !strcmp(site, "cross-site")) || !type || strncmp(type, "application/json", 16)) {
        mt_http_res_write_error(res, 400, "Expected same-origin JSON request"); return;
    }
    size_t len = 0;
    const uint8_t *body = mt_http_req_body(req, &len);
    cJSON *input = len && len <= 1024 && !memchr(body, '\0', len) ?
        cJSON_ParseWithLength((const char *)body, len) : NULL;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(input, "release_id");
    const cJSON *preview = cJSON_GetObjectItemCaseSensitive(input, "preview");
    mt_update_version_t target, current;
    bool valid = cJSON_IsObject(input) && cJSON_GetArraySize(input) == 3 && cJSON_IsBool(preview) &&
        cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= 9007199254740991.0 &&
        (double)(uint64_t)id->valuedouble == id->valuedouble &&
        mt_update_version_parse(str(input, "tag"), 1, &target) && !target.development &&
        mt_update_version_parse(MT_VERSION, MT_PACKAGE_REVISION, &current) &&
        mt_update_version_compare(&target, &current) > 0;
    if (!valid) { cJSON_Delete(input); mt_http_res_write_error(res, 400, "Invalid or non-newer release"); return; }
    int lock = lock_open(UPDATE_LOCK, 0, true);
    int code = 500;
    error = "Cannot prepare update";
    update_request_t *request = NULL;
    cJSON *out = NULL;
    if (lock < 0) { goto done; }
    if (flock(lock, LOCK_EX | LOCK_NB)) { code = 409; error = "An update is already running"; goto done; }
    if (thread_joinable) { (void)pthread_join(preparation_thread, NULL); thread_joinable = false; }
    if (!ctx->config_path || mt_app_save_config(ctx->app, ctx->config_path, ctx->config_version) != MT_OK) {
        error = "Cannot save configuration before update"; goto done;
    }
    request = calloc(1, sizeof(*request));
    out = cJSON_CreateObject();
    uint8_t random[16];
    if (!request || !out || mt_random_bytes(random, sizeof(random)) != MT_OK) { goto done; }
    update_state_t next = {.stage = "queued", .error = "", .target_revision = target.revision};
    for (size_t i = 0; i < sizeof(random); i++) { snprintf(next.job_id + i * 2, 3, "%02x", random[i]); }
    snprintf(next.tag, sizeof(next.tag), "%s", str(input, "tag"));
    snprintf(next.target_version, sizeof(next.target_version), "%s", target.base);
    snprintf(request->tag, sizeof(request->tag), "%s", next.tag);
    snprintf(request->suffix, sizeof(request->suffix), "%s", suffix);
    request->release_id = (uint64_t)id->valuedouble;
    request->preview = cJSON_IsTrue(preview);
    request->lock = lock;
    request->logger = system_logger();
    if (!request->logger || !identity(suffix, sizeof(suffix), &request->manager)) { goto done; }
    if (!cJSON_AddStringToObject(out, "job_id", next.job_id) ||
        !cJSON_AddStringToObject(out, "stage", "queued") ||
        !cJSON_AddStringToObject(out, "target_version", next.target_version) ||
        !cJSON_AddNumberToObject(out, "target_revision", next.target_revision)) { goto done; }
    pthread_mutex_lock(&state_mutex);
    state = next;
    pthread_mutex_unlock(&state_mutex);
    atomic_store(&canceled, false);
    if (pthread_create(&preparation_thread, NULL, prepare_update, request)) {
        set_stage("failed", "Cannot start update preparation"); goto done;
    }
    thread_joinable = true;
    request = NULL; lock = -1; /* Preparation now owns them. */
    mt_http_res_write_json(res, 202, out);
    out = NULL;
    error = NULL;
done:
    if (lock >= 0) { close(lock); }
    free(request);
    cJSON_Delete(input); cJSON_Delete(out);
    if (error) { mt_http_res_write_error(res, code, error); }
}

void mt_update_register_routes(mt_httpd_t *http, mt_system_ctx_t *ctx) {
    if (mt_httpd_route(http, "GET", "/api/v1/system/update", handle_info, ctx) != MT_OK ||
        mt_httpd_route(http, "GET", "/api/v1/system/update/status", handle_status, ctx) != MT_OK ||
        mt_httpd_route(http, "POST", "/api/v1/system/update/install", handle_install, ctx) != MT_OK) {
        MT_ERROR("cannot register update routes");
    }
}

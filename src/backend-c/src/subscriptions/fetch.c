/* See sub_fetch.h. Port of subscriptions/fetch.go's FetchList. */
#include "magitrickle/sub_fetch.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <curl/curl.h>

void mt_sub_fetch_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

void mt_sub_fetch_global_cleanup(void) { curl_global_cleanup(); }

static bool url_is_supported(const char *url) {
    CURLU *cu = curl_url();
    if (!cu) { return false; }
    bool ok = false;
    if (curl_url_set(cu, CURLUPART_URL, url, 0) == CURLUE_OK) {
        char *scheme = NULL;
        char *host = NULL;
        curl_url_get(cu, CURLUPART_SCHEME, &scheme, 0);
        curl_url_get(cu, CURLUPART_HOST, &host, 0);
        ok = scheme && host && host[0] != '\0' &&
             (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "https") == 0);
        curl_free(scheme);
        curl_free(host);
    }
    curl_url_cleanup(cu);
    return ok;
}

typedef struct fetch_buf {
    char *data;
    size_t len;
    size_t cap;
    mt_err_t error;
} fetch_buf_t;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    fetch_buf_t *buf = ud;
    if (nmemb && size > SIZE_MAX / nmemb) { buf->error = MT_ERR_LIMIT; return 0; }
    size_t n = size * nmemb;
    if (n > MT_SUB_FETCH_MAX_BODY_BYTES - buf->len) {
        buf->error = MT_ERR_LIMIT;
        return 0; /* abort the transfer */
    }
    if (buf->len + n + 1 > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < buf->len + n + 1) { new_cap *= 2; }
        if (new_cap > MT_SUB_FETCH_MAX_BODY_BYTES + 1) { new_cap = MT_SUB_FETCH_MAX_BODY_BYTES + 1; }
        char *na = realloc(buf->data, new_cap);
        if (!na) { buf->error = MT_ERR_NOMEM; return 0; }
        buf->data = na;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

static int transfer_progress(void *ud, curl_off_t total_down, curl_off_t down,
                             curl_off_t total_up, curl_off_t up) {
    (void)total_down; (void)down; (void)total_up; (void)up;
    const atomic_bool *cancel = ud;
    return cancel && atomic_load(cancel) ? 1 : 0;
}

mt_err_t mt_sub_fetch_list(const char *url, char **out_body, size_t *out_len) {
    return mt_sub_fetch_list_cancel(url, out_body, out_len, NULL);
}

mt_err_t mt_sub_fetch_list_cancel(const char *url, char **out_body, size_t *out_len,
                                  const atomic_bool *cancel) {
    if (!out_body || !out_len) { return MT_ERR_INVAL; }
    *out_body = NULL; *out_len = 0;
    if (cancel && atomic_load(cancel)) { return MT_ERR_CANCELED; }
    if (!url || !url_is_supported(url)) { return MT_ERR_INVAL; }
    CURL *curl = curl_easy_init();
    if (!curl) { return MT_ERR_NOMEM; }
    fetch_buf_t buf = {0};
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) { setup = curl_easy_setopt(curl, option, value); } } while (0)
    SET(CURLOPT_URL, url);
    /* One transfer owns the whole chain: native relative redirects, a single
     * timeout budget, and no private visited-URL cache or per-hop handles. */
    SET(CURLOPT_FOLLOWLOCATION, 1L);
    SET(CURLOPT_MAXREDIRS, (long)MT_SUB_FETCH_MAX_REDIRECTS);
    SET(CURLOPT_TIMEOUT, (long)MT_SUB_FETCH_TIMEOUT_SECONDS);
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR, "http,https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    SET(CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    SET(CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    SET(CURLOPT_WRITEFUNCTION, write_cb);
    SET(CURLOPT_WRITEDATA, &buf);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, transfer_progress);
    SET(CURLOPT_XFERINFODATA, cancel);
#undef SET
    CURLcode rc = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    if (rc == CURLE_OK) { rc = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status); }
    curl_easy_cleanup(curl);
    mt_err_t result = buf.error;
    if (cancel && atomic_load(cancel)) { result = MT_ERR_CANCELED; }
    else if (result == MT_OK && rc != CURLE_OK) {
        result = rc == CURLE_TOO_MANY_REDIRECTS ? MT_ERR_LIMIT :
                 rc == CURLE_UNSUPPORTED_PROTOCOL || rc == CURLE_URL_MALFORMAT ? MT_ERR_INVAL : MT_ERR_IO;
    }
    if (result == MT_OK && (status < 200 || status >= 300)) { result = MT_ERR_PROTO; }
    if (result != MT_OK) { free(buf.data); return result; }
    if (!buf.data) { buf.data = calloc(1, 1); }
    if (!buf.data) { return MT_ERR_NOMEM; }
    *out_body = buf.data; *out_len = buf.len;
    return MT_OK;
}

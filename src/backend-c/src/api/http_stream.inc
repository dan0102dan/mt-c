/* Private HTTP streaming implementation, included by httpd.c so connection
 * ownership is not exposed to API handlers. All operations are loop-local. */
struct mt_http_stream {
    mt_http_conn_t *conn;
    mt_http_stream_next_fn next;
    mt_http_stream_close_fn closed;
    void *ud;
    int maintenance_timer;
    uint64_t last_progress_ms, last_auth_ms;
    bool heartbeat_due, writable, failed;
};

static uint64_t stream_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0; }
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
static void stream_cleanup(mt_http_conn_t *c) {
    mt_http_stream_t *s = c->stream;
    if (!s) { return; }
    c->stream = NULL;
    if (s->maintenance_timer) {
        (void)mt_loop_del_timer(c->server->loop, s->maintenance_timer);
    }
    c->server->n_streams--;
    if (s->closed) { s->closed(s->ud); }
    free(s);
}

/* Reuse the exact admission middleware, including current password/expiry and
 * the live auth-enabled setting. Request headers outlive the stream; bodies
 * do not. A failed recheck closes the stream, not an already-sent HTTP status.
 * At most once/second on the data path, never once/DNS question. */
static bool stream_authorized(mt_http_stream_t *s, uint64_t now) {
    mt_http_conn_t *c = s->conn;
    mt_http_res_t res = {0};
    bool allowed = !c->server->middleware ||
        c->server->middleware(&c->req, &res, c->server->middleware_ud);
    free(res.body);
    if (res.deferred) { mt_http_res_cancel_defer(&res); allowed = false; }
    if (!allowed) { conn_close(c); return false; }
    s->last_auth_ms = now;
    return true;
}
void mt_httpd_revalidate_streams(mt_httpd_t *h) {
    if (!h) { return; }
    uint64_t now = stream_now_ms();
    for (mt_http_conn_t *c = h->conns, *next; c; c = next) {
        next = c->next; /* reauthorization may unlink/free this connection */
        if (c->stream) { (void)stream_authorized(c->stream, now); }
    }
}
void mt_http_stream_wake(mt_http_stream_t *s) {
    if (!s || s->writable || s->failed) { return; }
    /* Coalesce a DNS burst into a single epoll interest change. No writes,
     * allocation, callbacks or connection destruction during journal notify. */
    if (mt_loop_mod_fd(s->conn->server->loop, s->conn->fd,
                       EPOLLOUT | EPOLLIN | EPOLLRDHUP) == MT_OK) {
        s->writable = true;
    } else { s->failed = true; }
}
static void stream_maintenance(mt_loop_t *loop, void *ud) {
    (void)loop;
    mt_http_stream_t *s = ud;
    uint64_t now = stream_now_ms();
    if (s->failed || now - s->last_progress_ms >= MT_HTTPD_IDLE_TIMEOUT_MS) {
        conn_close(s->conn);
        return;
    }
    if (now - s->last_auth_ms >= MT_HTTP_STREAM_AUTH_CHECK_MS && !stream_authorized(s, now)) { return; }
    if (now - s->last_progress_ms >= MT_HTTP_STREAM_HEARTBEAT_MS) {
        s->heartbeat_due = true;
        mt_http_stream_wake(s);
    }
}

static bool stream_chunk(mt_http_conn_t *c, const char *text, size_t len) {
    if (len == 0 || len > MT_HTTP_STREAM_MAX_FRAME) { return false; }
    char prefix[32];
    int n = snprintf(prefix, sizeof(prefix), "%zx\r\n", len);
    if (n <= 0 || (size_t)n >= sizeof(prefix)) { return false; }
    size_t head = (size_t)n;
    c->wbuf = malloc(head + len + 2);
    if (!c->wbuf) { return false; }
    memcpy(c->wbuf, prefix, head);
    memcpy(c->wbuf + head, text, len);
    memcpy(c->wbuf + head + len, "\r\n", 2);
    c->wbuf_len = head + len + 2;
    c->wbuf_sent = 0;
    return true;
}

static void on_stream_io(mt_loop_t *loop, int fd, uint32_t events, void *ud) {
    (void)loop;
    mt_http_conn_t *c = ud;
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLIN)) {
        /* SSE has no client messages. EOF or a pipelined request ends it. */
        conn_close(c);
        return;
    }
    mt_http_stream_t *stream = c->stream;
    uint64_t now = stream_now_ms();
    if (stream->failed) { conn_close(c); return; }
    if (now - stream->last_auth_ms >= MT_HTTP_STREAM_AUTH_CHECK_MS &&
        !stream_authorized(stream, now)) { return; }
    /* Bound work per epoll turn, not just memory. DNS must keep running
     * even while replaying history to several browsers. */
    size_t budget = 64 * 1024;
    unsigned frames = 0;
    while (budget > 0 && frames < 32) {
        if (c->wbuf_sent < c->wbuf_len) {
            size_t remaining = c->wbuf_len - c->wbuf_sent;
            size_t want = remaining < budget ? remaining : budget;
            ssize_t n = send(fd, c->wbuf + c->wbuf_sent, want, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) { continue; }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
            if (n <= 0) { conn_close(c); return; }
            c->wbuf_sent += (size_t)n;
            budget -= (size_t)n;
            /* One persistent maintenance timer observes this timestamp;
             * no timerfd allocation/close or epoll churn on each send. */
            stream->last_progress_ms = now;
            if (c->wbuf_sent < c->wbuf_len) { continue; }
        }
        free(c->wbuf);
        c->wbuf = NULL;
        c->wbuf_len = c->wbuf_sent = 0;
        if (budget == 0) { return; }

        mt_http_stream_t *s = c->stream;
        size_t len = 0;
        char *text = s->next(s->ud, &len);
        if (!text) {
            if (len != 0) { conn_close(c); return; }
            if (s->heartbeat_due) {
                s->heartbeat_due = false;
                if (!stream_chunk(c, ": heartbeat\n\n", 13)) { conn_close(c); return; }
            } else {
                if (mt_loop_mod_fd(c->server->loop, fd, EPOLLIN | EPOLLRDHUP) != MT_OK) {
                    conn_close(c);
                } else { s->writable = false; }
                return;
            }
        } else {
            bool ok = stream_chunk(c, text, len);
            free(text);
            if (!ok) { conn_close(c); return; }
            s->heartbeat_due = false;
        }
        frames++;
    }
    /* EPOLLOUT stays armed if more work remains. */
}

static void stream_start(mt_http_conn_t *c) {
    static const char headers[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-store, no-cache, no-transform\r\n"
        "X-Accel-Buffering: no\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n\r\n";
    c->wbuf = malloc(sizeof(headers) - 1);
    if (!c->wbuf) { conn_close(c); return; }
    memcpy(c->wbuf, headers, sizeof(headers) - 1);
    c->wbuf_len = sizeof(headers) - 1;
    c->wbuf_sent = 0;
    c->server->reserved_body_bytes -= c->reserved_body_bytes;
    c->reserved_body_bytes = 0;
    free(c->rbuf); c->rbuf = NULL; c->rbuf_cap = c->rbuf_len = 0;
    c->req.body = NULL; c->req.body_len = 0;
    if (c->idle_timer_id) {
        (void)mt_loop_del_timer(c->server->loop, c->idle_timer_id);
        c->idle_timer_id = 0;
    }
    (void)mt_loop_del_fd(c->server->loop, c->fd);
    if (mt_loop_add_fd(c->server->loop, c->fd, EPOLLOUT | EPOLLIN | EPOLLRDHUP,
                       on_stream_io, c) != MT_OK) {
        conn_close(c);
    }
}

mt_http_stream_t *mt_http_stream_open(mt_http_req_t *req, mt_http_res_t *res,
    mt_http_stream_next_fn next, mt_http_stream_close_fn closed, void *ud) {
    mt_http_conn_t *c = req->conn;
    if (!c || !next || c->stream || c->pending || res->responded ||
        res->deferred || c->http_1_0 || c->server->n_streams >= MT_HTTP_STREAM_MAX_CLIENTS) {
        return NULL;
    }
    mt_http_stream_t *s = calloc(1, sizeof(*s));
    if (!s) { return NULL; }
    s->conn = c; s->next = next; s->closed = closed; s->ud = ud;
    s->writable = true;
    s->last_progress_ms = s->last_auth_ms = stream_now_ms();
    if (mt_loop_add_timer(c->server->loop, MT_HTTP_STREAM_AUTH_CHECK_MS,
        MT_HTTP_STREAM_AUTH_CHECK_MS, stream_maintenance, s, &s->maintenance_timer) != MT_OK) {
        free(s);
        return NULL;
    }
    c->stream = s;
    c->server->n_streams++;
    res->streaming = true;
    return s;
}

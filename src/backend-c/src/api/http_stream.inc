/* Private HTTP streaming implementation, included by httpd.c so connection
 * ownership is not exposed to API handlers. All operations are loop-local. */
struct mt_http_stream {
    mt_http_conn_t *conn;
    mt_http_stream_next_fn next;
    mt_http_stream_close_fn closed;
    void *ud;
    int heartbeat_timer;
    bool heartbeat_due;
};

static void stream_cleanup(mt_http_conn_t *c) {
    mt_http_stream_t *s = c->stream;
    if (!s) { return; }
    c->stream = NULL;
    if (s->heartbeat_timer) {
        (void)mt_loop_del_timer(c->server->loop, s->heartbeat_timer);
    }
    c->server->n_streams--;
    if (s->closed) { s->closed(s->ud); }
    free(s);
}

void mt_http_stream_wake(mt_http_stream_t *s) {
    if (!s) { return; }
    /* No writes and no callbacks here: safe to call during journal notify. */
    (void)mt_loop_mod_fd(s->conn->server->loop, s->conn->fd,
                         EPOLLOUT | EPOLLIN | EPOLLRDHUP);
}

static void stream_heartbeat(mt_loop_t *loop, void *ud) {
    (void)loop;
    mt_http_stream_t *s = ud;
    s->heartbeat_due = true;
    mt_http_stream_wake(s);
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
            /* Only actual write progress extends the stalled-peer timeout. */
            reset_idle_timer(c);
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
                }
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
    if (mt_loop_add_timer(c->server->loop, MT_HTTP_STREAM_HEARTBEAT_MS,
        MT_HTTP_STREAM_HEARTBEAT_MS, stream_heartbeat, s, &s->heartbeat_timer) != MT_OK) {
        free(s);
        return NULL;
    }
    c->stream = s;
    c->server->n_streams++;
    res->streaming = true;
    return s;
}

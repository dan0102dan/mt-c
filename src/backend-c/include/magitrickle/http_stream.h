/* SSE on the existing authenticated HTTP server, on the mt_loop thread.
 * Only one bounded event is retained per stream. History snapshots span
 * multiple events; the whole connection has no total-size cap.
 */
#ifndef MAGITRICKLE_HTTP_STREAM_H
#define MAGITRICKLE_HTTP_STREAM_H
#include "magitrickle/httpd.h"

#define MT_HTTP_STREAM_MAX_FRAME ((size_t)64 * 1024)
#define MT_HTTP_STREAM_MAX_CLIENTS 8
#define MT_HTTP_STREAM_HEARTBEAT_MS 15000
#define MT_HTTP_STREAM_AUTH_CHECK_MS 1000

typedef struct mt_http_stream mt_http_stream_t;
/* Return malloc'd SSE text, NULL/len=0 when caught up, NULL/len=SIZE_MAX
 * on failure. Do not close the stream from inside next(). */
typedef char *(*mt_http_stream_next_fn)(void *ud, size_t *len);
typedef void (*mt_http_stream_close_fn)(void *ud);
/* Called after middleware. On success closed(ud) runs exactly once on
 * disconnect/timeout/teardown. On NULL the caller still owns ud. */
mt_http_stream_t *mt_http_stream_open(mt_http_req_t *req, mt_http_res_t *res,
    mt_http_stream_next_fn next, mt_http_stream_close_fn closed, void *ud);
void mt_http_stream_wake(mt_http_stream_t *stream);
#endif

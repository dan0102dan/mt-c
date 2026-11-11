/* Optional process-owned application history. Off at startup, RAM only.
 * GET/read/subscribe never enables recording. Stop frees server history;
 * bytes already received by a browser remain that browser's property.
 */
#ifndef MAGITRICKLE_LOG_HISTORY_H
#define MAGITRICKLE_LOG_HISTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "magitrickle/log.h"

#define MT_LOG_SESSION_SIZE 33
#define MT_LOG_HISTORY_SUBSCRIBERS 8

typedef struct mt_log_history_state {
    bool enabled;
    char session[MT_LOG_SESSION_SIZE];
    uint64_t revision, count, dropped;
} mt_log_history_state_t;
typedef struct mt_log_history_piece {
    mt_log_level_t level;
    size_t length, copied;
} mt_log_history_piece_t;

/* Thread-safe. A successful new start creates a fresh session; repeated
 * requests for the current state are idempotent. A failed start stays off. */
bool mt_log_history_enable(bool enabled);
void mt_log_history_state(mt_log_history_state_t *out);
/* Copy only this slice while holding the journal lock; never expose a pointer
 * into storage that Stop can free. false means stale session/ID/offset. */
bool mt_log_history_read(const char *session, uint64_t id, size_t offset,
    void *out, size_t capacity, mt_log_history_piece_t *piece);
/* fd is a NONBLOCK pipe write end, borrowed until unsubscribe. Wakeups are
 * coalesced, never contain log data, and never allocate per viewer/record.
 * The owner must unsubscribe before closing either pipe end. */
int mt_log_history_subscribe(int fd);
void mt_log_history_ack(int subscription);
void mt_log_history_unsubscribe(int subscription);
#endif

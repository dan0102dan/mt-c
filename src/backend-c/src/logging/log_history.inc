/* Included by log.c. Append-only while enabled; no row/byte eviction quota.
 * Immutable owned records and short slice copies make Stop safe against readers.
 * The journal lives across runtime/DNS reconstruction, not across process exits.
 */
typedef struct history_entry {
    size_t length;
    mt_log_level_t level;
    unsigned char bytes[];
} history_entry_t;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Even = stopped, odd = recording. Also rejects a writer crossing Stop/Start. */
static _Atomic uint64_t history_revision;
static history_entry_t **history_rows;
static size_t history_count, history_capacity;
static uint64_t history_dropped;
static char history_session[MT_LOG_SESSION_SIZE];
static struct { bool used, pending; int fd; }
    history_watchers[MT_LOG_HISTORY_SUBSCRIBERS];

/* Called under history_mutex. A full pipe already represents a pending wakeup.
 * No per-record queue, HTTP callback or recursive logging from this path. */
static void history_notify(void)
{
    for (size_t i = 0; i < MT_LOG_HISTORY_SUBSCRIBERS; i++) {
        if (!history_watchers[i].used || history_watchers[i].pending) continue;
        ssize_t n;
        do { n = write(history_watchers[i].fd, "!", 1); } while (n < 0 && errno == EINTR);
        if (n == 1 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            history_watchers[i].pending = true;
        }
    }
}

static void history_append(mt_log_level_t level, const char *text, size_t len)
{
    uint64_t revision = atomic_load_explicit(&history_revision, memory_order_acquire);
    if (!(revision & 1u)) return; /* Off is allocation/lock/wakeup free. */
    history_entry_t *entry = len <= SIZE_MAX - sizeof(*entry)
        ? malloc(sizeof(*entry) + len) : NULL;
    if (entry) {
        entry->length = len; entry->level = level;
        memcpy(entry->bytes, text, len);
    }
    (void)pthread_mutex_lock(&history_mutex);
    if (revision != atomic_load_explicit(&history_revision, memory_order_relaxed)) {
        (void)pthread_mutex_unlock(&history_mutex);
        free(entry);
        return;
    }
    if (entry && history_count == history_capacity) {
        size_t next = history_capacity ? history_capacity * 2 : 64;
        history_entry_t **rows = next > history_capacity && next <= SIZE_MAX / sizeof(*rows)
            ? realloc(history_rows, next * sizeof(*rows)) : NULL;
        if (rows) { history_rows = rows; history_capacity = next; }
        else { free(entry); entry = NULL; }
    }
    if (entry) history_rows[history_count++] = entry;
    else if (history_dropped != UINT64_MAX) history_dropped++;
    history_notify();
    (void)pthread_mutex_unlock(&history_mutex);
}

bool mt_log_history_enable(bool enabled)
{
    (void)pthread_mutex_lock(&history_mutex);
    uint64_t revision = atomic_load_explicit(&history_revision, memory_order_relaxed);
    if (enabled == !!(revision & 1u)) {
        (void)pthread_mutex_unlock(&history_mutex);
        return true;
    }
    history_entry_t **retired = NULL;
    size_t retired_count = 0;
    if (enabled) {
        unsigned char bytes[16];
        if (mt_random_bytes(bytes, sizeof(bytes)) != MT_OK) {
            (void)pthread_mutex_unlock(&history_mutex);
            return false;
        }
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(bytes); i++) {
            history_session[i * 2] = hex[bytes[i] >> 4];
            history_session[i * 2 + 1] = hex[bytes[i] & 15u];
        }
        history_session[32] = '\0';
        history_dropped = 0;
    } else {
        retired = history_rows; retired_count = history_count;
        history_rows = NULL; history_count = history_capacity = 0;
    }
    atomic_store_explicit(&history_revision, revision + 1, memory_order_release);
    history_notify();
    (void)pthread_mutex_unlock(&history_mutex);
    /* Detached under the lock: readers never hold a borrowed record pointer. */
    for (size_t i = 0; i < retired_count; i++) free(retired[i]);
    free(retired);
    return true;
}

void mt_log_history_state(mt_log_history_state_t *out)
{
    (void)pthread_mutex_lock(&history_mutex);
    out->revision = atomic_load_explicit(&history_revision, memory_order_relaxed);
    out->enabled = !!(out->revision & 1u);
    memcpy(out->session, history_session, sizeof(out->session));
    out->count = (uint64_t)history_count;
    out->dropped = history_dropped;
    (void)pthread_mutex_unlock(&history_mutex);
}

bool mt_log_history_read(const char *session, uint64_t id, size_t offset,
    void *out, size_t capacity, mt_log_history_piece_t *piece)
{
    bool ok = false;
    (void)pthread_mutex_lock(&history_mutex);
    if (session && strcmp(session, history_session) == 0 && id && id <= history_count) {
        const history_entry_t *row = history_rows[(size_t)id - 1];
        if (offset < row->length && out && capacity && piece) {
            size_t len = row->length - offset;
            if (len > capacity) len = capacity;
            memcpy(out, row->bytes + offset, len);
            *piece = (mt_log_history_piece_t){.level = row->level,
                .length = row->length, .copied = len};
            ok = true;
        }
    }
    (void)pthread_mutex_unlock(&history_mutex);
    return ok;
}

int mt_log_history_subscribe(int fd)
{
    int result = 0;
    (void)pthread_mutex_lock(&history_mutex);
    for (size_t i = 0; fd >= 0 && i < MT_LOG_HISTORY_SUBSCRIBERS; i++) {
        if (history_watchers[i].used) continue;
        history_watchers[i].used = true;
        history_watchers[i].pending = false;
        history_watchers[i].fd = fd;
        result = (int)i + 1;
        break;
    }
    (void)pthread_mutex_unlock(&history_mutex);
    return result;
}
void mt_log_history_ack(int subscription)
{
    if (subscription < 1 || subscription > MT_LOG_HISTORY_SUBSCRIBERS) return;
    (void)pthread_mutex_lock(&history_mutex);
    history_watchers[subscription - 1].pending = false;
    (void)pthread_mutex_unlock(&history_mutex);
}
void mt_log_history_unsubscribe(int subscription)
{
    if (subscription < 1 || subscription > MT_LOG_HISTORY_SUBSCRIBERS) return;
    (void)pthread_mutex_lock(&history_mutex);
    history_watchers[subscription - 1].used = false;
    history_watchers[subscription - 1].pending = false;
    (void)pthread_mutex_unlock(&history_mutex);
}

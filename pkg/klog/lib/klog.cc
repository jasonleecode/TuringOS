#include <l4/klog/klog.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <time.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* ANSI 颜色码（仅控制台使用）                                         */
/* ------------------------------------------------------------------ */
#define COL_RED     "\033[31m"
#define COL_YELLOW  "\033[33m"
#define COL_DIM     "\033[2m"
#define COL_RESET   "\033[0m"

static const char *level_color(int lvl)
{
    if (lvl <= KLOG_ERR)  return COL_RED;
    if (lvl == KLOG_WARN) return COL_YELLOW;
    if (lvl >= KLOG_DEBUG) return COL_DIM;
    return "";
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                          */
/* ------------------------------------------------------------------ */
struct klog_entry {
    unsigned long long ts_us;
    unsigned long long seq;   /* 单调递增序号，用于追加去重 */
    int  level;
    int  facility;
    char msg[200];
};

static klog_entry      g_ring[KLOG_RING_SIZE];
static int             g_head          = 0;
static int             g_count         = 0;
static int             g_console_level = KLOG_INFO;
static unsigned long long g_global_seq = 0;   /* 下一条的序号 */
static unsigned long long g_file_seq   = 0;   /* 已写入文件的最大序号 */
static pthread_mutex_t g_lock          = PTHREAD_MUTEX_INITIALIZER;

static char g_log_path[256] = KLOG_FILE_PATH;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static const char *level_str(int lvl)
{
    static const char *t[] = {
        "EMERG", "ALERT", "CRIT ", "ERR  ", "WARN ", "NOTE ", "INFO ", "DEBUG"
    };
    return (lvl >= 0 && lvl <= 7) ? t[lvl] : "?    ";
}

static const char *fac_str(int fac)
{
    static const char *t[] = {
        "kern ", "shell", "net  ", "fs   ", "blk  ", "rtc  ", "user ", "disp "
    };
    return (fac >= 0 && fac <= 7) ? t[fac] : "user ";
}

static unsigned long long uptime_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (unsigned long long)ts.tv_sec  * 1000000ULL
             + (unsigned long long)ts.tv_nsec / 1000ULL;
    return 0;
}

/* 格式化成单行文本（控制台用，带颜色） */
static void fmt_entry_console(char *buf, size_t n, const klog_entry *e)
{
    unsigned long long sec = e->ts_us / 1000000ULL;
    unsigned long long us  = e->ts_us % 1000000ULL;
    const char *col   = level_color(e->level);
    const char *reset = (*col) ? COL_RESET : "";
    snprintf(buf, n, "[%5llu.%06llu] %s%s%s %s: %s",
             sec, us, col, level_str(e->level), reset,
             fac_str(e->facility), e->msg);
}

/* 格式化成单行文本（文件用，无颜色码） */
static void fmt_entry_file(char *buf, size_t n, const klog_entry *e)
{
    unsigned long long sec = e->ts_us / 1000000ULL;
    unsigned long long us  = e->ts_us % 1000000ULL;
    snprintf(buf, n, "[%5llu.%06llu] %s %s: %s",
             sec, us, level_str(e->level), fac_str(e->facility), e->msg);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void klog_init(const char *log_path)
{
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_head       = 0;
    g_count      = 0;
    g_global_seq = 0;
    g_file_seq   = 0;
    if (log_path)
        snprintf(g_log_path, sizeof(g_log_path), "%s", log_path);
    pthread_mutex_unlock(&g_lock);

    klog_info(KLOG_KERN, "klog: ring buffer ready (%d slots) → %s",
              KLOG_RING_SIZE, g_log_path);
}

void klog_write(int level, int facility, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    size_t len = strlen(msg);
    if (len > 0 && msg[len - 1] == '\n')
        msg[--len] = '\0';

    pthread_mutex_lock(&g_lock);

    klog_entry *e = &g_ring[g_head];
    e->ts_us    = uptime_us();
    e->seq      = ++g_global_seq;
    e->level    = level;
    e->facility = facility;
    memcpy(e->msg, msg, len + 1);

    g_head = (g_head + 1) % KLOG_RING_SIZE;
    if (g_count < KLOG_RING_SIZE) g_count++;

    bool do_print = (level <= g_console_level);
    bool do_flush = (level <= KLOG_WARN);

    pthread_mutex_unlock(&g_lock);

    if (do_print) {
        char line[320];
        fmt_entry_console(line, sizeof(line), e);
        printf("%s\n", line);
    }

    if (do_flush)
        klog_flush();
}

void klog_flush(void)
{
    pthread_mutex_lock(&g_lock);

    /* 快照 ring buffer */
    klog_entry snap[KLOG_RING_SIZE];
    int  count    = g_count;
    int  head     = g_head;
    unsigned long long file_seq = g_file_seq;
    memcpy(snap, g_ring, sizeof(snap));

    pthread_mutex_unlock(&g_lock);

    /* 收集 seq > file_seq 的新条目 */
    int start = (head - count + KLOG_RING_SIZE) % KLOG_RING_SIZE;
    int new_count = 0;
    for (int i = 0; i < count; i++) {
        if (snap[(start + i) % KLOG_RING_SIZE].seq > file_seq)
            new_count++;
    }
    if (new_count == 0) return;

    FILE *f = fopen(g_log_path, "a");
    if (!f) return;

    unsigned long long max_seq = file_seq;
    for (int i = 0; i < count; i++) {
        const klog_entry *e = &snap[(start + i) % KLOG_RING_SIZE];
        if (e->seq <= file_seq) continue;
        char line[320];
        fmt_entry_file(line, sizeof(line), e);
        fprintf(f, "%s\n", line);
        if (e->seq > max_seq) max_seq = e->seq;
    }
    fclose(f);

    pthread_mutex_lock(&g_lock);
    if (max_seq > g_file_seq) g_file_seq = max_seq;
    pthread_mutex_unlock(&g_lock);
}

void klog_dump(int min_level)
{
    pthread_mutex_lock(&g_lock);
    int count = g_count;
    int head  = g_head;
    klog_entry snap[KLOG_RING_SIZE];
    memcpy(snap, g_ring, sizeof(snap));
    pthread_mutex_unlock(&g_lock);

    int shown = 0;
    int start = (head - count + KLOG_RING_SIZE) % KLOG_RING_SIZE;
    for (int i = 0; i < count; i++) {
        const klog_entry *e = &snap[(start + i) % KLOG_RING_SIZE];
        if (e->level > min_level) continue;
        char line[320];
        fmt_entry_console(line, sizeof(line), e);
        printf("%s\n", line);
        shown++;
    }
    if (shown == 0)
        printf("(no log entries at this level)\n");
}

void klog_clear(void)
{
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_head  = 0;
    g_count = 0;
    pthread_mutex_unlock(&g_lock);
}

void klog_set_console_level(int level)
{
    pthread_mutex_lock(&g_lock);
    g_console_level = level;
    pthread_mutex_unlock(&g_lock);
}

int klog_count(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

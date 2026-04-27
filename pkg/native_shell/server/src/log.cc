#include "log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Ring buffer                                                          */
/* ------------------------------------------------------------------ */
struct klog_entry {
    unsigned long long ts_us;
    int  level;
    int  facility;
    char msg[216];
};

static klog_entry      g_ring[KLOG_RING_SIZE];
static int             g_head          = 0;
static int             g_count         = 0;
static int             g_dirty         = 0;
static int             g_console_level = KLOG_INFO;
static pthread_mutex_t g_lock          = PTHREAD_MUTEX_INITIALIZER;

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
        "kern ", "shell", "net  ", "fs   ", "blk  ", "rtc  ", "user "
    };
    return (fac >= 0 && fac <= 6) ? t[fac] : "user ";
}

static unsigned long long uptime_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (unsigned long long)ts.tv_sec  * 1000000ULL
             + (unsigned long long)ts.tv_nsec / 1000ULL;
    return 0;
}

/* 将一条 entry 格式化成单行文本，不带尾部换行 */
static void fmt_entry(char *buf, size_t n, const klog_entry *e)
{
    unsigned long long sec = e->ts_us / 1000000ULL;
    unsigned long long us  = e->ts_us % 1000000ULL;
    snprintf(buf, n, "[%5llu.%06llu] %s %s: %s",
             sec, us, level_str(e->level), fac_str(e->facility), e->msg);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void klog_init(void)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_head  = 0;
    g_count = 0;
    g_dirty = 0;

    /* 创建日志目录（失败不报错，可能已存在） */
    mkdir("/ext4/var",     0755);
    mkdir("/ext4/var/log", 0755);

    klog_info(KLOG_KERN, "klog: ring buffer ready (%d slots)", KLOG_RING_SIZE);
}

void klog_write(int level, int facility, const char *fmt, ...)
{
    char msg[216];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* 去掉末尾换行，格式化时统一加 */
    size_t len = strlen(msg);
    if (len > 0 && msg[len - 1] == '\n')
        msg[--len] = '\0';

    pthread_mutex_lock(&g_lock);

    klog_entry *e = &g_ring[g_head];
    e->ts_us    = uptime_us();
    e->level    = level;
    e->facility = facility;
    memcpy(e->msg, msg, len + 1);

    g_head = (g_head + 1) % KLOG_RING_SIZE;
    if (g_count < KLOG_RING_SIZE) g_count++;
    g_dirty++;

    bool print   = (level <= g_console_level);
    bool do_flush = (level <= KLOG_ERR);

    pthread_mutex_unlock(&g_lock);

    if (print) {
        char line[280];
        fmt_entry(line, sizeof(line), e);
        printf("%s\n", line);
    }

    if (do_flush)
        klog_flush();
}

void klog_flush(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_dirty == 0) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    /* 快照 ring buffer，释放锁后再做 I/O */
    klog_entry snap[KLOG_RING_SIZE];
    int count = g_count;
    int head  = g_head;
    memcpy(snap, g_ring, sizeof(snap));
    g_dirty = 0;
    pthread_mutex_unlock(&g_lock);

    FILE *f = fopen(KLOG_FILE_PATH, "w");
    if (!f) return;

    int start = (head - count + KLOG_RING_SIZE) % KLOG_RING_SIZE;
    for (int i = 0; i < count; i++) {
        char line[280];
        fmt_entry(line, sizeof(line), &snap[(start + i) % KLOG_RING_SIZE]);
        fprintf(f, "%s\n", line);
    }
    fclose(f);
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
        char line[280];
        fmt_entry(line, sizeof(line), e);
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
    g_dirty = 0;
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

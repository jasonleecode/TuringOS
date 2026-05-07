#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* Log levels — Linux syslog 兼容 */
enum klog_level {
    KLOG_EMERG  = 0,
    KLOG_ALERT  = 1,
    KLOG_CRIT   = 2,
    KLOG_ERR    = 3,
    KLOG_WARN   = 4,
    KLOG_NOTICE = 5,
    KLOG_INFO   = 6,
    KLOG_DEBUG  = 7,
};

/* 设施 */
enum klog_facility {
    KLOG_KERN  = 0,
    KLOG_SHELL = 1,
    KLOG_NET   = 2,
    KLOG_FS    = 3,
    KLOG_BLK   = 4,
    KLOG_RTC   = 5,
    KLOG_USER  = 6,
    KLOG_DISP  = 7,
};

#define KLOG_FILE_PATH  "/ext4/var/log/syslog.txt"
#define KLOG_RING_SIZE  256

/*
 * log_path: 日志文件路径；传 NULL 则使用 KLOG_FILE_PATH。
 * 无 ext4 访问权限的进程 fopen 会静默失败，控制台输出仍正常。
 */
void klog_init(const char *log_path);
void klog_write(int level, int facility, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void klog_flush(void);   /* 将新条目 append 到日志文件 */
void klog_dump(int min_level);
void klog_clear(void);
void klog_set_console_level(int level);
int  klog_count(void);

/* 快捷宏 */
#define klog_emerg(fac, fmt, ...)  klog_write(KLOG_EMERG,  fac, fmt, ##__VA_ARGS__)
#define klog_err(fac, fmt, ...)    klog_write(KLOG_ERR,    fac, fmt, ##__VA_ARGS__)
#define klog_warn(fac, fmt, ...)   klog_write(KLOG_WARN,   fac, fmt, ##__VA_ARGS__)
#define klog_info(fac, fmt, ...)   klog_write(KLOG_INFO,   fac, fmt, ##__VA_ARGS__)
#define klog_dbg(fac, fmt, ...)    klog_write(KLOG_DEBUG,  fac, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

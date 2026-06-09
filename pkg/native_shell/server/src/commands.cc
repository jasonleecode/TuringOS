#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <readline/history.h>
#include <pthread.h>

#include <l4/re/env>
#include <l4/re/util/cap_alloc>
#include <l4/sys/factory>
#include <ext4_file_proto.h>
#include <l4/rtc/rtc.h>
#include <l4/util/util.h>
#include <l4/vbus/vbus>
#include <l4/vbus/vbus_gpio>
#include <l4/ds18b20/ds18b20.h>
#include <l4/tef6686hn/tef6686hn.h>

#include "commands.h"
#include "shell_exec.h"
#include "log.h"
#include <spawn_ipc.h>

shell_cmd commands[] = {
    /* general */
    { "help",       "List available commands",              cmd_help       },
    { "echo",       "Print arguments  [-n: no newline]",   cmd_echo       },
    { "info",       "Print system information",             cmd_info       },
    { "clear",      "Clear the screen",                    cmd_clear      },
    { "history",    "Show command history",                 cmd_history    },
    { "exit",       "Exit the shell",                      cmd_exit       },
    /* filesystem */
    { "pwd",        "Print working directory",              cmd_pwd        },
    { "cd",         "Change directory  [dir]",              cmd_cd         },
    { "ls",         "List directory    [dir]",              cmd_ls         },
    { "cat",        "Print file        <file>",             cmd_cat        },
    { "mkdir",      "Create directory  <dir>",              cmd_mkdir      },
    { "rm",         "Remove file       <file>",             cmd_rm         },
    { "tmpfstest",  "Self-test the /tmp RAM filesystem",    cmd_tmpfstest  },
    /* system */
    { "uname",      "Print OS/arch info",                  cmd_uname      },
    { "env",        "Print environment variables",          cmd_env        },
    { "date",       "Print current date and time",          cmd_date       },
    { "uptime",     "Show system uptime",                   cmd_uptime     },
    { "list_tasks", "List background tasks",                cmd_list_tasks },
    { "top",        "Process monitor (q to quit)",          cmd_top        },
    { "dmesg",      "Show/manage kernel log  [-c] [-l N] [-n N] [--save]", cmd_dmesg },
    /* program execution */
    { "run",        "Run a program  <rom/xxx|/ext4/...> [&]",   cmd_run  },
    { "wait",       "Wait for background job  <handle>",         cmd_wait },
    { "kill",       "Kill background job  <handle>",             cmd_kill },
    /* hardware */
    { "temp",       "Read DS18B20 temperature  [pin]",                 cmd_temp  },
    { "radio",      "TEF6686HN radio  <init|tune|seek|status|...>",    cmd_radio },
    /* network */
    { "net",        "Start TCP echo server (virtio-net + lwIP)  [status]", cmd_net      },
    { "ifconfig",   "Show network interfaces and IP info",                  cmd_ifconfig },
    { "dhcp",       "Acquire IP via DHCP  [release]",                       cmd_dhcp     },
    { "ping",       "ICMP ping  <host> [-c count]",                        cmd_ping     },
    { "nslookup",   "DNS lookup  <hostname>",                              cmd_nslookup },
    { "udp",        "Start UDP echo server on port 5001",                  cmd_udp      },
    /* net-cluster — outward connectivity */
    { "mqtt",       "MQTT client  pub|sub <broker> <topic> [msg|secs]",    cmd_mqtt     },
    { "telnetd",    "Telnet server (single session)  [port]",              cmd_telnetd  },
};

int num_commands = sizeof(commands) / sizeof(commands[0]);

/* ------------------------------------------------------------------ */
/* General commands                                                     */
/* ------------------------------------------------------------------ */

void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Available commands:\n");
    for (int i = 0; i < num_commands; i++)
        printf("  %-10s  %s\n", commands[i].name, commands[i].help);
}

void cmd_echo(int argc, char **argv)
{
    int newline = 1;
    int start   = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start   = 2;
    }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        printf("%s", argv[i]);
    }
    if (newline) putchar('\n');
}

void cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("OS:           TuringOS\n");
#if defined(__aarch64__)
    printf("Architecture: aarch64\n");
#elif defined(__x86_64__)
    printf("Architecture: x86_64\n");
#elif defined(__arm__)
    printf("Architecture: arm (armv7)\n");
#elif defined(__i386__)
    printf("Architecture: i386\n");
#else
    printf("Architecture: unknown\n");
#endif
    printf("Runtime:      L4Re / Fiasco microkernel\n");
}

void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
    fflush(stdout);
}

void cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    HIST_ENTRY **list = history_list();
    if (!list) {
        printf("No history.\n");
        return;
    }
    for (int i = 0; list[i]; i++)
        printf("  %3d  %s\n", i + 1, list[i]->line);
}

void cmd_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("Goodbye.\n");
    exit(0);
}

/* ------------------------------------------------------------------ */
/* Filesystem commands                                                  */
/* ------------------------------------------------------------------ */

void cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        printf("%s\n", cwd);
    else
        printf("pwd: %s\n", strerror(errno));
}

void cmd_cd(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    if (chdir(path) != 0)
        printf("cd: %s: %s\n", path, strerror(errno));
}

static int ls_name_cmp(const void *a, const void *b)
{
    return strcmp(*static_cast<char const *const *>(a),
                  *static_cast<char const *const *>(b));
}

void cmd_ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";
    DIR *dir = opendir(path);
    if (!dir) {
        printf("ls: %s: %s\n", path, strerror(errno));
        return;
    }

    enum { MAX_ENTRIES = 1024, TERM_COLS = 80 };
    char **names = static_cast<char **>(malloc(MAX_ENTRIES * sizeof(char *)));
    int    n     = 0;
    size_t maxw  = 0;

    struct dirent *entry;
    while (names && n < MAX_ENTRIES && (entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        /* Use d_type set by ext4 dirinfo type tag; avoid stat() which would
         * block indefinitely on non-VFS caps (sigma0, factory, etc.). */
        bool   is_dir = (entry->d_type == DT_DIR);
        size_t nlen   = strlen(entry->d_name);
        size_t len    = nlen + (is_dir ? 1 : 0);   // displayed width incl. '/'
        char  *s      = static_cast<char *>(malloc(len + 1));
        if (!s) break;
        memcpy(s, entry->d_name, nlen);
        if (is_dir) s[nlen] = '/';
        s[len] = '\0';
        names[n++] = s;
        if (len > maxw) maxw = len;
    }
    closedir(dir);

    if (n == 0) { free(names); return; }

    /* Sort alphabetically, like GNU ls. */
    qsort(names, n, sizeof(char *), ls_name_cmp);

    /* Column-major layout (entries run DOWN each column, then across) that
     * fits TERM_COLS, with a 2-space gap between columns. */
    size_t colw  = maxw + 2;
    int    ncols = (int)(TERM_COLS / colw);
    if (ncols < 1) ncols = 1;
    int    nrows = (n + ncols - 1) / ncols;

    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++) {
            int idx = c * nrows + r;
            if (idx >= n) continue;
            bool last_on_row = ((c + 1) * nrows + r >= n);  // no entry to the right
            if (last_on_row) printf("%s", names[idx]);
            else             printf("%-*s", (int)colw, names[idx]);
        }
        printf("\n");
    }

    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return;
    }
    for (int i = 1; i < argc && !g_shell_interrupt; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            printf("cat: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        char buf[512];
        size_t n;
        while (!g_shell_interrupt && (n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
    }
}

// Convert a POSIX path (absolute or cwd-relative) to an absolute ext4 path.
// Ext4 VFS prefix is "/ext4".  Returns false if not on ext4.
static bool posix_to_ext4(const char *posix, char *out, size_t outsz)
{
    char abs[1024];
    if (posix[0] == '/') {
        strncpy(abs, posix, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = '\0';
    } else {
        char cwd[768];
        if (!getcwd(cwd, sizeof(cwd))) return false;
        snprintf(abs, sizeof(abs), "%s/%s", cwd, posix);
    }
    if (strncmp(abs, "/ext4", 5) != 0) return false;
    const char *rest = abs + 5;
    if (*rest == '\0') { snprintf(out, outsz, "/"); return true; }
    if (*rest != '/')  return false;
    snprintf(out, outsz, "%s", rest);
    return true;
}

static L4::Cap<Ext4_dir_ops> ext4_dir_cap()
{
    return L4Re::Env::env()->get_cap<Ext4_dir_ops>("ext4");
}

void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: mkdir <dir>\n");
        return;
    }
    auto cap = ext4_dir_cap();
    if (!cap.is_valid()) {
        printf("mkdir: no ext4 filesystem\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        char ext4p[512];
        if (!posix_to_ext4(argv[i], ext4p, sizeof(ext4p))) {
            printf("mkdir: %s: not on ext4\n", argv[i]);
            continue;
        }
        long r = cap->ext_mkdir(
            L4::Ipc::Array<char const, unsigned long>(strlen(ext4p), ext4p));
        if (r == -L4_EEXIST)
            printf("mkdir: %s: File exists\n", argv[i]);
        else if (r < 0)
            printf("mkdir: %s: failed (%ld)\n", argv[i], r);
    }
}

void cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rm <file>\n");
        return;
    }
    auto cap = ext4_dir_cap();
    if (!cap.is_valid()) {
        printf("rm: no ext4 filesystem\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        char ext4p[512];
        if (!posix_to_ext4(argv[i], ext4p, sizeof(ext4p))) {
            printf("rm: %s: not on ext4\n", argv[i]);
            continue;
        }
        long r = cap->ext_unlink(
            L4::Ipc::Array<char const, unsigned long>(strlen(ext4p), ext4p));
        if (r < 0)
            printf("rm: %s: No such file or directory\n", argv[i]);
    }
}

/* Exercise the in-RAM /tmp filesystem entirely through libc (open/write/
 * lseek/read/mkdir/opendir/readdir/unlink/stat) — this is exactly the path
 * libc tmpfile()/mkstemp() rely on, and doubles as a permanent self-test. */
void cmd_tmpfstest(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *path = "/tmp/tmpfstest.txt";
    const char *data = "hello tmpfs streaming 12345";
    const size_t dlen = strlen(data);
    int fails = 0;
    char buf[64];
    struct stat st;

#define T(cond, msg) do { \
        if (cond) printf("  ok   %s\n", (msg)); \
        else { printf("  FAIL %s\n", (msg)); ++fails; } \
    } while (0)

    /* create + write */
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    T(fd >= 0, "open(/tmp/.. , O_CREAT|O_RDWR)");
    ssize_t w = (fd >= 0) ? write(fd, data, dlen) : -1;
    T(w == (ssize_t)dlen, "write");

    /* lseek + read back */
    T(fd >= 0 && lseek(fd, 0, SEEK_SET) == 0, "lseek SEEK_SET");
    memset(buf, 0, sizeof(buf));
    ssize_t r = (fd >= 0) ? read(fd, buf, sizeof(buf) - 1) : -1;
    T(r == (ssize_t)dlen && memcmp(buf, data, dlen) == 0, "read back matches");
    if (fd >= 0) close(fd);

    /* stat */
    T(stat(path, &st) == 0 && (off_t)st.st_size == (off_t)dlen
      && S_ISREG(st.st_mode), "stat size + mode");

    /* mkdir + file inside subdir */
    mkdir("/tmp/sub", 0755);   /* tolerate EEXIST from a prior run */
    int fd2 = open("/tmp/sub/inner.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    T(fd2 >= 0, "create file in /tmp/sub");
    if (fd2 >= 0) { write(fd2, "x", 1); close(fd2); }

    /* opendir + readdir lists both entries */
    int saw_file = 0, saw_dir = 0;
    DIR *d = opendir("/tmp");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (strcmp(e->d_name, "tmpfstest.txt") == 0) saw_file = 1;
            if (strcmp(e->d_name, "sub") == 0)           saw_dir  = 1;
        }
        closedir(d);
    }
    T(d != nullptr && saw_file && saw_dir, "opendir/readdir lists entries");

    /* unlink + confirm gone */
    T(unlink(path) == 0, "unlink");
    T(stat(path, &st) != 0, "file gone after unlink");

    /* cleanup subdir */
    unlink("/tmp/sub/inner.txt");
    rmdir("/tmp/sub");

#undef T
    if (fails == 0) printf("tmpfstest: PASS\n");
    else            printf("tmpfstest: FAIL (%d)\n", fails);
}

/* ------------------------------------------------------------------ */
/* System commands                                                      */
/* ------------------------------------------------------------------ */

void cmd_uname(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("TuringOS  Fiasco/L4Re  ");
#if defined(__aarch64__)
    printf("aarch64");
#elif defined(__x86_64__)
    printf("x86_64");
#elif defined(__arm__)
    printf("armv7");
#elif defined(__i386__)
    printf("i386");
#else
    printf("unknown");
#endif
    printf("\n");
}

void cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    extern char **environ;
    if (!environ) {
        printf("(no environment)\n");
        return;
    }
    for (char **e = environ; *e; e++)
        printf("%s\n", *e);
}

static time_t utc_mktime(int y, int mo, int d, int h, int mi, int s)
{
    static const int mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int  is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int  years   = y - 1970;
    long leap    = (years + 1) / 4 - (years + 1) / 100 + (years + 1) / 400;
    long days    = static_cast<long>(years) * 365 + leap
                   + mdays[mo - 1] + (mo > 2 && is_leap ? 1 : 0) + d - 1;
    return static_cast<time_t>(days * 86400L + h * 3600 + mi * 60 + s);
}

void cmd_date(int argc, char **argv)
{
    struct timespec mono, real;
    char buf[64];

    if (argc >= 3 && strcmp(argv[1], "-s") == 0) {
        int y, mo, d, h, mi, s;
        if (sscanf(argv[2], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6
            || mo < 1 || mo > 12 || d < 1 || d > 31
            || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59) {
            printf("Usage: date -s \"YYYY-MM-DD HH:MM:SS\"\n");
            return;
        }

        l4_cap_idx_t rtc_cap = l4re_env_get_cap("rtc");
        if (l4_is_invalid_cap(rtc_cap)) {
            printf("date: RTC service not available\n");
            return;
        }

        time_t t = utc_mktime(y, mo, d, h, mi, s);

        clock_gettime(CLOCK_MONOTONIC, &mono);
        l4_uint64_t uptime_ns  = static_cast<l4_uint64_t>(mono.tv_sec) * 1000000000ULL
                                 + static_cast<l4_uint64_t>(mono.tv_nsec);
        l4_uint64_t new_offset = static_cast<l4_uint64_t>(t) * 1000000000ULL - uptime_ns;

        if (l4rtc_set_offset_to_realtime(rtc_cap, new_offset) == 0) {
            struct tm *tm = localtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", tm);
            printf("Date set to: %s\n", buf);
        } else {
            printf("date: failed to set RTC time\n");
        }
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME,  &real);

    if (real.tv_sec > 946684800L) {
        struct tm *tm = localtime(&real.tv_sec);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", tm);
        printf("%s\n", buf);
    } else {
        long h = mono.tv_sec / 3600;
        long m = (mono.tv_sec % 3600) / 60;
        long s = mono.tv_sec % 60;
        printf("(RTC unavailable) uptime: %02ld:%02ld:%02ld\n", h, m, s);
    }
}

void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    long total = mono.tv_sec;
    long days  = total / 86400;
    long h     = (total % 86400) / 3600;
    long m     = (total % 3600) / 60;
    long s     = total % 60;
    if (days > 0)
        printf("up %ld day%s, %02ld:%02ld:%02ld\n", days, days == 1 ? "" : "s", h, m, s);
    else
        printf("up %02ld:%02ld:%02ld\n", h, m, s);
}

/* ------------------------------------------------------------------ */
/* Hardware commands                                                    */
/* ------------------------------------------------------------------ */

void cmd_temp(int argc, char **argv)
{
    int pin = 4;
    if (argc >= 2)
        pin = atoi(argv[1]);

    auto vbus = L4Re::Env::env()->get_cap<L4vbus::Vbus>("vbus");
    if (!vbus) {
        printf("temp: 'vbus' capability not available\n");
        return;
    }

    L4vbus::Device gpio_dev;
    if (vbus->root().device_by_hid(&gpio_dev, "gpio") < 0) {
        printf("temp: GPIO device not found on vbus\n");
        return;
    }

    Ds18b20 sensor(L4vbus::Gpio_pin(gpio_dev, pin));
    if (!sensor.present()) {
        printf("temp: no device on GPIO pin %d\n", pin);
        return;
    }

    int temp_c100;
    if (sensor.read_temp_c100(&temp_c100) != L4_EOK) {
        printf("temp: read failed\n");
        return;
    }

    bool neg   = temp_c100 < 0;
    int  abs_v = neg ? -temp_c100 : temp_c100;
    printf("%s%d.%02d °C\n", neg ? "-" : "", abs_v / 100, abs_v % 100);
}

/* ------------------------------------------------------------------ */
/* Radio command (TEF6686HN FM/AM tuner)                               */
/* ------------------------------------------------------------------ */

static Tef6686hn *g_radio = nullptr;

/* Open the I2C device for the TEF6686HN (addr 0x1C) via the factory cap. */
static L4::Cap<I2c_device_ops> radio_open_i2c()
{
    auto factory = L4Re::Env::env()->get_cap<L4::Factory>("i2c");
    if (!factory.is_valid())
    {
        printf("radio: 'i2c' capability not available\n");
        return L4::Cap<I2c_device_ops>::Invalid;
    }

    auto dev = L4Re::Util::cap_alloc.alloc<I2c_device_ops>();
    if (!dev.is_valid())
    {
        printf("radio: capability allocation failed\n");
        return L4::Cap<I2c_device_ops>::Invalid;
    }

    char addr_str[16];
    snprintf(addr_str, sizeof(addr_str), "addr=%x",
             static_cast<unsigned>(Tef6686hn::I2c_addr));

    long r = l4_error(factory->create(dev, 1L)
                      << static_cast<char const *>(addr_str));
    if (r < 0)
    {
        printf("radio: I2C device create failed (%ld)\n", r);
        L4Re::Util::cap_alloc.free(dev);
        return L4::Cap<I2c_device_ops>::Invalid;
    }

    return dev;
}

/* Parse FM frequency string "98.0" or "98" → kHz (98000). */
static l4_uint32_t parse_fm_mhz(const char *s)
{
    const char *dot = strchr(s, '.');
    if (!dot)
        return static_cast<l4_uint32_t>(atoi(s)) * 1000;

    l4_uint32_t whole = static_cast<l4_uint32_t>(atoi(s));
    l4_uint32_t frac  = 0;
    int         digits = 0;
    for (const char *p = dot + 1; *p && digits < 3; ++p, ++digits)
        frac = frac * 10 + static_cast<l4_uint32_t>(*p - '0');
    while (digits++ < 3)
        frac *= 10;
    return whole * 1000 + frac;
}

static void radio_print_quality(Tef6686hn::Quality const &q)
{
    printf("  RSSI %5.1f dBuV   SNR %5.1f dB   MP %3d   offset %+d Hz\n",
           q.rssi      / 10.0,
           q.snr       / 10.0,
           static_cast<int>(q.multipath),
           static_cast<int>(q.freq_offset) * 10);
}

static void radio_help()
{
    printf("Usage: radio <subcommand> [args]\n");
    printf("  init               Initialize the chip (run first)\n");
    printf("  tune <MHz>         FM tune, e.g. 'radio tune 98.0'\n");
    printf("  tune mw <kHz>      MW tune, e.g. 'radio tune mw 999'\n");
    printf("  tune lw <kHz>      LW tune, e.g. 'radio tune lw 198'\n");
    printf("  seek [up|down]     Seek next FM station\n");
    printf("  status             Current frequency and signal quality\n");
    printf("  volume <0-255>     Set output level\n");
    printf("  mute               Mute audio\n");
    printf("  unmute             Unmute audio\n");
    printf("  rds                Read one RDS group\n");
}

void cmd_radio(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0)
    {
        radio_help();
        return;
    }

    /* ---- init ---- */
    if (strcmp(argv[1], "init") == 0)
    {
        if (g_radio)
        {
            printf("radio: already initialized\n");
            return;
        }
        auto i2c = radio_open_i2c();
        if (!i2c.is_valid())
            return;

        g_radio = new Tef6686hn(i2c);
        long r = g_radio->init();
        if (r < 0)
        {
            printf("radio: init failed (%ld)\n", r);
            delete g_radio;
            g_radio = nullptr;
            return;
        }
        printf("radio: TEF6686HN initialized\n");
        return;
    }

    if (!g_radio)
    {
        printf("radio: not initialized — run 'radio init' first\n");
        return;
    }

    /* ---- tune ---- */
    if (strcmp(argv[1], "tune") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: radio tune <MHz>  |  radio tune mw <kHz>  |  radio tune lw <kHz>\n");
            return;
        }

        Tef6686hn::Band band  = Tef6686hn::Band::FM;
        l4_uint32_t     freq  = 0;

        if (strcmp(argv[2], "mw") == 0 || strcmp(argv[2], "MW") == 0)
        {
            if (argc < 4) { printf("Usage: radio tune mw <kHz>\n"); return; }
            band = Tef6686hn::Band::MW;
            freq = static_cast<l4_uint32_t>(atoi(argv[3]));
        }
        else if (strcmp(argv[2], "lw") == 0 || strcmp(argv[2], "LW") == 0)
        {
            if (argc < 4) { printf("Usage: radio tune lw <kHz>\n"); return; }
            band = Tef6686hn::Band::LW;
            freq = static_cast<l4_uint32_t>(atoi(argv[3]));
        }
        else
        {
            freq = parse_fm_mhz(argv[2]);
        }

        long r = g_radio->tune(band, freq);
        if (r < 0)
        {
            printf("radio: tune failed (%ld)\n", r);
            return;
        }

        l4_usleep(50000); /* 50 ms for AFC to settle */

        if (band == Tef6686hn::Band::FM)
        {
            l4_uint32_t actual = 0;
            g_radio->get_frequency(&actual);
            printf("radio: tuned to %u.%u MHz\n",
                   actual / 1000, (actual % 1000) / 100);
        }
        else
        {
            printf("radio: tuned to %u kHz\n", freq);
        }
        return;
    }

    /* ---- seek ---- */
    if (strcmp(argv[1], "seek") == 0)
    {
        Tef6686hn::Seek_dir dir = Tef6686hn::Seek_dir::Up;
        if (argc >= 3 && strcmp(argv[2], "down") == 0)
            dir = Tef6686hn::Seek_dir::Down;

        printf("radio: seeking %s...\n",
               dir == Tef6686hn::Seek_dir::Up ? "up" : "down");

        long r = g_radio->seek(dir);
        if (r == L4_EOK)
        {
            l4_uint32_t freq = 0;
            g_radio->get_frequency(&freq);
            printf("radio: found %u.%u MHz\n",
                   freq / 1000, (freq % 1000) / 100);
            Tef6686hn::Quality q = {};
            if (g_radio->get_quality(&q) == L4_EOK)
                radio_print_quality(q);
        }
        else if (r == -L4_ENOENT)
        {
            printf("radio: no station found\n");
        }
        else
        {
            printf("radio: seek error (%ld)\n", r);
        }
        return;
    }

    /* ---- status ---- */
    if (strcmp(argv[1], "status") == 0)
    {
        l4_uint32_t freq = 0;
        if (g_radio->get_frequency(&freq) == L4_EOK)
            printf("radio: %u.%u MHz\n",
                   freq / 1000, (freq % 1000) / 100);
        else
            printf("radio: frequency unavailable\n");

        Tef6686hn::Quality q = {};
        if (g_radio->get_quality(&q) == L4_EOK)
            radio_print_quality(q);
        return;
    }

    /* ---- volume ---- */
    if (strcmp(argv[1], "volume") == 0)
    {
        if (argc < 3) { printf("Usage: radio volume <0-255>\n"); return; }
        int v = atoi(argv[2]);
        if (v < 0 || v > 255) { printf("radio: volume must be 0-255\n"); return; }
        long r = g_radio->set_output_level(static_cast<l4_uint16_t>(v));
        if (r < 0)
            printf("radio: volume set failed (%ld)\n", r);
        else
            printf("radio: volume set to %d\n", v);
        return;
    }

    /* ---- mute / unmute ---- */
    if (strcmp(argv[1], "mute") == 0)
    {
        long r = g_radio->mute(true);
        if (r < 0) printf("radio: mute failed (%ld)\n", r);
        else        printf("radio: muted\n");
        return;
    }
    if (strcmp(argv[1], "unmute") == 0)
    {
        long r = g_radio->mute(false);
        if (r < 0) printf("radio: unmute failed (%ld)\n", r);
        else        printf("radio: unmuted\n");
        return;
    }

    /* ---- rds ---- */
    if (strcmp(argv[1], "rds") == 0)
    {
        Tef6686hn::Rds_group g = {};
        long r = g_radio->get_rds_group(&g);
        if (r < 0)
        {
            printf("radio: RDS read error (%ld)\n", r);
            return;
        }
        if (!g.valid)
        {
            printf("radio: no RDS data available\n");
            return;
        }
        printf("RDS  A=%04X  B=%04X  C=%04X  D=%04X  err=%02X\n",
               g.block[0], g.block[1], g.block[2], g.block[3], g.err_bits);

        /* Decode group 0A: programme service name */
        unsigned group_type  = (g.block[1] >> 12) & 0xF;
        unsigned version_bit = (g.block[1] >> 11) & 0x1;
        if (group_type == 0 && !version_bit)
        {
            unsigned seg = g.block[1] & 0x03;
            char c0 = static_cast<char>(g.block[3] >> 8);
            char c1 = static_cast<char>(g.block[3] & 0xFF);
            printf("     PS[%u%u] = '%c%c'\n",
                   seg * 2, seg * 2 + 1,
                   (c0 >= 0x20 && c0 < 0x7F) ? c0 : '?',
                   (c1 >= 0x20 && c1 < 0x7F) ? c1 : '?');
        }
        return;
    }

    printf("radio: unknown subcommand '%s' — try 'radio help'\n", argv[1]);
}

/* ------------------------------------------------------------------ */
/* Background task registry                                             */
/* ------------------------------------------------------------------ */

static constexpr int MAX_TASKS = 16;

struct task_entry {
    char        name[64];
    char        desc[64];
    l4_uint32_t handle;  // 0 = internal task, non-zero = spawnd process handle
    bool        active;
};

static task_entry   g_tasks[MAX_TASKS];
static int          g_task_count = 0;
static pthread_mutex_t g_task_mtx = PTHREAD_MUTEX_INITIALIZER;

void task_register(const char *name, const char *desc)
{
    pthread_mutex_lock(&g_task_mtx);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].active && g_tasks[i].handle == 0
            && strcmp(g_tasks[i].name, name) == 0) {
            pthread_mutex_unlock(&g_task_mtx);
            return;
        }
    }
    if (g_task_count < MAX_TASKS) {
        snprintf(g_tasks[g_task_count].name, sizeof(g_tasks[0].name), "%s", name);
        snprintf(g_tasks[g_task_count].desc, sizeof(g_tasks[0].desc), "%s", desc);
        g_tasks[g_task_count].handle = 0;
        g_tasks[g_task_count].active = true;
        g_task_count++;
    }
    pthread_mutex_unlock(&g_task_mtx);
}

void task_unregister(const char *name)
{
    pthread_mutex_lock(&g_task_mtx);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].active && g_tasks[i].handle == 0
            && strcmp(g_tasks[i].name, name) == 0) {
            g_tasks[i].active = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_task_mtx);
}

void task_register_proc(l4_uint32_t handle, const char *name)
{
    pthread_mutex_lock(&g_task_mtx);
    if (g_task_count < MAX_TASKS) {
        snprintf(g_tasks[g_task_count].name, sizeof(g_tasks[0].name), "%s", name);
        g_tasks[g_task_count].desc[0] = '\0';
        g_tasks[g_task_count].handle  = handle;
        g_tasks[g_task_count].active  = true;
        g_task_count++;
    }
    pthread_mutex_unlock(&g_task_mtx);
}

bool task_name_by_handle(l4_uint32_t handle, char *buf, int bufsz)
{
    pthread_mutex_lock(&g_task_mtx);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].active && g_tasks[i].handle == handle) {
            snprintf(buf, bufsz, "%s", g_tasks[i].name);
            pthread_mutex_unlock(&g_task_mtx);
            return true;
        }
    }
    pthread_mutex_unlock(&g_task_mtx);
    return false;
}

void task_unregister_proc(l4_uint32_t handle)
{
    pthread_mutex_lock(&g_task_mtx);
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].active && g_tasks[i].handle == handle) {
            g_tasks[i].active = false;
            break;
        }
    }
    pthread_mutex_unlock(&g_task_mtx);
}

void cmd_list_tasks(int argc, char **argv)
{
    (void)argc; (void)argv;
    pthread_mutex_lock(&g_task_mtx);
    bool any = false;
    for (int i = 0; i < g_task_count; i++) {
        if (!g_tasks[i].active) continue;
        if (!any) {
            printf("%-8s  %s\n", "HANDLE", "NAME");
            printf("%-8s  %s\n", "------", "----");
            any = true;
        }
        if (g_tasks[i].handle) {
            printf("%-8u  %s\n", g_tasks[i].handle, g_tasks[i].name);
        } else {
            if (g_tasks[i].desc[0])
                printf("%-8s  %s (%s)\n", "-", g_tasks[i].name, g_tasks[i].desc);
            else
                printf("%-8s  %s\n", "-", g_tasks[i].name);
        }
    }
    if (!any)
        printf("No background tasks running.\n");
    pthread_mutex_unlock(&g_task_mtx);
}

/* ------------------------------------------------------------------ */
/* Program execution — shared spawnd cap + job table                   */
/* ------------------------------------------------------------------ */

static L4::Cap<Spawn_svr> g_spawnd;

static L4::Cap<Spawn_svr> spawnd_cap()
{
    if (!g_spawnd.is_valid())
        g_spawnd = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
    return g_spawnd;
}


/* ------------------------------------------------------------------ */
/* Program execution commands                                            */
/* ------------------------------------------------------------------ */

/* Pack argv[0..] into a NUL-separated buffer, double-NUL terminated.
 * Returns total bytes written, including the final extra NUL. */
static size_t pack_argv(char *buf, size_t bufsz, char *const *argv)
{
    size_t pos = 0;
    for (int i = 0; argv[i] && pos + 2 < bufsz; i++) {
        size_t len = strlen(argv[i]);
        if (pos + len + 2 >= bufsz) break;
        memcpy(buf + pos, argv[i], len + 1);
        pos += len + 1;
    }
    if (pos < bufsz) buf[pos++] = '\0';  /* double-NUL terminator */
    return pos;
}

void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: run <program> [args...]\n");
        printf("  run rom/hello         - Run from ROM\n");
        printf("  run hello             - Run from ROM (short form)\n");
        printf("  run /ext4/prog        - Run from ext4 filesystem\n");
        return;
    }

    auto spawnd = spawnd_cap();
    if (!spawnd.is_valid()) {
        printf("run: spawnd not available (no 'spawnd' cap)\n");
        klog_warn(KLOG_SHELL, "run: spawnd cap not found");
        return;
    }

    const char *program = argv[1];
    bool background = g_run_background;
    g_run_background = false;

    klog_info(KLOG_SHELL, "run: starting %s%s", program, background ? " &" : "");

    /* Pack argv into IPC format: "prog\0arg1\0arg2\0\0" */
    char args_buf[1024];
    size_t args_len = pack_argv(args_buf, sizeof(args_buf), &argv[1]);

    l4_uint32_t flags = background ? SPAWN_BG : SPAWN_WAIT;
    long ret = spawnd->spawn(
        L4::Ipc::Array<char const>(strlen(program) + 1, program),
        L4::Ipc::Array<char const>(args_len, args_buf),
        flags);

    if (ret < 0) {
        klog_err(KLOG_SHELL, "run: spawn failed (%ld)", ret);
        printf("run: failed to start %s (err=%ld)\n", program, ret);
        return;
    }

    if (background) {
        klog_info(KLOG_SHELL, "run: %s started in background (handle=%ld)", program, ret);
        printf("[%ld] %s\n", ret, program);
        task_register_proc((l4_uint32_t)ret, program);
        return;
    }

    /* SPAWN_WAIT: ret is exit code */
    klog_info(KLOG_SHELL, "run: %s exited code=%ld", program, ret);
    printf("run: task exited with code: %ld\n", ret);
}

void cmd_wait(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: wait <handle>\n");
        return;
    }
    auto spawnd = spawnd_cap();
    if (!spawnd.is_valid()) {
        printf("wait: spawnd not available\n");
        return;
    }
    l4_uint32_t handle = (l4_uint32_t)atoi(argv[1]);
    long ret = spawnd->wait(handle, WAIT_BLOCK);
    if (ret == -L4_ENOENT) {
        printf("wait: unknown handle %u\n", handle);
        return;
    }
    task_unregister_proc(handle);
    klog_info(KLOG_SHELL, "wait: handle=%u exited code=%ld", handle, ret);
    printf("[%u] done  exit code: %ld\n", handle, ret);
}

void cmd_kill(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: kill <handle>\n");
        return;
    }
    auto spawnd = spawnd_cap();
    if (!spawnd.is_valid()) {
        printf("kill: spawnd not available\n");
        return;
    }
    l4_uint32_t handle = (l4_uint32_t)atoi(argv[1]);
    long ret = spawnd->kill(handle);
    if (ret == -L4_ENOENT) {
        printf("kill: unknown handle %u\n", handle);
        return;
    }
    task_unregister_proc(handle);
    klog_info(KLOG_SHELL, "kill: handle=%u terminated", handle);
    printf("[%u] killed\n", handle);
}

/* ------------------------------------------------------------------ */
/* dmesg — kernel log viewer                                           */
/* ------------------------------------------------------------------ */

void cmd_dmesg(int argc, char **argv)
{
    int  min_level = KLOG_DEBUG;  /* 默认显示全部 */
    bool do_clear  = false;
    bool do_save   = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            do_clear = true;
        } else if ((strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "-n") == 0)
                   && i + 1 < argc) {
            min_level = atoi(argv[++i]);
            if (strcmp(argv[i - 1], "-n") == 0) {
                /* -n 设置控制台输出级别，不打印日志 */
                klog_set_console_level(min_level);
                printf("console log level set to %d\n", min_level);
                return;
            }
        } else if (strcmp(argv[i], "--save") == 0 || strcmp(argv[i], "-s") == 0) {
            do_save = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: dmesg [-c] [-l LEVEL] [-n LEVEL] [--save]\n");
            printf("  (no args)  show ring buffer (all levels)\n");
            printf("  -l N       show entries at level <= N  (0=EMERG .. 7=DEBUG)\n");
            printf("  -n N       set console output level (suppresses lower-priority prints)\n");
            printf("  -c         clear ring buffer after printing\n");
            printf("  --save     flush ring buffer to %s\n", KLOG_FILE_PATH);
            printf("Levels: 0 EMERG  1 ALERT  2 CRIT  3 ERR  4 WARN  5 NOTE  6 INFO  7 DEBUG\n");
            return;
        } else {
            printf("dmesg: unknown option '%s' (try --help)\n", argv[i]);
            return;
        }
    }

    if (do_save) {
        klog_flush();
        printf("log saved → %s  (%d entries)\n", KLOG_FILE_PATH, klog_count());
    } else {
        klog_dump(min_level);
    }

    if (do_clear) {
        klog_clear();
        printf("ring buffer cleared\n");
    }
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <readline/history.h>
#include <l4/re/env.h>
#include <l4/rtc/rtc.h>
#include "commands.h"

struct shell_cmd commands[] = {
    /* general */
    { "help",    "List available commands",              cmd_help    },
    { "echo",    "Print arguments  [-n: no newline]",   cmd_echo    },
    { "info",    "Print system information",             cmd_info    },
    { "clear",   "Clear the screen",                    cmd_clear   },
    { "history", "Show command history",                 cmd_history },
    { "exit",    "Exit the shell",                      cmd_exit    },
    /* filesystem */
    { "pwd",     "Print working directory",              cmd_pwd     },
    { "cd",      "Change directory  [dir]",              cmd_cd      },
    { "ls",      "List directory    [dir]",              cmd_ls      },
    { "cat",     "Print file        <file>",             cmd_cat     },
    { "mkdir",   "Create directory  <dir>",              cmd_mkdir   },
    { "rm",      "Remove file       <file>",             cmd_rm      },
    /* system */
    { "uname",   "Print OS/arch info",                  cmd_uname   },
    { "env",     "Print environment variables",          cmd_env     },
    { "date",    "Print current date and time",          cmd_date    },
    /* hardware */
    { "temp",    "Read DS18B20 temperature  [pin]",      cmd_temp    },
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

void cmd_ls(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : ".";
    DIR *dir = opendir(path);
    if (!dir) {
        printf("ls: %s: %s\n", path, strerror(errno));
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* skip . and .. */
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        /* append '/' for directories when we can stat */
        char full[4096];
        struct stat st;
        snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            printf("%s/\n", entry->d_name);
        else
            printf("%s\n", entry->d_name);
    }
    closedir(dir);
}

void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            printf("cat: %s: %s\n", argv[i], strerror(errno));
            continue;
        }
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
    }
}

void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: mkdir <dir>\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) != 0)
            printf("mkdir: %s: %s\n", argv[i], strerror(errno));
    }
}

void cmd_rm(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: rm <file>\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        if (remove(argv[i]) != 0)
            printf("rm: %s: %s\n", argv[i], strerror(errno));
    }
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

/* Portable UTC broken-down time -> Unix timestamp (avoids timegm dependency) */
static time_t utc_mktime(int y, int mo, int d, int h, int mi, int s)
{
    static const int mdays[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int is_leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int years   = y - 1970;
    /* Leap days between 1970 and start of year y */
    long leap   = (years + 1) / 4 - (years + 1) / 100 + (years + 1) / 400;
    long days   = (long)years * 365 + leap
                  + mdays[mo - 1] + (mo > 2 && is_leap ? 1 : 0) + d - 1;
    return (time_t)(days * 86400L + h * 3600 + mi * 60 + s);
}

void cmd_date(int argc, char **argv)
{
    struct timespec mono, real;
    char buf[64];

    /* ---- date -s "YYYY-MM-DD HH:MM:SS" ---- */
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

        /* offset = desired_realtime_ns - current_uptime_ns */
        clock_gettime(CLOCK_MONOTONIC, &mono);
        l4_uint64_t uptime_ns  = (l4_uint64_t)mono.tv_sec * 1000000000ULL
                                 + (l4_uint64_t)mono.tv_nsec;
        l4_uint64_t new_offset = (l4_uint64_t)t * 1000000000ULL - uptime_ns;

        if (l4rtc_set_offset_to_realtime(rtc_cap, new_offset) == 0) {
            struct tm *tm = gmtime(&t);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", tm);
            printf("Date set to: %s\n", buf);
        } else {
            printf("date: failed to set RTC time\n");
        }
        return;
    }

    /* ---- display current time ---- */
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME,  &real);

    /* Jan 1 2000 = 946684800 — anything above means RTC is valid */
    if (real.tv_sec > 946684800L) {
        struct tm *tm = gmtime(&real.tv_sec);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", tm);
        printf("%s\n", buf);
    } else {
        long h = mono.tv_sec / 3600;
        long m = (mono.tv_sec % 3600) / 60;
        long s = mono.tv_sec % 60;
        printf("(RTC unavailable) uptime: %02ld:%02ld:%02ld\n", h, m, s);
    }
}

/* ------------------------------------------------------------------ */
/* Hardware commands                                                    */
/* ------------------------------------------------------------------ */

void cmd_temp(int argc, char **argv)
{
    int pin = 4; /* default GPIO pin, same as ds18b20 example */
    if (argc >= 2)
        pin = atoi(argv[1]);

    int temp_c100;
    if (ds18b20_read_temp(pin, &temp_c100) != 0)
        return;

    int neg   = (temp_c100 < 0);
    int abs_v = neg ? -temp_c100 : temp_c100;
    printf("%s%d.%02d °C\n", neg ? "-" : "", abs_v / 100, abs_v % 100);
}

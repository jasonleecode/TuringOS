/*
 * cmd_top.cc — htop-style process monitor for TuringOS native_shell.
 *
 * Uses termbox2.h (L4Re-compatible TUI) for rendering and spawnd's
 * task_count / task_stat RPCs for process data.
 * CPU% is computed from two l4_thread_stats_time() snapshots 800 ms apart.
 */
#include "commands.h"
#include "log.h"
#include "termbox2.h"

#include <spawn_ipc.h>
#include <l4/re/env>
#include <l4/re/mem_alloc>
#include <l4/sys/ipc.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <time.h>

static constexpr int SAMPLE_MS  = 800;
static constexpr int MAX_SLOTS  = 32;
static constexpr int TICK_MS    = 50;   /* poll interval for 'q' */

struct Top_entry {
    l4_uint32_t handle;
    l4_uint32_t state;      /* 1=running  2=exited */
    l4_uint64_t cpu_us;     /* current snapshot */
    l4_uint64_t prev_us;    /* previous snapshot */
    char        name[64];
};

static Top_entry g_entries[MAX_SLOTS];
static int       g_nvalid;

/* Defined in commands.cc, declared in commands.h */
extern bool task_name_by_handle(l4_uint32_t handle, char *buf, int bufsz);

/* ------------------------------------------------------------------ */
/* Sample spawnd                                                        */
/* ------------------------------------------------------------------ */
static void sample(L4::Cap<Spawn_svr> spawnd)
{
    /* Shift previous snapshots */
    for (int i = 0; i < g_nvalid; i++)
        g_entries[i].prev_us = g_entries[i].cpu_us;

    /* Build a fresh ordered list from raw slots 0..31 */
    Top_entry next[MAX_SLOTS];
    int nnext = 0;

    for (int slot = 0; slot < MAX_SLOTS && nnext < MAX_SLOTS; slot++) {
        l4_uint64_t cpu_us = 0;
        l4_uint32_t state  = 0;
        l4_uint32_t handle = 0;
        if (spawnd->task_stat((l4_uint32_t)slot, cpu_us, state, handle) < 0)
            continue;

        Top_entry &e = next[nnext++];
        e.cpu_us  = cpu_us;
        e.state   = state;
        e.handle  = handle;

        /* Try to carry over prev_us from the previous sample */
        e.prev_us = 0;
        for (int j = 0; j < g_nvalid; j++) {
            if (g_entries[j].handle == handle) {
                e.prev_us = g_entries[j].prev_us; /* already shifted above */
                break;
            }
        }

        if (!task_name_by_handle(handle, e.name, sizeof(e.name)))
            snprintf(e.name, sizeof(e.name), "[%u]", handle);
    }

    memcpy(g_entries, next, nnext * sizeof(Top_entry));
    g_nvalid = nnext;
}

/* ------------------------------------------------------------------ */
/* Memory stats                                                         */
/* ------------------------------------------------------------------ */
static void mem_stats(unsigned long *total_mb, unsigned long *free_mb)
{
    L4Re::Mem_alloc::Stats st{};
    *total_mb = 0; *free_mb = 0;
    if (L4Re::Env::env()->mem_alloc()->info(st) == 0) {
        *total_mb = (unsigned long)(st.mem_limit / (1024 * 1024));
        *free_mb  = (unsigned long)(st.mem_free  / (1024 * 1024));
    }
}

/* ------------------------------------------------------------------ */
/* Render one frame                                                     */
/* ------------------------------------------------------------------ */
static void render(l4_uint64_t delta_us)
{
    int w = tb_width();
    int h = tb_height();
    tb_clear();

    /* Row 0: title bar */
    time_t now = time(nullptr);
    struct tm *tm_info = localtime(&now);
    char timebuf[12] = "--:--:--";
    if (tm_info)
        snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d",
                 tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    unsigned long total_mb, free_mb;
    mem_stats(&total_mb, &free_mb);

    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    long up_total = mono.tv_sec;
    long up_days  = up_total / 86400;
    long up_h     = (up_total % 86400) / 3600;
    long up_m     = (up_total % 3600) / 60;
    long up_s     = up_total % 60;
    char uptimebuf[32];
    if (up_days > 0)
        snprintf(uptimebuf, sizeof(uptimebuf), "up %ldd %02ld:%02ld:%02ld",
                 up_days, up_h, up_m, up_s);
    else
        snprintf(uptimebuf, sizeof(uptimebuf), "up %02ld:%02ld:%02ld",
                 up_h, up_m, up_s);

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        " TuringOS top  %s  %s    Tasks: %d    Mem: %lu M / %lu M free  ",
        timebuf, uptimebuf, g_nvalid, total_mb, free_mb);
    tb_print(0, 0, TB_BLACK, TB_CYAN, hdr);
    for (int x = (int)strlen(hdr); x < w; x++)
        tb_set_cell(x, 0, ' ', TB_BLACK, TB_CYAN);

    /* Row 1: separator */
    for (int x = 0; x < w; x++)
        tb_set_cell(x, 1, '-', TB_CYAN, TB_DEFAULT);

    /* Row 2: column headers */
    tb_printf(0, 2, TB_WHITE | TB_BOLD, TB_DEFAULT,
              " %5s  %-30s  %-8s  %6s", "HDL", "NAME", "STATE", "CPU%");

    /* Row 3: separator */
    for (int x = 0; x < w; x++)
        tb_set_cell(x, 3, '-', TB_CYAN, TB_DEFAULT);

    /* Rows 4+: task entries */
    int row = 4;
    for (int i = 0; i < g_nvalid && row < h - 2; i++, row++) {
        Top_entry &e = g_entries[i];

        double cpu_pct = 0.0;
        if (delta_us > 0 && e.cpu_us >= e.prev_us)
            cpu_pct = (double)(e.cpu_us - e.prev_us) / (double)delta_us * 100.0;
        if (cpu_pct > 100.0) cpu_pct = 100.0;

        const char  *state_str = (e.state == 1) ? "running" : "exited";
        uintattr_t   state_col = (e.state == 1) ? TB_GREEN  : TB_YELLOW;
        uintattr_t   cpu_col   = (cpu_pct > 50.0) ? (TB_RED | TB_BOLD) : TB_DEFAULT;

        tb_printf(0, row, TB_DEFAULT, TB_DEFAULT,
                  " %5u  %-30s", e.handle, e.name);
        tb_printf(38, row, state_col,  TB_DEFAULT, "%-8s", state_str);
        tb_printf(47, row, cpu_col, TB_DEFAULT, " %5.1f%%", cpu_pct);
    }

    if (g_nvalid == 0)
        tb_print(2, 4, TB_YELLOW, TB_DEFAULT,
                 "No user tasks running.  Use 'run <prog> &' to start one.");

    /* Last row: footer */
    const char *footer = " Press 'q' to quit";
    tb_print(0, h - 1, TB_BLACK, TB_WHITE, footer);
    for (int x = (int)strlen(footer); x < w; x++)
        tb_set_cell(x, h - 1, ' ', TB_BLACK, TB_WHITE);

    tb_present();
}

/* ------------------------------------------------------------------ */
/* cmd_top entry point                                                  */
/* ------------------------------------------------------------------ */
void cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;

    auto spawnd = L4Re::Env::env()->get_cap<Spawn_svr>("spawnd");
    if (!spawnd.is_valid()) {
        printf("top: spawnd not available\n");
        return;
    }

    memset(g_entries, 0, sizeof(g_entries));
    g_nvalid = 0;

    /* Seed the first prev_us snapshot */
    sample(spawnd);
    for (int i = 0; i < g_nvalid; i++)
        g_entries[i].prev_us = g_entries[i].cpu_us;

    tb_init();

    for (;;) {
        /* Poll for 'q' every TICK_MS, accumulating SAMPLE_MS total sleep */
        for (int slept = 0; slept < SAMPLE_MS; slept += TICK_MS) {
            l4_ipc_sleep(l4_timeout(L4_IPC_TIMEOUT_0,
                         l4_timeout_from_us((l4_uint32_t)TICK_MS * 1000u)));

            struct tb_event ev;
            if (tb_peek_event(&ev, 0) == TB_EVENT_KEY) {
                if (ev.ch == 'q' || ev.ch == 'Q' || ev.key == TB_KEY_CTRL_C)
                    goto done;
            }
            if (g_shell_interrupt) goto done;
        }

        sample(spawnd);
        render((l4_uint64_t)SAMPLE_MS * 1000ULL);
    }

done:
    tb_shutdown();
    g_shell_interrupt = 0;
}

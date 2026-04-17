#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <pthread.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <l4/devfs/devfs.h>
#include "commands.h"

extern void setup_devices();

static constexpr int MAX_ARGS = 64;
static constexpr int MAX_LINE = 4096;

volatile sig_atomic_t g_shell_interrupt = 0;
static volatile bool  g_cmd_running     = false;

/* ---- Keyboard ring buffer (replaces pipe — pipe() not in L4Re) ---- */
static char            g_kbd_buf[256];
static unsigned        g_kbd_head = 0; /* write index */
static unsigned        g_kbd_tail = 0; /* read  index */
static pthread_mutex_t g_kbd_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_kbd_cv   = PTHREAD_COND_INITIALIZER;

static void kbd_push(char c)
{
    pthread_mutex_lock(&g_kbd_mtx);
    unsigned next = (g_kbd_head + 1) % sizeof(g_kbd_buf);
    if (next != g_kbd_tail) {           /* drop if full */
        g_kbd_buf[g_kbd_head] = c;
        g_kbd_head = next;
        pthread_cond_signal(&g_kbd_cv);
    }
    pthread_mutex_unlock(&g_kbd_mtx);
}

static int kbd_pop()
{
    pthread_mutex_lock(&g_kbd_mtx);
    while (g_kbd_head == g_kbd_tail)
        pthread_cond_wait(&g_kbd_cv, &g_kbd_mtx);
    char c = g_kbd_buf[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1) % sizeof(g_kbd_buf);
    pthread_mutex_unlock(&g_kbd_mtx);
    return (unsigned char)c;
}

/* readline getc hook — reads from ring buffer instead of stdin */
static int rl_getc_buf(FILE *) { return kbd_pop(); }

/* ---- stdin monitor thread ---- */
static void *stdin_monitor(void *)
{
    char c;
    while (read(STDIN_FILENO, &c, 1) > 0) {
        if ((unsigned char)c == 3) {
            g_shell_interrupt = 1;
            if (!g_cmd_running)
                kbd_push(c); /* at readline prompt: let readline raise SIGINT */
        } else {
            kbd_push(c);
        }
    }
    return nullptr;
}

/* ---- SIGINT handler ---- */
static sigjmp_buf            g_shell_jmp;
static volatile sig_atomic_t g_shell_jmp_active = 0;

static void handle_sigint(int)
{
    g_shell_interrupt = 1;
    if (g_shell_jmp_active)
        siglongjmp(g_shell_jmp, 1);
}

/* ---- Parse line into argv ---- */
static int parse_line(char *line, char **argv, int max_args)
{
    static char buf[MAX_LINE];
    char *out = buf;
    char *p   = line;
    int   argc = 0;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        argv[argc++] = out;

        while (*p && *p != ' ' && *p != '\t') {
            if (*p == '\'') {
                for (++p; *p && *p != '\''; ) *out++ = *p++;
                if (*p) p++;
            } else if (*p == '"') {
                for (++p; *p && *p != '"'; ) {
                    if (*p == '\\' && *(p + 1)) p++;
                    *out++ = *p++;
                }
                if (*p) p++;
            } else if (*p == '\\' && *(p + 1)) {
                p++;
                *out++ = *p++;
            } else {
                *out++ = *p++;
            }
        }
        *out++ = '\0';
    }
    argv[argc] = nullptr;
    return argc;
}

static char *command_generator(const char *text, int state)
{
    static int idx, len;
    if (!state) { idx = 0; len = static_cast<int>(strlen(text)); }
    while (idx < num_commands) {
        const char *name = commands[idx++].name;
        if (strncmp(name, text, len) == 0)
            return strdup(name);
    }
    return nullptr;
}

static char **shell_completion(const char *text, int start, int end)
{
    (void)end;
    if (start == 0) {
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, command_generator);
    }
    return nullptr;
}

int main()
{
    /* Start stdin monitor thread (detached, runs for shell lifetime) */
    pthread_t mon;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&mon, &attr, stdin_monitor, nullptr);
    pthread_attr_destroy(&attr);

    int r = Devfs::init();
    if (r < 0)
        printf("devfs: mount failed (%d)\n", r);
    setup_devices();

    signal(SIGINT, handle_sigint);
    rl_catch_signals             = 1;
    rl_getc_function             = rl_getc_buf;
    rl_attempted_completion_function = shell_completion;

    printf("TuringOS Native Shell\n");
    printf("Type 'help' for available commands.\n\n");

    char  *line = nullptr;
    char  *argv[MAX_ARGS];
    int    argc;

    for (;;) {
        g_shell_interrupt  = 0;
        g_cmd_running      = false;
        g_shell_jmp_active = 1;

        if (sigsetjmp(g_shell_jmp, 1) != 0) {
            if (line) { free(line); line = nullptr; }
            rl_free_line_state();
            rl_cleanup_after_signal();
            putchar('\n');
            fflush(stdout);
            g_shell_interrupt  = 0;
            g_shell_jmp_active = 0;
            continue;
        }

        line = readline("turingos> ");
        g_shell_jmp_active = 0;

        if (!line) {
            printf("\nGoodbye.\n");
            break;
        }

        if (*line)
            add_history(line);

        argc = parse_line(line, argv, MAX_ARGS);
        if (argc == 0) {
            free(line); line = nullptr;
            continue;
        }

        g_shell_interrupt = 0;
        g_cmd_running     = true;

        bool found = false;
        for (int i = 0; i < num_commands; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].func(argc, argv);
                found = true;
                break;
            }
        }
        if (!found)
            printf("%s: command not found\n", argv[0]);

        g_cmd_running = false;

        if (g_shell_interrupt) {
            putchar('\n');
            fflush(stdout);
        }

        free(line);
        line = nullptr;
    }

    return 0;
}


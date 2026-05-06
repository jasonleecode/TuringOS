#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <l4/devfs/devfs.h>
#include <l4/re/env>
#include <l4/re/l4aux.h>
#include "commands.h"
#include "shell_exec.h"
#include "log.h"

l4re_aux_t const *l4re_aux = nullptr;

extern void setup_devices();

static constexpr int MAX_ARGS = 64;
static constexpr int MAX_LINE = 4096;

volatile sig_atomic_t g_shell_interrupt = 0;
static volatile bool  g_cmd_running     = false;
bool                  g_run_background  = false;

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

/* ---- Login ---- */
static constexpr const char *LOGIN_USER = "root";
static constexpr const char *LOGIN_PASS = "12345678";
static constexpr int MAX_LOGIN_ATTEMPTS = 3;

static void read_echoed(char *buf, int maxlen, bool echo)
{
    int i = 0;
    for (;;) {
        int c = kbd_pop();
        if (c == '\n' || c == '\r') {
            putchar('\n'); fflush(stdout);
            break;
        } else if ((c == '\b' || c == 127) && i > 0) {
            i--;
            if (echo) { printf("\b \b"); fflush(stdout); }
        } else if (i < maxlen - 1 && c >= 0x20) {
            buf[i++] = (char)c;
            if (echo) { putchar(c); fflush(stdout); }
        }
    }
    buf[i] = '\0';
}

static bool do_login()
{
    char user[64], pass[64];
    for (int attempt = 0; attempt < MAX_LOGIN_ATTEMPTS; attempt++) {
        printf("login: "); fflush(stdout);
        read_echoed(user, sizeof(user), true);
        printf("Password: "); fflush(stdout);
        read_echoed(pass, sizeof(pass), false);
        if (strcmp(user, LOGIN_USER) == 0 && strcmp(pass, LOGIN_PASS) == 0)
            return true;
        printf("Login incorrect\n\n");
    }
    return false;
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

int main(int argc, char const* const* argv)
{
    // Parse the AUX vector (follows envp in the startup stack) to get l4re_aux.
    auto auxp = &argv[argc] + 1;
    while (*auxp) ++auxp;   // skip past envp
    ++auxp;                  // skip past envp's NULL
    auto *sentinel = reinterpret_cast<char const*>(0xf0);
    while (*auxp) {
        if (*auxp == sentinel)
            l4re_aux = reinterpret_cast<l4re_aux_t const*>(auxp[1]);
        auxp += 2;
    }

    /* Start stdin monitor thread (detached, runs for shell lifetime) */
    pthread_t mon;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&mon, &attr, stdin_monitor, nullptr);
    pthread_attr_destroy(&attr);

    klog_init();
    klog_info(KLOG_KERN, "TuringOS native_shell starting");

    int r = Devfs::init();
    if (r < 0)
        klog_err(KLOG_KERN, "devfs: mount failed (%d)", r);
    else
        klog_info(KLOG_KERN, "devfs: mounted");
    setup_devices();

    net_auto_init();
    klog_info(KLOG_NET, "net: auto-init started");

    // Initialize shell-exec: set up programs library
    auto* env = L4Re::Env::env();
    auto programs_ns = env->get_cap<L4Re::Namespace>("programs");
    if (programs_ns.is_valid()) {
        File_resolver::set_programs_library(programs_ns);
        klog_info(KLOG_KERN, "shell-exec: programs library ready");
    } else {
        klog_warn(KLOG_KERN, "shell-exec: programs library not available");
    }

    signal(SIGINT, handle_sigint);
    rl_catch_signals             = 1;
    rl_getc_function             = rl_getc_buf;
    rl_attempted_completion_function = shell_completion;

    klog_info(KLOG_KERN, "shell ready");
    printf("\nTuringOS 1.0 (ttyS0)\n\n");
    for (;;) {
        if (do_login()) break;
        printf("Too many login failures. Try again.\n\n");
    }
    printf("\nWelcome, %s!\n", LOGIN_USER);
    printf("Type 'help' for available commands.\n\n");

    char  *line = nullptr;
    char  *cmd_argv[MAX_ARGS];
    int    cmd_argc;

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

        cmd_argc = parse_line(line, cmd_argv, MAX_ARGS);
        if (cmd_argc == 0) {
            free(line); line = nullptr;
            continue;
        }

        // Detect '&' (background) and '>' (redirection), strip both from argv
        const char *redir_path = nullptr;
        bool background = false;
        int new_argc = 0;
        for (int i = 0; i < cmd_argc; i++) {
            if (strcmp(cmd_argv[i], ">") == 0 && i + 1 < cmd_argc) {
                redir_path = cmd_argv[i + 1];
                i++;
            } else if (strcmp(cmd_argv[i], "&") == 0) {
                background = true;
            } else {
                cmd_argv[new_argc++] = cmd_argv[i];
            }
        }
        cmd_argv[new_argc] = nullptr;
        cmd_argc = new_argc;
        g_run_background = background;

        // Set up redirection: open file, save old stdout fd, dup2
        int saved_stdout = -1;
        int redir_fd     = -1;
        if (redir_path) {
            fflush(stdout);
            saved_stdout = dup(STDOUT_FILENO);
            redir_fd = open(redir_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (redir_fd < 0) {
                printf("shell: cannot open '%s' for writing\n", redir_path);
                redir_path = nullptr;
            } else {
                dup2(redir_fd, STDOUT_FILENO);
            }
        }

        g_shell_interrupt = 0;
        g_cmd_running     = true;

        bool found = false;
        for (int i = 0; i < num_commands; i++) {
            if (strcmp(cmd_argv[0], commands[i].name) == 0) {
                commands[i].func(cmd_argc, cmd_argv);
                found = true;
                break;
            }
        }
        if (!found) {
            klog_warn(KLOG_SHELL, "command not found: %s", cmd_argv[0]);
            printf("%s: command not found\n", cmd_argv[0]);
        }

        g_cmd_running = false;

        // Restore stdout after redirection
        if (redir_path && saved_stdout >= 0) {
            fflush(stdout);
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (redir_fd >= 0)
            close(redir_fd);

        if (g_shell_interrupt) {
            putchar('\n');
            fflush(stdout);
        }

        free(line);
        line = nullptr;
    }

    return 0;
}


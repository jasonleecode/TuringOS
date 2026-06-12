#pragma once

#include <csignal>
#include <l4/sys/types.h>

typedef void (*cmd_func_t)(int argc, char **argv);

struct shell_cmd {
    const char *name;
    const char *help;
    cmd_func_t  func;
};

extern shell_cmd commands[];
extern int       num_commands;

/* general */
void cmd_help(int argc, char **argv);
void cmd_echo(int argc, char **argv);
void cmd_info(int argc, char **argv);
void cmd_clear(int argc, char **argv);
void cmd_history(int argc, char **argv);
void cmd_exit(int argc, char **argv);

/* filesystem */
void cmd_pwd(int argc, char **argv);
void cmd_cd(int argc, char **argv);
void cmd_ls(int argc, char **argv);
void cmd_cat(int argc, char **argv);
void cmd_mkdir(int argc, char **argv);
void cmd_rm(int argc, char **argv);
void cmd_tmpfstest(int argc, char **argv);

/* system */
void cmd_uname(int argc, char **argv);
void cmd_env(int argc, char **argv);
void cmd_date(int argc, char **argv);
void cmd_uptime(int argc, char **argv);

/* program execution */
void cmd_run(int argc, char **argv);
void cmd_wait(int argc, char **argv);
void cmd_kill(int argc, char **argv);

/* hardware */
void cmd_temp(int argc, char **argv);
void cmd_radio(int argc, char **argv);

/* Networking is fully out of the shell: the stack lives in the netd server and
 * every net command is a standalone tool (pkg/net-cluster/tools) run via
 * `run rom/<tool>` (ifconfig, netcat, dhcp, ping, nslookup, udpecho, tcpecho,
 * mqtt).  native_shell links no lwIP and holds no sigma0. */

/* PATH-style auto-exec: run `argv[0]` as a program (rom/<cmd>) when it isn't a
 * builtin.  Returns true if a matching program was found/launched. */
bool shell_try_exec(int argc, char **argv);

/* system */
void cmd_list_tasks(int argc, char **argv);
void cmd_top(int argc, char **argv);
void cmd_dmesg(int argc, char **argv);

/* Background task registry */
void task_register(const char *name, const char *desc);      // internal threads
void task_unregister(const char *name);
void task_register_proc(l4_uint32_t handle, const char *name); // spawnd processes
void task_unregister_proc(l4_uint32_t handle);

/* Ctrl+C interrupt flag — set by main's SIGINT handler or stdin polling */
extern volatile sig_atomic_t g_shell_interrupt;

/* Set by main loop when trailing '&' is detected; consumed by cmd_run */
extern bool g_run_background;

int kbd_try_pop();  /* non-blocking key read; -1 if empty */
bool task_name_by_handle(l4_uint32_t handle, char *buf, int bufsz);

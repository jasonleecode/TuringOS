#pragma once

#include <csignal>

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

/* system */
void cmd_uname(int argc, char **argv);
void cmd_env(int argc, char **argv);
void cmd_date(int argc, char **argv);

/* program execution */
void cmd_run(int argc, char **argv);

/* hardware */
void cmd_temp(int argc, char **argv);
void cmd_radio(int argc, char **argv);

/* network */
void cmd_net(int argc, char **argv);
void cmd_ifconfig(int argc, char **argv);
void net_auto_init(); /* start network stack in background at boot */

/* system */
void cmd_list_tasks(int argc, char **argv);
void cmd_dmesg(int argc, char **argv);

/* Background task registry */
void task_register(const char *name, const char *desc);
void task_unregister(const char *name);

/* Ctrl+C interrupt flag — set by main's SIGINT handler or stdin polling */
extern volatile sig_atomic_t g_shell_interrupt;

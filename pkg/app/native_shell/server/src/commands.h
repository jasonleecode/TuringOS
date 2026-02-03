#ifndef COMMANDS_H
#define COMMANDS_H

typedef void (*cmd_func_t)(int argc, char **argv);

struct shell_cmd {
    const char *name;
    const char *help;
    cmd_func_t  func;
};

extern struct shell_cmd commands[];
extern int num_commands;

void cmd_help(int argc, char **argv);
void cmd_echo(int argc, char **argv);
void cmd_info(int argc, char **argv);
void cmd_clear(int argc, char **argv);
void cmd_history(int argc, char **argv);
void cmd_exit(int argc, char **argv);

#endif

#include <stdio.h>
#include <stdlib.h>
#include <readline/history.h>
#include "commands.h"

struct shell_cmd commands[] = {
    { "help",    "List available commands",           cmd_help    },
    { "echo",    "Echo arguments",                    cmd_echo    },
    { "info",    "Print system information",          cmd_info    },
    { "clear",   "Clear the screen",                  cmd_clear   },
    { "history", "Show command history",              cmd_history },
    { "exit",    "Exit the shell",                    cmd_exit    },
};

int num_commands = sizeof(commands) / sizeof(commands[0]);

void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Available commands:\n");
    for (int i = 0; i < num_commands; i++)
        printf("  %-10s %s\n", commands[i].name, commands[i].help);
}

void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
}

void cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("OS:           TuringOS\n");
#if defined(__aarch64__)
    printf("Architecture: aarch64\n");
#elif defined(__x86_64__)
    printf("Architecture: x86_64\n");
#elif defined(__arm__)
    printf("Architecture: arm\n");
#elif defined(__i386__)
    printf("Architecture: i386\n");
#else
    printf("Architecture: unknown\n");
#endif
    printf("Runtime:      L4Re\n");
}

void cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("\033[2J\033[H");
    fflush(stdout);
}

void cmd_history(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    HIST_ENTRY **list = history_list();
    if (!list) {
        printf("No history.\n");
        return;
    }
    for (int i = 0; list[i]; i++)
        printf("  %d  %s\n", i + 1, list[i]->line);
}

void cmd_exit(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Goodbye.\n");
    exit(0);
}

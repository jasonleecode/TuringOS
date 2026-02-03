#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "commands.h"

#define MAX_ARGS 64

static int parse_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < max_args - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

int main(void)
{
    char *line;
    char *argv[MAX_ARGS];
    int argc;

    printf("TuringOS Native Shell\n");
    printf("Type 'help' for available commands.\n\n");

    while ((line = readline("turingos> ")) != NULL) {
        if (*line)
            add_history(line);

        argc = parse_line(line, argv, MAX_ARGS);
        if (argc == 0) {
            free(line);
            continue;
        }

        int found = 0;
        for (int i = 0; i < num_commands; i++) {
            if (strcmp(argv[0], commands[i].name) == 0) {
                commands[i].func(argc, argv);
                found = 1;
                break;
            }
        }

        if (!found)
            printf("Unknown command: %s\n", argv[0]);

        free(line);
    }

    printf("\n");
    return 0;
}

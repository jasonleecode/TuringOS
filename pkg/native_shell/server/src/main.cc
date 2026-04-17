#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <readline/readline.h>
#include <readline/history.h>
#include <l4/devfs/devfs.h>
#include "commands.h"

extern void setup_devices();

static constexpr int MAX_ARGS = 64;
static constexpr int MAX_LINE = 4096;

/* Parse line into argv, handling single/double quotes and backslash escapes */
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

/* Tab completion: command names at start of line, filenames for arguments */
static char *command_generator(const char *text, int state)
{
    static int idx, len;
    if (!state) {
        idx = 0;
        len = static_cast<int>(strlen(text));
    }
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
    Devfs::init();
    setup_devices();

    rl_attempted_completion_function = shell_completion;

    printf("TuringOS Native Shell\n");
    printf("Type 'help' for available commands.\n\n");

    char  *line;
    char  *argv[MAX_ARGS];
    int    argc;

    while ((line = readline("turingos> ")) != nullptr) {
        if (*line)
            add_history(line);

        argc = parse_line(line, argv, MAX_ARGS);
        if (argc == 0) {
            free(line);
            continue;
        }

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

        free(line);
    }

    printf("\nGoodbye.\n");
    return 0;
}

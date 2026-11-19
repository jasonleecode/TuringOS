#include <cstdio>
#include <cstring>

#ifdef SHELL_LOCAL_TOOLS
#define main shell_tool_echo
#endif
int main(int argc, char **argv)
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
    return 0;
}

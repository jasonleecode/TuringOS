#include <cstdio>
#include <cstring>

#ifdef SHELL_LOCAL_TOOLS
#define main shell_tool_clear
#endif
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("\033[2J\033[H");
    fflush(stdout);
    return 0;
}

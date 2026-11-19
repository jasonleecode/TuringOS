#include <cstdio>
#include <cstring>

#ifdef SHELL_LOCAL_TOOLS
#define main shell_tool_uname
#endif
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("TuringOS  Fiasco/L4Re  ");
#if defined(__aarch64__)
    printf("aarch64");
#elif defined(__x86_64__)
    printf("x86_64");
#elif defined(__arm__)
    printf("armv7");
#elif defined(__i386__)
    printf("i386");
#else
    printf("unknown");
#endif
    printf("\n");
    return 0;
}

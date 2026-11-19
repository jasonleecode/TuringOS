#include <cstdio>
#include <cstring>

#ifdef SHELL_LOCAL_TOOLS
#define main shell_tool_info
#endif
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("OS:           TuringOS\n");
#if defined(__aarch64__)
    printf("Architecture: aarch64\n");
#elif defined(__x86_64__)
    printf("Architecture: x86_64\n");
#elif defined(__arm__)
    printf("Architecture: arm (armv7)\n");
#elif defined(__i386__)
    printf("Architecture: i386\n");
#else
    printf("Architecture: unknown\n");
#endif
    printf("Runtime:      L4Re / Fiasco microkernel\n");
    return 0;
}

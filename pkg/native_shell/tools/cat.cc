#include <cstdio>
#include <cstring>
#ifndef SHELL_LOCAL_TOOLS
#include <l4/devfs/devfs.h>
void setup_devices();
#endif

#ifdef SHELL_LOCAL_TOOLS
#define main shell_tool_cat
#endif
int main(int argc, char **argv)
{
#ifndef SHELL_LOCAL_TOOLS
    if (Devfs::init() == 0) setup_devices();
#endif
    int status = 0;
    for (int i = 1; i < argc || (argc == 1 && i == 1); ++i) {
        bool use_stdin = argc == 1 || !strcmp(argv[i], "-");
        FILE *f = use_stdin ? stdin : fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); status = 1; continue; }
        char buf[1024];
        size_t n;
        while ((n = fread(buf,1,sizeof(buf),f)) > 0)
            if (fwrite(buf,1,n,stdout) != n) { if (!use_stdin) fclose(f); return 1; }
        if (ferror(f)) status = 1;
        if (!use_stdin) fclose(f);
    }
    return status;
}

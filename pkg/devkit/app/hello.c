#include <stdio.h>

int main(int argc, char **argv)
{
    static const char message[] = "Hello from a TuringOS application";
    if (argc > 2) {
        fputs("usage: sdk-hello [output-file]\n", stderr);
        return 2;
    }
    if (argc == 1)
        return puts(message) == EOF || fflush(stdout) == EOF;

    FILE *output = fopen(argv[1], "w");
    if (!output) {
        perror(argv[1]);
        return 1;
    }
    int failed = fprintf(output, "%s\n", message) < 0;
    if (fclose(output) == EOF || failed) {
        perror(argv[1]);
        return 1;
    }
    return 0;
}

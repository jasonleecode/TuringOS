// WAMR's builtin libc supplies env.puts. This module needs no WASI sysroot.
__attribute__((import_module("env"), import_name("puts")))
extern int puts(const char *);

int main(int argc, char **argv)
{
    puts("Hello from a TuringOS WASM application");
    if (argc > 1) puts(argv[1]);
    return 0;
}

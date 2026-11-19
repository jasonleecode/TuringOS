#include "wasm_export.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Validate the runnable entry and its imports without executing a module start function.
static bool runnable(wasm_module_t module)
{
    for (int i = 0; i < wasm_runtime_get_import_count(module); ++i) {
        wasm_import_t import;
        wasm_runtime_get_import_type(module, i, &import);
        if (!import.linked) return false;
    }
    for (int i = 0; i < wasm_runtime_get_export_count(module); ++i) {
        wasm_export_t item;
        wasm_runtime_get_export_type(module, i, &item);
        if (item.kind != WASM_IMPORT_EXPORT_KIND_FUNC
            || (strcmp(item.name,"main") && strcmp(item.name,"_main")
                && strcmp(item.name,"__main_argc_argv"))) continue;
        wasm_func_type_t type = item.u.func_type;
        unsigned params = wasm_func_type_get_param_count(type);
        unsigned results = wasm_func_type_get_result_count(type);
        return (!params || (params == 2 && wasm_func_type_get_param_valkind(type,0) == WASM_I32
                            && wasm_func_type_get_param_valkind(type,1) == WASM_I32))
               && (!results || (results == 1 && wasm_func_type_get_result_valkind(type,0) == WASM_I32));
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: wasm-run FILE [args...]"); return 2; }
    bool check = argc == 3 && !strcmp(argv[1],"--check");
    FILE *f = fopen(argv[check ? 2 : 1], "rb");
    struct stat st;
    if (!f || fstat(fileno(f), &st) || st.st_size < 8 || st.st_size > 4 * 1024 * 1024) {
        fprintf(stderr, "wasm-run: invalid or unreadable module\n");
        if (f) fclose(f);
        return 1;
    }
    size_t len = st.st_size;
    uint8_t *bytes = malloc(len);
    int ok = bytes && fread(bytes, 1, len, f) == len;
    fclose(f);
    if (!ok || !wasm_runtime_init()) { free(bytes); return 1; }
    char error[128] = "";
    wasm_module_t module = wasm_runtime_load(bytes, len, error, sizeof(error));
    if (check) {
        bool valid = module && runnable(module);
        if (!valid) fprintf(stderr,"wasm-run: %s\n",module ? "invalid entry or unresolved import" : error);
        if (module) wasm_runtime_unload(module);
        wasm_runtime_destroy();
        free(bytes);
        return valid ? 0 : 1;
    }
    wasm_module_inst_t instance = module ? wasm_runtime_instantiate(module, 16384, 65536, error, sizeof(error)) : NULL;
    int status = 1;
    if (instance) {
        wasm_function_inst_t main_func = wasm_runtime_lookup_function(instance, "main");
        if (!main_func) main_func = wasm_runtime_lookup_function(instance, "__main_argc_argv");
        if (!main_func) main_func = wasm_runtime_lookup_function(instance, "_main");
        bool has_result = main_func && wasm_func_get_result_count(main_func, instance);
        if (wasm_application_execute_main(instance, argc - 1, argv + 1)) {
            status = 0;
            if (has_result) memcpy(&status, argv + 1, sizeof(status));
        } else fprintf(stderr,"wasm-run: %s\n", wasm_runtime_get_exception(instance));
        wasm_runtime_deinstantiate(instance);
    } else fprintf(stderr, "wasm-run: %s\n", error);
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    free(bytes);
    return status & 255;
}

/*
 * WAMR example for L4Re.
 *
 * Loads an embedded WASM module (no filesystem), instantiates it,
 * calls the exported "add" function, and prints the result.
 *
 * The WASM module implements:
 *   (func $add (export "add") (param i32 i32) (result i32)
 *     local.get 0  local.get 1  i32.add)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wasm_export.h"

/* ---- Embedded WASM binary ---- */

/*
 * WAT source:
 *   (module
 *     (func $add (export "add") (param i32 i32) (result i32)
 *       local.get 0  local.get 1  i32.add))
 *
 * Sections: type(1) function(3) export(7) code(10)
 */
static const uint8_t wasm_add[] = {
    /* magic + version */
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    /* type section: (i32 i32) -> i32 */
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    /* function section: func[0] uses type[0] */
    0x03, 0x02, 0x01, 0x00,
    /* export section: "add" -> func[0] */
    0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
    /* code section: local.get 0, local.get 1, i32.add, end */
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
};

#define WASM_STACK_SIZE (8  * 1024)
#define WASM_HEAP_SIZE  (64 * 1024)

int main(void)
{
    RuntimeInitArgs   args;
    wasm_module_t     mod  = NULL;
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t   env  = NULL;
    wasm_function_inst_t func;
    char error[128];
    uint32_t argv[2];
    int rc = 0;

    printf("wamr-example: starting\n");

    /* 1. Initialise runtime with system allocator */
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&args)) {
        printf("wamr-example: runtime init failed\n");
        return 1;
    }

    /* 2. Load the WASM module — wasm_runtime_load may modify the buffer
     *    in place, so copy the const byte array to a heap buffer first. */
    uint8_t *wasm_buf = (uint8_t *)malloc(sizeof(wasm_add));
    if (!wasm_buf) {
        printf("wamr-example: out of memory\n");
        wasm_runtime_destroy();
        return 1;
    }
    memcpy(wasm_buf, wasm_add, sizeof(wasm_add));

    mod = wasm_runtime_load(wasm_buf, sizeof(wasm_add), error, sizeof(error));
    if (!mod) {
        printf("wamr-example: load failed: %s\n", error);
        rc = 1;
        goto cleanup_runtime;
    }

    /* 3. Instantiate (allocates WASM stack and heap) */
    inst = wasm_runtime_instantiate(mod, WASM_STACK_SIZE, WASM_HEAP_SIZE,
                                    error, sizeof(error));
    if (!inst) {
        printf("wamr-example: instantiate failed: %s\n", error);
        rc = 1;
        goto cleanup_module;
    }

    /* 4. Look up the exported "add" function */
    func = wasm_runtime_lookup_function(inst, "add");
    if (!func) {
        printf("wamr-example: function 'add' not found\n");
        rc = 1;
        goto cleanup_inst;
    }

    /* 5. Create execution environment */
    env = wasm_runtime_create_exec_env(inst, WASM_STACK_SIZE);
    if (!env) {
        printf("wamr-example: exec env create failed\n");
        rc = 1;
        goto cleanup_inst;
    }

    /* 6. Call add(40, 2) */
    argv[0] = 40;
    argv[1] = 2;
    if (!wasm_runtime_call_wasm(env, func, 2, argv)) {
        printf("wamr-example: call failed: %s\n",
               wasm_runtime_get_exception(inst));
        rc = 1;
    } else {
        printf("wamr-example: add(40, 2) = %u\n", argv[0]);
    }

    wasm_runtime_destroy_exec_env(env);

cleanup_inst:
    wasm_runtime_deinstantiate(inst);
cleanup_module:
    wasm_runtime_unload(mod);
cleanup_runtime:
    wasm_runtime_destroy();
    free(wasm_buf);

    printf("wamr-example: %s\n", rc ? "FAILED" : "done");
    return rc;
}

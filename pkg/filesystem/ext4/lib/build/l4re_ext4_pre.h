/*
 * Pre-include shim for lwext4 on L4Re/uClibc.
 *
 * uClibc's internal uClibc_stdio.h uses __unused as a struct member name.
 * lwext4's ext4_misc.h (conditionally) redefines __unused as __attribute__((unused)).
 *
 * Fix: include <stdio.h> first (while __unused is still undefined), then undefine
 * __unused so ext4_misc.h's #ifndef guard can proceed normally.  Once stdio.h's
 * include-guards fire, the second pull-in by ext4 sources is a no-op.
 */
#include <stdio.h>
#undef __unused

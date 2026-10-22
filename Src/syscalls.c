/*
 * Minimal stubs for bare-metal operation.
 *
 * __libc_init_array: overrides the libc.a version (which is ARM-mode and
 * creates dangerous ARM/Thumb relocations on newer toolchains).  We have no
 * C++ static constructors that need runtime initialisation, so a no-op is
 * correct.
 *
 * _init: required by newlib's __libc_init_array hook; must be present even
 * when we override __libc_init_array to prevent linker errors if any
 * translation unit still pulls in the real one.
 */

void __libc_init_array(void) {}
void _init(void) {}

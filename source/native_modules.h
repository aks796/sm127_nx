/* native_modules.h -- GDNative / GDExtension libraries the engine dlopen()s.
 * See native_modules.c. MIT license; see LICENSE. */

#ifndef __NATIVE_MODULES_H__
#define __NATIVE_MODULES_H__

#include <stddef.h>

// Carve-out of the .so reserve that later modules are loaded into. main.c
// sets it before the engine starts.
void native_modules_set_region(void *base, size_t size);

// dlopen() of a library that exists as <data_root>/<basename>: load it with
// so_util and return an opaque handle, or NULL if the file is not there (the
// caller then applies its usual rules) or failed to load.
void *native_module_open(const char *path);

// 1 if `handle` came from native_module_open.
int native_module_is_handle(const void *handle);

// dlsym() on such a handle.
void *native_module_sym(void *handle, const char *symbol);

#endif

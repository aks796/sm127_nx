/* egl_shim.h -- dlopen/dlsym -> mesa EGL/GLES bridge. MIT license; see LICENSE. */

#ifndef __EGL_SHIM_H__
#define __EGL_SHIM_H__

void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
char *dlerror_fake(void);

// dlopen(NULL): "the main program and everything loaded into it".
#define MAIN_PROG_DL_HANDLE ((void *)0x4d41494e)  /* 'MAIN' */

#endif

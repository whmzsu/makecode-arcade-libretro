#ifndef MCA_WIN32_SHIM_DLFCN_H
#define MCA_WIN32_SHIM_DLFCN_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_NOW 2
#define RTLD_LOCAL 0

void *mca_dlopen(const char *path, int flags);
void *mca_dlsym(void *handle, const char *name);
char *mca_dlerror(void);
int mca_dlclose(void *handle);

#define dlopen  mca_dlopen
#define dlsym   mca_dlsym
#define dlerror mca_dlerror
#define dlclose mca_dlclose

#ifdef __cplusplus
}
#endif

#endif

#include "dlfcn.h"

#include <stdio.h>

static char mca_dl_error[512];

void *mca_dlopen(const char *path, int flags) {
    HMODULE h;
    (void)flags;
    h = LoadLibraryA(path);
    if (!h) {
        snprintf(mca_dl_error, sizeof(mca_dl_error), "LoadLibraryA failed: %lu",
                 (unsigned long)GetLastError());
        return NULL;
    }
    mca_dl_error[0] = 0;
    return (void *)h;
}

void *mca_dlsym(void *handle, const char *name) {
    FARPROC p = GetProcAddress((HMODULE)handle, name);
    if (!p) {
        snprintf(mca_dl_error, sizeof(mca_dl_error), "GetProcAddress(%s) failed: %lu", name,
                 (unsigned long)GetLastError());
        return NULL;
    }
    return (void *)p;
}

char *mca_dlerror(void) {
    return mca_dl_error;
}

int mca_dlclose(void *handle) {
    if (!FreeLibrary((HMODULE)handle)) {
        snprintf(mca_dl_error, sizeof(mca_dl_error), "FreeLibrary failed: %lu",
                 (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}

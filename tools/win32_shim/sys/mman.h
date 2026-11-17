#ifndef MCA_WIN32_SHIM_SYS_MMAN_H
#define MCA_WIN32_SHIM_SYS_MMAN_H

/* mca_host_android.c includes this unconditionally. Windows builds use
 * malloc/free for cart data; the Android-only mmap branch is not compiled. */

#endif

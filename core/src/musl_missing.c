/*
 * musl_missing.c — 补齐 zig 0.17 自带 musl 源码树缺失的内部符号
 *
 * 背景：zig 0.17 的 musl 源码树（zig/lib/libc/musl）缺了两个官方文件：
 *   * src/fcntl/dup3.c   —— 提供 dup3 / __dup3（musl 内部头 src/include/unistd.h
 *                           声明了 hidden __dup3，freopen.c、posix_spawn.c 引用它）
 *   * src/mman/mprotect.c —— 提供 mprotect / __mprotect（src/include/sys/mman.h
 *                           声明了 hidden __mprotect，pthread_create.c 引用它）
 * 用 zig cc -shared 编 musl 静态库产物时（全静态 .so，无 NEEDED）这两个符号
 * 解析不了，lld 直接报 "undefined hidden symbol"。本文件按官方 musl 源码补齐。
 *
 * 只补 aarch64（唯一 musl 目标）。系统调用号用 asm-generic 统一编号：
 *   SYS_dup3 = 24, SYS_mprotect = 226
 * 不依赖 musl 内部头（syscall.h 等是构建期生成的），直接内嵌 svc 汇编。
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>

#if !defined(__aarch64__)
#error "musl_missing.c 目前只支持 aarch64"
#endif

#ifndef SYS_dup3
#define SYS_dup3 24
#endif
#ifndef SYS_mprotect
#define SYS_mprotect 226
#endif

__attribute__((visibility("hidden")))
static long mcad_syscall3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc 0"
			  : "=r"(x0)
			  : "r"(x0), "r"(x1), "r"(x2), "r"(x8)
			  : "memory", "cc");
	return x0;
}

__attribute__((visibility("hidden")))
int __mprotect(void *addr, size_t len, int prot)
{
	return (int)mcad_syscall3(SYS_mprotect, (long)addr, (long)len, prot);
}

__attribute__((__weak__, __alias__("__mprotect"))) int mprotect(void *addr, size_t len, int prot);

__attribute__((visibility("hidden")))
int __dup3(int old, int newfd, int flags)
{
	long r;
	if (flags & ~O_CLOEXEC) {
		errno = EINVAL;
		return -1;
	}
	if (old == newfd)
		return fcntl(old, F_DUPFD_CLOEXEC, 0) < 0 ? -1 : old;
	/* musl 官方实现：内核忙（EBUSY）时重试 */
	do {
		r = mcad_syscall3(SYS_dup3, old, newfd, flags);
	} while (r == -EBUSY);
	if (r < 0) {
		errno = -r;
		return -1;
	}
	return (int)r;
}

__attribute__((__weak__, __alias__("__dup3"))) int dup3(int old, int newfd, int flags);

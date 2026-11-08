// nx_diag.cpp — NX（Switch）诊断实现：开机里程碑 + 运行期 trace
//
// 整个文件在非 Switch 平台上编成空 TU（零符号、零代码），这样它可以直接
// 列在构建清单里，不需要任何平台条件；条件只在下面的 #if 里出现一次。
//
// 两条日志各管一段，别混：
//   mca_boot.log —— 启动阶段（ctor / set_environment / init / load_game / 首帧）。
//                   stdio，写一行 flush 一行。进程崩在下一行之前，最后一行
//                   就是最后到达的位置。
//   mca_run.log  —— 运行期（每 30 帧一行，只在 -DMCA_NX_DIAG=1 的诊断构建里）。
//                   裸 POSIX IO，因为它的用处正是"堆已经被写坏之后还能写出去"，
//                   而 fprintf 扩容缓冲要 malloc。
#include "nx_diag.h"

#if defined(__SWITCH__)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <unistd.h>

namespace {

// ---- 开机里程碑 ---------------------------------------------------------
//
// Switch 上既拿不到前端日志（RetroArch 的 stdout 只在 nxlink 时可见），也没有
// gdb，真机报障往往只剩"一启动就报错"一句话。所以核心自己在最早的时机把里程碑
// 写进 sdmc:/switch/mca_boot.log，**写一行 flush 一行**。
//
// 只写十几次（启动 + 装载 + 首帧），不在每帧路径上，对性能没有影响。
FILE* g_nx_boot = nullptr;
int   g_nx_boot_tries = 0;

// ---- 运行期 trace（每 30 帧一行） ---------------------------------------
//
// mca_boot.log 只覆盖启动阶段。真机上"能启动、加载游戏后黑屏几秒才崩"这类
// 问题崩在运行期，需要一条**崩溃前还在写**的日志：
//   * 裸 POSIX IO，每次 open 再 close（0.5 秒一次，开销可忽略，且不长期持 fd）；
//   * 写日志先于任何堆探针 —— 探针崩掉时，文件最后一行就是崩溃帧号。
#if defined(MCA_NX_DIAG)
void nxRunLogV(const char* fmt, va_list ap) {
    static int lines = 0;
    if (lines > 4000) return;   // 约 33 分钟，防止日志把 SD 卡写满
    ++lines;
    char buf[256];
    int n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(buf)) - 2) n = static_cast<int>(sizeof(buf)) - 2;
    buf[n++] = '\n';
    int fd = ::open("sdmc:/switch/mca_run.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    ssize_t ignored = ::write(fd, buf, static_cast<size_t>(n));
    (void)ignored;
    ::close(fd);
}
#endif  // MCA_NX_DIAG

}  // namespace

// 定义在 namespace 外：调用方（libretro_core.cpp）用普通声明即可，
// 链接期仍是一个内部符号——它不构成对外接口。
void nxBoot(const char* fmt, ...) {
    if (!g_nx_boot) {
        if (g_nx_boot_tries >= 4) return;   // 反复失败就别每帧重试了
        // 一律用 "w"：只有上一次 fopen 真的失败（g_nx_boot 仍为 nullptr）才会
        // 走到这里，也就是说这一轮之前没有写过任何字节，截断不会冲掉本次内容。
        // 反过来用 "a" 有风险：上一轮会话留下的旧日志会被当成本次的进度。
        ++g_nx_boot_tries;
        g_nx_boot = std::fopen("sdmc:/switch/mca_boot.log", "w");
        if (!g_nx_boot) return;
        // 记一行"文件已开"，用来区分"ctor 跑过但当时文件系统还没挂上"
        // 与"ctor 根本没执行"——这两种情况都表现为缺 [boot] ctor 这一行。
        std::fputs("[boot] log opened\n", g_nx_boot);
        std::fflush(g_nx_boot);
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, g_nx_boot);
    std::fputc('\n', g_nx_boot);
    std::fflush(g_nx_boot);
}

namespace {
// 构造函数：只要本文件的目标文件被链进 NRO，这个函数就在 main 之前跑。
// 日志里有这一行 = 我们的代码真的被执行了，而不只是被链进来。
__attribute__((constructor)) void nxBootCtor(void) { nxBoot("[boot] ctor"); }
}  // namespace

#if defined(MCA_NX_DIAG)
void nxRunLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nxRunLogV(fmt, ap);
    va_end(ap);
}

// 支持 sdmc:/switch/mca_mode.txt 切通路：
//   0 = 正常；1 = 不推视频帧；2 = 不推音频；3 = 完全不拉 VM 数据（黑帧+静音）
// 一次装包能把"哪条通路把堆写坏"用四次运行切开。
int nxMode() {
    static int cached = -1;
    if (cached >= 0) return cached;
    char b[16] = {0};
    int fd = ::open("sdmc:/switch/mca_mode.txt", O_RDONLY);
    if (fd < 0) { cached = 0; return cached; }
    int n = static_cast<int>(::read(fd, b, sizeof(b) - 1));
    ::close(fd);
    cached = (n > 0) ? std::atoi(b) : 0;
    if (cached < 0 || cached > 3) cached = 0;
    return cached;
}

// C 链接包装：runtime 侧（vmload.cpp 等）用 weak 引用它，把宿主线程/栈信息
// 也写进同一条运行期日志。weak 意味着"没有 core 时不影响任何其他构建"。
extern "C" void mca_nx_runlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nxRunLogV(fmt, ap);
    va_end(ap);
}
#endif  // MCA_NX_DIAG

#endif  // __SWITCH__

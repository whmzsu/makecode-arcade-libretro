#ifndef __PXT_PLATFORM_H
#define __PXT_PLATFORM_H

#define PAGE_SIZE 1024 // not really

#define DEV_NUM_PINS 28

#define DEV_PWM_PINS 0xffffffffULL
#define DEV_AIN_PINS 0ULL

// Codal doesn't yet distinguish between PWM and AIN
#define DEV_ANALOG_PINS (DEV_PWM_PINS | DEV_AIN_PINS)

#define CODAL_PIN ZPin
#define CODAL_TIMER ZTimer
#define CODAL_SPI ZSPI
#define CODAL_I2C ZI2C

const char *vm_settings_dir(void);
#define SETTINGSDIR vm_settings_dir()

namespace pxt {

class ZPin;
class AbstractButton;
class MultiButton;
class CodalComponent;

typedef void (*reset_fn_t)();
void registerResetFunction(reset_fn_t fn);
void soft_panic(int errorCode);
} // namespace pxt

#define IMAGE_BITS 4

#define PXT_IN_ISR() false

#define GC_BLOCK_SIZE (1024 * 64)

#define PXT_REGISTER_RESET(fn) pxt::registerResetFunction(fn)

// MCA PATCH 6：重启路径 trace（定义在 core---vm/scheduler.cpp 的 namespace pxt）。
// 必须声明在 namespace pxt 里：pxtbase.h 有 `using namespace pxt`，各 ext 源文件
// 才能直接写 mcaTrace(...)；若声明在全局作用域会变成另一个实体、链接不上。
#ifdef __cplusplus
namespace pxt {
void mcaTrace(const char *fmt, ...);
}
#endif

// MCA PATCH 6：重启路径 trace（存档目录下的 mca_trace.log）。
// 定义在 core---vm/scheduler.cpp，C 链接，core 侧（vm_host.cpp）也用它。
#ifdef __cplusplus
extern "C" {
#endif
const char *pxt_vm_get_data_directory(void);
void pxt_vm_trace(const char *msg);
const char *pxt_vm_trace_path(void);

// MCA PATCH 7：VM「在线」标志 + 输出通道关机钩子。
//
// 重启期间前端线程仍然每帧调 pxt_screen_get_pixels / pxt_get_audio_samples，
// 而此刻 VM 的堆和单例（instWDisplay / dac / WSynthesizer）已经被释放 ——
// 不挡住就是 use-after-free；更糟的是 pxt_screen_get_pixels 里的 target_panic
// 在 VM 版会走 systemReset()，在**前端线程**上 pthread_exit，整个前端就假死。
// 所以：VM 线程清理的第一步把 online 置 0 并把这些静态指针丢掉，
// 新一局镜像装载成功后再置回 1。
int pxt_vm_online(void);
void pxt_vm_set_online(int online);
void pxt_screen_shutdown(void);  // screen.cpp：丢 instWDisplay / numGetPixels
void pxt_screen_publish(unsigned long long seq); // screen.cpp：发布一帧（seq = 帧序号）
void pxt_audio_shutdown(void);   // sound.cpp：丢 dac
void pxt_music_shutdown(void);   // melody.cpp：丢 instWSynthesizer（见 PATCH 9）
// vmload.cpp：当前线程是不是 VM 线程（panic 要不要当场清理的判断，见 PATCH 11）
int pxt_vm_is_vm_thread(void);
int pxt_vm_running(void);        // VM 线程是否还活着（vmload.cpp，DLLEXPORT）
void pxt_vm_request_reset(void); // 置重启标志，VM 线程在主循环顶收尾（vmload.cpp）
#ifdef __cplusplus
}
#endif

#ifdef __APPLE__
#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#define PXT_IOS 1
#endif
#endif

// MCA 3DS/Vita (probe/pxt32-3ds-vita)：GC 堆基址运行时变量，定义于 scheduler.cpp。
// 所有平台都有此符号（PXT64 下不使用），故声明放在分支外，保证 Win32 下走
// PXT32 旁路表的 gc.cpp 也能看到。
extern uint8_t *mca_gc_base;

#if defined(PXT64) || defined(__MINGW32__)
#define GC_BASE 0x2000000000
#define GC_PAGE_SIZE (64 * 1024)

// always allocate 1M of heap
#define PXT_VM_HEAP_ALLOC_BITS 20
extern uint8_t *gcBase;
#define PXT_IS_READONLY(v)                                                                         \
    (!isPointer(v) || (((uintptr_t)v - (uintptr_t)gcBase) >> PXT_VM_HEAP_ALLOC_BITS) != 0)

#else // PXT32
#define GC_BASE 0x20000000
#define GC_PAGE_SIZE 4096

// MCA P1 (probe/pxt32): 32 位通用宿主（Linux/Android）的核心是 ET_DYN DSO，
// vtable 与 VM 镜像都落在高地址。旧的 ARM 假设「高 4 位非零 = 可写 RAM」
// 会把它们误判成堆内对象，GC 扫描时改写 vtable 标记位、堆结构随即损坏
// （实测 sweep() 里 free block 的 sz 变成 1.3 亿）。PXT64 早已改用基于
// gcBase 的精确堆区间，这里补上等价的动态堆顶 mca_gc_top。
extern uint8_t *mca_gc_top;
// MCA 3DS/Vita (probe/pxt32-3ds-vita)：GC 堆基址从编译期常量改为运行时变量。
// 默认值仍是 GC_BASE（见 scheduler.cpp 定义），故 Linux/Android/Win32 的既有
// 行为逐位不变。静态链接平台（无 MAP_FIXED mmap，0x20000000 可能被占）可在
// VM 启动前用 mca_vm_set_gc_heap() 把基址改到本平台合法堆地址。
#define PXT_IS_READONLY(v)                                                                         \
    (!isPointer(v) || (uintptr_t)v < (uintptr_t)mca_gc_base ||                                    \
     (uintptr_t)v >= (uintptr_t)mca_gc_top)

#endif // PXT32 vs 64

#endif

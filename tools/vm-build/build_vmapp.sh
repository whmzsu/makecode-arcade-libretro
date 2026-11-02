#!/usr/bin/env bash
# build_vmapp.sh — 把 pxt 生成的 VM 原生 app 源码树编译成可链接的静态库/测试宿主
#
# 背景：官方构建走 docker（pext/crossbuild 镜像）。我们用 zig 本地编译，
#      同时补齐两个 codegen / 64 位移植问题（见 PATCH 注释）。
#
# 用法：
#   ./build_vmapp.sh [--rebuild-host]
#
# 前置：已用 pxt CLI 生成 pxtapp 源码树：
#   cd node_modules/pxt-arcade/testproj
#   PATH=<本目录>/../fakebin:$PATH PXT_FORCE_LOCAL=1 \
#     node ../../pxt-core/built/pxt.js build --hwvariant vm
#   （fakebin/docker.cmd 是空壳，用于跳过 docker 原生编译；我们只要编译器产出的
#     built/binary.pxt64 与 built/dockercross/pxtapp 源码树）

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ZIG="${ZIG:-$ROOT/../common tools/zig/zig.exe}"
APPDIR="${APPDIR:-$HERE/node_modules/pxt-arcade/testproj/built/dockercross}"
PXTAPP="$APPDIR/pxtapp"
OBJ="$APPDIR/objs"

CXXFLAGS=(
  -std=c++11
  -fno-rtti -fno-exceptions -fno-unwind-tables
  -fwrapv -fno-threadsafe-statics -fno-strict-aliasing
  -ffunction-sections -O2 -g
  -I"$PXTAPP"
  -Wno-unused-parameter -Wno-unused-private-field -Wno-vla-cxx-extension
)

echo "== 1. 修补生成的源码（pxt codegen 问题）=="
python - "$PXTAPP" <<'PY'
import re, sys, pathlib
app = pathlib.Path(sys.argv[1])

# PATCH 1: core---vm/vm.cpp find_src_map() 在 64 位下用 uint32_t 截断指针
p = app / 'core---vm' / 'vm.cpp'
s = p.read_text(encoding='utf8')
old = "const uint32_t *p = (const uint32_t *)((uint32_t)vmImg->dataEnd & ~0xf);"
new = "const uint32_t *p = (const uint32_t *)((uintptr_t)vmImg->dataEnd & ~(uintptr_t)0xf);"
if old in s:
    p.write_text(s.replace(old, new), encoding='utf8')
    print("  patched vm.cpp find_src_map()")

# PATCH 2: pointers.cpp 里 `aspxt::RefCollection(` 应为 `pxt::asRefCollection(`
#          （pxt 13.2.x 对带命名空间的参数类型拼 as 转换函数时多加了命名空间前缀）
p = app / 'pointers.cpp'
s = p.read_text(encoding='utf8')
s2, n = re.subn(r'\bas([A-Za-z_]\w*)::([A-Za-z_]\w*)\(', r'\1::as\2(', s)
if n:
    p.write_text(s2, encoding='utf8')
    print(f"  patched pointers.cpp: {n} bad as-casts")

# PATCH 3: VM 线程栈大小。pthread_create(attr=NULL) 时线程栈取**宿主可执行文件**
#          的 PE 栈预留；libretro 前端（RetroArch.exe 仅 2MB）会让 VM 在
#          initRuntime 期间栈溢出。显式给 8MB，与前端无关。
p = app / 'core---vm' / 'vmload.cpp'
s = p.read_text(encoding='utf8')
if 'MCA_VM_STACK_SIZE' not in s:
    s = s.replace('#include "pxt.h"',
                  '#include "pxt.h"\n\n'
                  '// MCA PATCH 3：VM 线程栈大小（见 spinThread 里的说明）\n'
                  '#ifndef MCA_VM_STACK_SIZE\n'
                  '#define MCA_VM_STACK_SIZE (8 * 1024 * 1024)\n'
                  '#endif\n', 1)
    s = s.replace('    pthread_create(&vm_thread, NULL, multiStart, NULL);',
                  '    // MCA PATCH 3（本项目）：显式指定 VM 线程栈大小。\n'
                  '    // pthread_create 的 attr 为 NULL 时，线程栈取的是**宿主可执行文件**的\n'
                  '    // PE 栈预留：官方 sdlmain.exe 大而无碍，但 libretro 前端（RetroArch.exe\n'
                  '    // 只有 2MB）会让 VM 在 initRuntime 阶段栈溢出 —— Windows 上表现为进程\n'
                  '    // 无声消失、无任何日志。固定 8MB 与前端无关。\n'
                  '    pthread_attr_t vm_attr;\n'
                  '    pthread_attr_init(&vm_attr);\n'
                  '    pthread_attr_setstacksize(&vm_attr, MCA_VM_STACK_SIZE);\n'
                  '    pthread_create(&vm_thread, &vm_attr, multiStart, NULL);\n'
                  '    pthread_attr_destroy(&vm_attr);')
    p.write_text(s, encoding='utf8')
    print("  patched vmload.cpp: VM thread stack size")

# PATCH 4: initRandomSeed 对 fd=-1 调 read()。Windows 没有 /dev/urandom，
#          open() 必然失败，UCRT 对无效 fd 的 read() 会 _invoke_watson →
#          __fastfail(0xC0000409)：进程立刻消失，无日志无 WER（libretro
#          前端表现为"RetroArch 无声崩溃"）。
p = app / 'core---vm' / 'platform.cpp'
s = p.read_text(encoding='utf8')
if 'MCA PATCH 4' not in s:
    s = s.replace('#include <fcntl.h>', '#include <fcntl.h>\n\n#ifdef _WIN32\n#include <windows.h>\n#endif')
    s = s.replace('''static void initRandomSeed() {
    int seed = 0xC0DA1;
    int fd = open("/dev/urandom", O_RDONLY);
    read(fd, &seed, sizeof(seed));
    close(fd);
    seedRandom(seed);
}''', '''static void initRandomSeed() {
    int seed = 0xC0DA1;
#ifdef _WIN32
    // MCA PATCH 4（本项目）：Windows 没有 /dev/urandom。原实现在 open() 返回
    // -1 之后仍然对 fd=-1 调 read()，UCRT 会判定"无效参数"并走
    // _invoke_watson → __fastfail(0xC0000409)：进程立刻消失，不产生任何
    // 日志、栈回溯或 WER 记录（libretro 前端里表现为"RetroArch 无声崩溃"）。
    seed ^= (int)GetTickCount64();
    seed ^= (int)(uintptr_t)&seed;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
#endif
    seedRandom(seed);
}''')
    p.write_text(s, encoding='utf8')
    print("  patched platform.cpp: safe random seed on Windows")

# PATCH 5: 宿主可请求"重开这一局"（libretro 的 reset 键 / Restart）。
#   官方 ABI 是 single-shot，但 systemReset() 本身就是完整重启收尾（清
#   fiber/handler/GC）。两个问题要修：
#   a) 它最后 pthread_exit —— Windows/mingw 下 VM 线程的 fiber 栈已错位，
#      pthread_exit 直接 0xC0000005（实测）。改为清理后让线程正常 return。
#   b) 64 位下 GC 堆固定 1MB 且分配游标不回绕，重开几次就 soft_panic(20)。
#   新增宿主接口：pxt_vm_request_reset / pxt_vm_poll_exit / pxt_vm_running
sched = app / 'core---vm' / 'scheduler.cpp'
s = sched.read_text(encoding='utf8')
if 'MCA PATCH 5' not in s:
    # 5a) 标志（放在 namespace pxt 开头）
    s = s.replace('''uint8_t *gcBase;

namespace pxt {

#ifndef PXT_ESP32
static uint64_t startTime;
#endif''', '''uint8_t *gcBase;

namespace pxt {

// MCA PATCH 5（本项目）：宿主请求重启（libretro 的 reset 键）。
//
// 官方宿主是"单游戏单进程"，VM 没有 stop 接口；但 systemReset() 本身就是
// 一套完整的重启收尾（清 fiber / handler 绑定 / GC，然后 pthread_exit）。
// 它只能在 VM 线程里跑，所以宿主线程只置标志，由 mainRunLoop 自行退出去
// 交给 initRuntime 末尾的 systemReset()。
//   mcaResetRequest: 宿主线程写、VM 线程读（单字节标志，volatile 足够）
//   mcaVmExited    : VM 线程退出前置位、宿主线程读后清零
volatile int mcaResetRequest = 0;
volatile int mcaVmExited = 0;
static volatile int mcaRestartDone = 0;   // 清理已做完，initRuntime 跳过 systemReset()

#ifndef PXT_ESP32
static uint64_t startTime;
#endif''')

    # 5b) mainRunLoop：响应宿主重启请求
    s = s.replace('''static void mainRunLoop() {''',
'''static void hostRestartCleanup();

static void mainRunLoop() {''', 1)
    s = s.replace('''static void mainRunLoop() {
    FiberContext *f = NULL;
    for (;;) {
        if (panicCode)
            return;''', '''static void mainRunLoop() {
    FiberContext *f = NULL;
    for (;;) {
        // MCA PATCH 5：宿主请求重启 → 做清理后退出主循环；
        // initRuntime 看到 mcaRestartDone 就不再 systemReset()，线程正常结束
        if (mcaResetRequest) {
            mcaResetRequest = 0;
            hostRestartCleanup();
            return;
        }
        if (panicCode)
            return;''')

    # 5c) initRuntime 尾部：宿主重启时不走 pthread_exit
    s = s.replace('''    mainRunLoop();
    systemReset();''', '''    mainRunLoop();
    if (mcaRestartDone) {
        // 宿主重启：清理已在 hostRestartCleanup() 里完成，
        // 这里正常 return，让 VM 线程干净退出（宿主的 pthread_join 才安全）
        mcaRestartDone = 0;
        return;
    }
    systemReset();''')

    # 5d) GC 分配游标提为文件级（重启时回绕）
    s = s.replace('''void *gcAllocBlock(size_t sz) {
#ifdef PXT_ESP32
    void *r = xmalloc(sz);
#else
    static uint8_t *currPtr = (uint8_t *)GC_BASE;
    sz = (sz + GC_PAGE_SIZE - 1) & ~(GC_PAGE_SIZE - 1);''', '''// MCA PATCH 5：GC 分配游标从函数内 static 提到文件级，好让重启时绕回堆首。
// 64 位下 VM 堆固定 1MB（PXT_VM_HEAP_ALLOC_BITS=20），不回绕重开几次就
// soft_panic(20)。
static uint8_t *gcAllocPtr = (uint8_t *)GC_BASE;

void *gcAllocBlock(size_t sz) {
#ifdef PXT_ESP32
    void *r = xmalloc(sz);
#else
    uint8_t *&currPtr = gcAllocPtr;
    sz = (sz + GC_PAGE_SIZE - 1) & ~(GC_PAGE_SIZE - 1);''')

    # 5e) systemReset：抽出清理序列，新增宿主重启收尾
    i = s.index('void systemReset() {')
    j = s.index('\n}\n', i) + 3
    s = s[:i] + '''// MCA PATCH 5（本项目）：VM 的"回到初始状态"收尾序列。
// 官方 systemReset() 把这段内联在里面，宿主侧重启要复用它但**不能** pthread_exit
//（Windows/mingw 下从 VM 线程 pthread_exit 会在 fiber 栈已错位时崩，
// 实测 0xC0000005；改成让线程正常 return 即可安全 join），所以抽出来共用。
static void resetRuntimeState() {
    gcFreeze();

    for (int i = 0; i < MAX_RESET_FN; ++i) {
        auto fn = resetFunctions[i];
        if (fn)
            fn();
    }

    coreReset(); // clears handler bindings

    currentFiber = NULL;
    while (allFibers) {
        disposeFiber(allFibers);
    }

    // this will consume all events, but won't dispatch anything, since all listener maps are empty
    wakeFibers();

    // mark all GC memory as free
    gcReset();

    // GC 分配游标绕回堆首 —— 与第一次启动时的状态一致
#if defined(PXT64) || defined(__MINGW32__)
    if (gcBase)
        gcAllocPtr = gcBase;
#else
    gcAllocPtr = (uint8_t *)GC_BASE;
#endif
    // 清空 reset 回调槽：registerResetFunction() 在槽满时会 target_panic，
    // 而 systemReset 只调回调不清表，重启时重复注册就会 panic
    for (int i = 0; i < MAX_RESET_FN; ++i)
        resetFunctions[i] = NULL;
}

// 宿主说"重开这一局"（libretro 的 reset 键 / Restart）
static void hostRestartCleanup() {
    dmesg("HOST RESET");
    if (!panicCode)
        panicCode = -1;
    resetRuntimeState();
    mcaVmExited = 1;   // 告知宿主：线程即将正常返回，可以 join
    mcaRestartDone = 1;
}

void systemReset() {
#ifdef PXT_ESP32
    esp_restart();
#else
    if (!panicCode)
        panicCode = -1;

    dmesg("TARGET RESET");

    resetRuntimeState();

    pthread_exit(NULL);
#endif
}
''' + s[j:]
    sched.write_text(s, encoding='utf8')
    print("  patched scheduler.cpp: host-requested restart")

# PATCH 5 (cont): vmload.cpp 导出宿主接口
p = app / 'core---vm' / 'vmload.cpp'
s = p.read_text(encoding='utf8')
if 'pxt_vm_request_reset' not in s:
    s = s.replace('''namespace pxt {

VMImage *vmImg;''', '''namespace pxt {

// MCA PATCH 5：定义在 scheduler.cpp（VM 主循环与 systemReset 都在那边）
extern volatile int mcaResetRequest;
extern volatile int mcaVmExited;

VMImage *vmImg;''')
    s = s.replace('''DLLEXPORT void pxt_vm_start_buffer(uint8_t *data, unsigned len) {
    vm_filename = NULL;
    vm_data = data;
    vm_len = len;
    spinThread();
}''', '''DLLEXPORT void pxt_vm_start_buffer(uint8_t *data, unsigned len) {
    vm_filename = NULL;
    vm_data = data;
    vm_len = len;
    spinThread();
}

// MCA PATCH 5（本项目）：宿主侧的"重启游戏"接口。
//
// 用法（宿主必须严格按顺序）：
//   1) pxt_vm_request_reset()          置标志，VM 线程在下一轮主循环退出
//   2) 轮询 pxt_vm_poll_exit()         返回 1 表示 VM 线程已退出（join 安全）
//   3) 用**未打补丁的原始镜像**重新 pxt_vm_start_buffer(data, len)
// 镜像会被 VM 就地打补丁（vm_patch_image），所以第 3 步必须换一份干净的副本，
// 不能复用第一次传进来的缓冲区。
DLLEXPORT void pxt_vm_request_reset() {
    mcaResetRequest = 1;
}

DLLEXPORT int pxt_vm_poll_exit() {
    if (!mcaVmExited)
        return 0;
    mcaVmExited = 0;
    return 1;
}

DLLEXPORT int pxt_vm_running() {
    return vm_has_thread && !mcaVmExited;
}''')
    p.write_text(s, encoding='utf8')
    print("  patched vmload.cpp: host restart API")
PY

echo "== 2. 编译 VM 内核 + 引擎（排除 SDL 宿主 sdlmain.cpp 与 main.cpp）=="
rm -rf "$OBJ" && mkdir -p "$OBJ"
count=0
for f in "$PXTAPP"/base/*.cpp "$PXTAPP"/core---vm/*.cpp "$PXTAPP"/game/*.cpp \
         "$PXTAPP"/mixer---ext/*.cpp "$PXTAPP"/screen---ext/*.cpp \
         "$PXTAPP"/settings---files/*.cpp "$PXTAPP"/pointers.cpp; do
  rel="${f#$PXTAPP/}"                       # e.g. core---vm/vm.cpp
  out="$OBJ/${rel//\//_}.o"                 # e.g. core---vm_vm.o （避免同名覆盖）
  "$ZIG" c++ "${CXXFLAGS[@]}" -c "$f" -o "$out"
  count=$((count + 1))
done
echo "  compiled $count objects -> $OBJ"

echo "== 3. 链接冒烟宿主 =="
"$ZIG" c++ -std=c++11 -O2 -I"$PXTAPP" "$ROOT/core/tests/vm_smoke.cpp" "$OBJ"/*.o \
  -o "$APPDIR/vm_smoke.exe" -lpthread
echo "  -> $APPDIR/vm_smoke.exe"

# MakeCode Arcade libretro core
#
# 标准 libretro Makefile 形态：官方 buildbot 用
#     make -f <MAKEFILE_PATH>/Makefile platform=<platform>
# 构建（CC/CXX/CFLAGS/CXXFLAGS/LDFLAGS 由 CI 环境注入，本文件只做追加）。
#
# 本地直接 `make` 即可（自动探测当前平台）；交叉编译矩阵仍可用
# tools/build_target.sh（zig 驱动的独立对象目录，产物更细），两者源码集合一致。
#
# 支持的 platform：
#   unix / unix-*       Linux 或其它 POSIX（.so）——含 linux-x64 / aarch64 / armhf
#   win / win64         MinGW（.dll）
#   osx / osx-arm64     macOS（.dylib）
#   android             需配合 NDK 工具链（CI 走 jni/，见 core/jni/Android.mk）
#   libnx               Nintendo Switch 裸机（devkitA64 + libnx），产**静态库**
#                       ——Switch 没有 dlopen，core 是 .a 链进 RetroArch NRO
#
# 用法示例：
#   make                                   本机构建
#   make platform=unix CC=x86_64-linux-gnu-gcc CXX=x86_64-linux-gnu-g++
#   make platform=win64 CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++
#   make clean                             清理对象与产物

TARGET_NAME := makecode_arcade_libretro

# ---------------------------------------------------------------- 平台探测
ifeq ($(platform),)
   platform = unix
   ifeq ($(shell uname -s),)
      platform = win
   else ifneq (,$(findstring MINGW,$(shell uname -s)))
      platform = win64
   else ifneq (,$(findstring MSYS,$(shell uname -s)))
      platform = win64
   else ifneq (,$(findstring Darwin,$(shell uname -s)))
      platform = osx
   endif
else ifneq (,$(findstring armv,$(platform)))
   override platform += unix
else ifneq (,$(findstring rpi,$(platform)))
   override platform += unix
endif

# ---------------------------------------------------------------- 目录布局
# 以本 Makefile 所在目录为基准，因此 `make -f core/Makefile`（buildbot 的做法）
# 在仓库根或任意 CWD 下都成立。
CORE_DIR   := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
PXTAPP_DIR := $(CORE_DIR)/../runtime/pxtapp

# 可选 sanitizer（CI 的契约自检构型）：make platform=unix SANITIZE=undefined
# 必须放在 BUILD_TAG 之前——打开后对象与产物落独立目录，不覆盖普通构建。
# 目录名带上 sanitizer 名：address 与 undefined 的 .o **不通用**，共用一个
# -san 目录会链到上一次构型的旧对象（症状是 `undefined symbol:
# __ubsan_handle_*` 或者干脆没插桩，静默给出假的"干净"结果）。
ifneq ($(SANITIZE),)
   SANFLAGS  := -fsanitize=$(SANITIZE) -fno-sanitize-recover=$(SANITIZE) -g
   SAN_TAG   := -san-$(SANITIZE)
endif

# 覆盖率构型：make platform=unix COVERAGE=1 && ./tools/coverage.sh
# -O0 是为了行/分支计数准（-O2 会把小函数内联掉，报告里那一行永远不出现）；
# 产物落在对象目录里，不覆盖 core/ 根下的正式 .so。
ifneq ($(COVERAGE),)
   COVFLAGS := --coverage -O0 -g
   COV_TAG  := -cov
endif

# 产物与对象：对象按平台分目录，避免多目标构建互相覆盖（build/ 已在 .gitignore）
# platform 可能带后缀（如 unix-armv7-hardfloat-neon），空格/斜杠会破坏路径，
# 统一压成 - 分隔的目录名
empty      :=
space      := $(empty) $(empty)
BUILD_TAG  := $(subst $(space),-,$(strip $(platform)))$(SAN_TAG)$(COV_TAG)
BUILD_DIR  := $(CORE_DIR)/build/make/$(BUILD_TAG)
OBJ_DIR    := $(BUILD_DIR)/obj

# ---------------------------------------------------------------- 编译开关
# 与 tools/build_target.sh 保持一致：
#   -fno-rtti/-fno-exceptions：pxt VM 不用异常/RTTI，去掉可显著减小体积
#   -fwrapv：VM 依赖有符号整数回绕语义
#   -fno-threadsafe-statics：pxt 的 SINGLETON 是裸 static，防引入 __cxa_guard
COMMON_FLAGS := -O2 -fPIC
COMMON_FLAGS += -fwrapv -fno-strict-aliasing -ffunction-sections

CXXFLAGS  += $(COMMON_FLAGS) -fno-rtti -fno-exceptions -fno-unwind-tables
CXXFLAGS  += -fno-threadsafe-statics
CXXFLAGS  += -Wno-unused-parameter -Wno-unused-private-field -Wno-vla-cxx-extension
CFLAGS    += $(COMMON_FLAGS) -std=c99
CXXFLAGS  += $(SANFLAGS)
CFLAGS    += $(SANFLAGS)
LDFLAGS   += $(SANFLAGS)
CXXFLAGS  += $(COVFLAGS)
CFLAGS    += $(COVFLAGS)
LDFLAGS   += $(COVFLAGS)

INCFLAGS := -I$(CORE_DIR)/src -I$(PXTAPP_DIR)

# ---------------------------------------------------------------- 目标形态
ifneq (,$(findstring libnx,$(platform)))
   # Nintendo Switch：裸机 aarch64（newlib + libnx），没有动态链接。
   # 产物是静态库，由 RetroArch 的 Makefile.libnx 连同自身一起链进
   # retroarch_switch.nro（官方把 core 改名 libretro_libnx.a 放源码根目录）。
   # 编译参数与 RetroArch 的 ARCH 保持一致，否则对象与前端 ABI/指令集不匹配。
   STATIC_LINKING := 1
   TARGET  := $(CORE_DIR)/$(TARGET_NAME)_libnx.a
   ifeq ($(DEVKITPRO),)
      $(error platform=libnx 需要 DEVKITPRO：devkitPro 装好后 source /etc/profile.d/devkit-env.sh（或 docker 用 devkitpro/devkita64 镜像）)
   endif
   # 显式指定工具链：CI/本地 shell 里若导出了 CC=zig 之类会污染构建
   CC  := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
   CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
   AR  := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-ar
   NX_ARCH := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE -mcpu=cortex-a57+crc+fp+simd
   # 裸机没有 -fPIC 共享对象概念，PIE 才是 NRO 的正确模型
   CXXFLAGS := $(filter-out -fPIC,$(CXXFLAGS)) $(NX_ARCH) -fdata-sections
   CFLAGS   := $(filter-out -fPIC,$(CFLAGS)) $(NX_ARCH) -fdata-sections
   CXXFLAGS += -D__SWITCH__ -DHAVE_LIBNX -D_GNU_SOURCE
   CFLAGS   += -D__SWITCH__ -DHAVE_LIBNX -D_GNU_SOURCE
   # 诊断构建开关：打开后核心每 30 帧往 sdmc:/switch/mca_run.log 写一行运行期
   # trace（裸 POSIX IO，堆坏了也写得出去），并支持 mca_mode.txt 切通路口径。
   # 发版包不开（写 SD 卡换不到收益），只有 nx-diag 的两个对比 NRO 开。
   ifeq ($(MCA_NX_DIAG),1)
      CXXFLAGS += -DMCA_NX_DIAG=1
      CFLAGS   += -DMCA_NX_DIAG=1
   endif
else ifneq (,$(findstring win,$(platform)))
   TARGET  := $(CORE_DIR)/$(TARGET_NAME).dll
   LDFLAGS += -shared -Wl,--export-all-symbols
   # Windows PE 不需要 -fPIC；MinGW 对 -fPIC 只会多一层间接
   CXXFLAGS := $(filter-out -fPIC,$(CXXFLAGS))
   CFLAGS   := $(filter-out -fPIC,$(CFLAGS))
   # VM 代码用了 posix 线程（pthread_mutex/pthread_create）。zig 的 mingw
   # 目标自带 pthread 仿真不用管；真 mingw-gcc（buildbot 的 MXE、apt 的
   # mingw-w64）的 win32 线程模型不会自动链 winpthreads，链接时报
   # undefined reference to pthread_mutex_lock。探测工具链里哪个库在就链哪个
   #（-print-file-name 找不到时原样返回名字本身，据此判断）。
   PTHREAD_A := $(shell $(CC) -print-file-name=libpthread.a 2>/dev/null)
   ifeq ($(PTHREAD_A),libpthread.a)
      PTHREAD_A := $(shell $(CC) -print-file-name=libwinpthread.a 2>/dev/null)
   endif
   ifneq ($(PTHREAD_A),)
   ifneq ($(PTHREAD_A),libpthread.a)
   ifneq ($(PTHREAD_A),libwinpthread.a)
      LDLIBS += -lpthread
   endif
   endif
   endif
   # 把 Windows API 基线抬到 Win7（只用 #ifndef 兜底，见该头文件注释）：
   # mingw-gcc 默认按 XP 裁 windows.h，platform.cpp 的 GetTickCount64 编不过；
   # 官方 buildbot 用的就是 mingw-gcc。
   WIN_BASELINE := -include $(CORE_DIR)/src/win32_baseline.h
   CXXFLAGS += $(WIN_BASELINE)
   CFLAGS   += $(WIN_BASELINE)
else ifneq (,$(findstring osx,$(platform)))
   TARGET  := $(CORE_DIR)/$(TARGET_NAME).dylib
   LDFLAGS += -dynamiclib -undefined dynamic_lookup
else ifneq (,$(findstring android,$(platform)))
   TARGET  := $(CORE_DIR)/$(TARGET_NAME).so
   # -static-libstdc++：产物只依赖 libc/libm/libdl，RetroArch 无需 libc++_shared.so
   # max-page-size=16384：Android 15 起有 16KB 页设备，4KB 对齐无法 dlopen
   LDFLAGS += -shared -static-libstdc++ -Wl,-z,max-page-size=16384
   LDFLAGS += -Wl,-soname,$(TARGET_NAME).so
   # build-id：NDK 打包时会把 .so strip 掉，符号只能靠 build-id 才能与
   # 对应的调试文件对上（ndk-stack / addr2line 都用它）。不加这个段，
   # strip 之后崩溃地址就没法回溯到源码行。
   LDFLAGS += -Wl,--build-id=sha1
else
   # Linux 及其它 POSIX。不加 --no-undefined：libc++ 里未用到的代码可能引用
   # 目标 glibc 没有的符号（如 ppc64le 的 __vsnprintfieee128），动态链接下无害。
   TARGET  := $(CORE_DIR)/$(TARGET_NAME).so
   LDFLAGS += -shared -Wl,-soname,$(TARGET_NAME).so
   # DF_1_NODELETE：pxt 运行时带 static 析构。RetroArch 退出时先 dlclose 再走
   # atexit 链，析构函数会跳进已解除映射的代码段 —— 用户侧表现就是"退出即 139"。
   # 让 ld.so 保留映射即可；代价是换核心时不回收，单次会话可忽略。
   LDFLAGS += -Wl,-z,nodelete
   # build-id：发行包里的 ELF 会 strip 掉调试信息（体积差 5 倍），strip 后
   # 只剩 build-id 能把"用户报的崩溃地址"对回源码行。sha1 是内容哈希，
   # 不破坏可复现构建：同样输入仍然得到同样的 build-id。
   LDFLAGS += -Wl,--build-id=sha1
endif

# 插桩产物别覆盖 core/ 根下那个"当前正式可用"的 .so：覆盖率构型改落对象目录
ifneq ($(COVERAGE),)
   TARGET := $(BUILD_DIR)/$(notdir $(TARGET))
endif

# ---------------------------------------------------------------- 源文件# 清单只有一份：core/sources.mk。Android.mk 与 tools/build_*.sh 读的是同一个
# 文件（shell 侧用 tools/lib/sources.sh），加源文件只改那一个地方。
include $(CORE_DIR)/sources.mk

# VM 内核 + 引擎：只编清单里的子目录（runtime/pxtapp/main.cpp 有自己的 main()，排除）
VM_SOURCES := $(foreach d,$(MCA_VM_DIRS),$(sort $(wildcard $(PXTAPP_DIR)/$(d)/*.cpp)))
VM_SOURCES += $(foreach f,$(MCA_VM_FILES),$(PXTAPP_DIR)/$(f))

# core 自身：只有两个 C++ 源。卡带侧不做任何解码（v0.3.5 起 PNG 入口与
# cart/miniz/lzma 一并删除，只认 .pxt64 镜像）。
# 一律用 $(CORE_DIR) 前缀，使规则不依赖当前工作目录
CORE_CPP_SOURCES := $(foreach f,$(MCA_CORE_FILES),$(CORE_DIR)/$(f))

# pxtapp 用 C++11（上游如此），core 宿主用 C++14
# 对象路径去掉源码前缀（pxtapp 在 ../ 下，直接拼会产生带 .. 的目标名）
VM_OBJS   := $(patsubst $(PXTAPP_DIR)/%.cpp,$(OBJ_DIR)/vm/%.o,$(VM_SOURCES))
CORE_OBJ  := $(patsubst $(CORE_DIR)/%.cpp,$(OBJ_DIR)/core/%.o,$(CORE_CPP_SOURCES))
OBJECTS   := $(VM_OBJS) $(CORE_OBJ)
DEPS      := $(OBJECTS:.o=.d)

# ---------------------------------------------------------------- 规则
all: $(TARGET)

ifeq ($(STATIC_LINKING),1)
# 静态库：不链接，只归档（符号解析留给 RetroArch 的 NRO 链接阶段）。
# 必须先把对象拷成"路径扁平化"的唯一文件名：GNU ar 默认丢弃目录、只存基名，
# 而 pxtapp 里 base/control.cpp 与 core---vm/control.cpp 同名，直接归档会
# 让后一个成员静默覆盖前一个（符号就没了，链接期才炸，极难查）。
FLAT_DIR := $(BUILD_DIR)/flat
$(TARGET): $(OBJECTS)
	@rm -rf $(FLAT_DIR) $@
	@mkdir -p $(FLAT_DIR)
	@for o in $(OBJECTS); do cp "$$o" "$(FLAT_DIR)/o_$$(printf '%s' "$$o" | tr '/' '_')"; done
	$(AR) rcs $@ $(FLAT_DIR)/*.o
	@narm=$$($(AR) t $@ | wc -l); nobj=$(words $(OBJECTS)); \
	 if [ "$$narm" -ne "$$nobj" ]; then \
	   echo "归档成员数 $$narm != 对象数 $$nobj（有重名成员被覆盖）" >&2; exit 1; fi; \
	 echo "  $$narm 个对象已归档"
else
$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)
endif

$(OBJ_DIR)/vm/%.o: $(PXTAPP_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++11 $(INCFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/core/%.o: $(CORE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++14 $(INCFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/core/%.o: $(CORE_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCFLAGS) -MMD -MP -c -o $@ $<

# ------------------------------------------------------- 契约自检（假前端）
# 不需要卡带：校验 get_system_info、环境调用顺序（像素格式必须在 load 前协商）、
# descriptor / controller / variables 三个数组的终止哨兵，以及"非 .pxt64 必须
# load 失败"。CI 在 buildbot 同构型（gcc/make）和 UBSan 构型各跑一次。
# 只对会产动态库的 unix 目标开放：libnx 是静态库、win64 上的可执行文件另有用途。
#
# 两个实现细节：
#   * 链接不用 $(LDFLAGS)：unix 分支里有 -shared，会把测试程序链成 .so
#   * 运行放到 build 目录：核心会在 CWD 落 mca_trace.log，别污染源码树
ifneq ($(STATIC_LINKING),1)
ifneq (,$(findstring unix,$(platform)))
CONTRACT_BIN := $(BUILD_DIR)/contract_test

contract-test: $(CONTRACT_BIN)
	cd $(BUILD_DIR) && ./contract_test

$(CONTRACT_BIN): $(OBJECTS) $(CORE_DIR)/tests/contract_test.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -std=c++14 $(INCFLAGS) -o $@ \
	   $(CORE_DIR)/tests/contract_test.cpp $(OBJECTS) $(SANFLAGS) $(COVFLAGS) -lpthread
endif
endif

# 本地快捷方式：用 zig 当编译器驱动（本机无 gcc/mingw 时）
#   make platform=win64 CC="zig cc -target x86_64-windows-gnu" \
#                      CXX="zig c++ -target x86_64-windows-gnu"
#   make platform=unix  CC="zig cc -target x86_64-linux-gnu.2.17" \
#                       CXX="zig c++ -target x86_64-linux-gnu.2.17"
ZIG ?=

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)

.PHONY: all clean contract-test

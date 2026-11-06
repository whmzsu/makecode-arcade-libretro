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

# 产物与对象：对象按平台分目录，避免多目标构建互相覆盖（build/ 已在 .gitignore）
# platform 可能带后缀（如 unix-armv7-hardfloat-neon），空格/斜杠会破坏路径，
# 统一压成 - 分隔的目录名
empty      :=
space      := $(empty) $(empty)
BUILD_TAG  := $(subst $(space),-,$(strip $(platform)))
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

INCFLAGS := -I$(CORE_DIR)/src -I$(CORE_DIR)/third_party/miniz \
            -I$(CORE_DIR)/third_party/lzma -I$(PXTAPP_DIR)

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
else
   # Linux 及其它 POSIX。不加 --no-undefined：libc++ 里未用到的代码可能引用
   # 目标 glibc 没有的符号（如 ppc64le 的 __vsnprintfieee128），动态链接下无害。
   TARGET  := $(CORE_DIR)/$(TARGET_NAME).so
   LDFLAGS += -shared -Wl,-soname,$(TARGET_NAME).so
endif

# ---------------------------------------------------------------- 源文件
# VM 内核 + 引擎：只编这些子目录（runtime/pxtapp/main.cpp 有自己的 main()，排除）
VM_DIRS    := base core---vm game mixer---ext screen---ext settings---files
VM_SOURCES := $(foreach d,$(VM_DIRS),$(sort $(wildcard $(PXTAPP_DIR)/$(d)/*.cpp)))
VM_SOURCES += $(PXTAPP_DIR)/pointers.cpp

# core 自身：C 组件（cart/decompress 自 mcarcade-core，MIT）+ C++ 宿主
# 一律用 $(CORE_DIR) 前缀，使规则不依赖当前工作目录
CORE_C_SOURCES   := $(CORE_DIR)/src/cart.c $(CORE_DIR)/src/decompress.c \
                    $(CORE_DIR)/third_party/miniz/miniz_tinfl.c \
                    $(CORE_DIR)/third_party/lzma/LzmaDec.c
CORE_CPP_SOURCES := $(CORE_DIR)/src/libretro_core.cpp $(CORE_DIR)/src/png_loader.cpp \
                    $(CORE_DIR)/src/vm_host.cpp

# pxtapp 用 C++11（上游如此），core 宿主用 C++14
# 对象路径去掉源码前缀（pxtapp 在 ../ 下，直接拼会产生带 .. 的目标名）
VM_OBJS   := $(patsubst $(PXTAPP_DIR)/%.cpp,$(OBJ_DIR)/vm/%.o,$(VM_SOURCES))
CORE_COBJ := $(patsubst $(CORE_DIR)/%.c,$(OBJ_DIR)/core/%.o,$(CORE_C_SOURCES))
CORE_OBJ  := $(patsubst $(CORE_DIR)/%.cpp,$(OBJ_DIR)/core/%.o,$(CORE_CPP_SOURCES))
OBJECTS   := $(VM_OBJS) $(CORE_COBJ) $(CORE_OBJ)
DEPS      := $(OBJECTS:.o=.d)

# ---------------------------------------------------------------- 规则
all: $(TARGET)

ifeq ($(STATIC_LINKING),1)
# 静态库：不链接，只归档（符号解析留给 RetroArch 的 NRO 链接阶段）
$(TARGET): $(OBJECTS)
	@rm -f $@
	$(AR) rcs $@ $(OBJECTS)
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

# 本地快捷方式：用 zig 当编译器驱动（本机无 gcc/mingw 时）
#   make platform=win64 CC="zig cc -target x86_64-windows-gnu" \
#                      CXX="zig c++ -target x86_64-windows-gnu"
#   make platform=unix  CC="zig cc -target x86_64-linux-gnu.2.17" \
#                       CXX="zig c++ -target x86_64-linux-gnu.2.17"
ZIG ?=

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)

.PHONY: all clean

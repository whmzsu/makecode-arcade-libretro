# Android NDK 构建脚本（官方 buildbot 的 android-jni 模板要求）
#
# 模板用法：JNI_PATH=core，模板在 core/jni 下执行 ndk-build，
# 由 Application.mk 指定 ABI / API 级别。
#
# 本地验证（在仓库根执行，产物放 out/ 避免污染源码树）：
#   NDK="$ANDROID_NDK_HOME"
#   "$NDK/ndk-build" NDK_PROJECT_PATH=. \
#       APP_BUILD_SCRIPT=core/jni/Android.mk \
#       NDK_APPLICATION_MK=core/jni/Application.mk \
#       APP_ABI=arm64-v8a NDK_OUT=out/ndk/obj NDK_LIBS_OUT=out/ndk/libs

LOCAL_PATH := $(call my-dir)

# ndk-build 把 LOCAL_SRC_FILES 当相对 LOCAL_PATH 解析，必须给绝对路径
CORE_DIR   := $(abspath $(LOCAL_PATH)/..)
PXTAPP_DIR := $(abspath $(CORE_DIR)/../runtime/pxtapp)

include $(CLEAR_VARS)
# 必须叫 retro（产出 libretro.so）：buildbot 的 android-jni 模板固定执行
#   mv $JNI_PATH/libs/$ABI/libretro.so ${CORENAME}_libretro_android.so
# 模块名换成别的（如 core 名）会让 mv 找不到文件、job 直接红。
LOCAL_MODULE := retro
LOCAL_CPP_EXTENSION := .cpp

# VM 内核 + 引擎：只编这些子目录（runtime/pxtapp/main.cpp 有自己的 main()，排除）
VM_DIRS    := base core---vm game mixer---ext screen---ext settings---files
VM_SOURCES := $(foreach d,$(VM_DIRS),$(sort $(wildcard $(PXTAPP_DIR)/$(d)/*.cpp)))
VM_SOURCES += $(PXTAPP_DIR)/pointers.cpp

# core 自身：C 组件 + C++ 宿主
CORE_C_SOURCES   := $(CORE_DIR)/src/cart.c $(CORE_DIR)/src/decompress.c \
                    $(CORE_DIR)/third_party/miniz/miniz_tinfl.c \
                    $(CORE_DIR)/third_party/lzma/LzmaDec.c
CORE_CPP_SOURCES := $(CORE_DIR)/src/libretro_core.cpp $(CORE_DIR)/src/png_loader.cpp \
                    $(CORE_DIR)/src/vm_host.cpp

LOCAL_SRC_FILES  := $(VM_SOURCES) $(CORE_C_SOURCES) $(CORE_CPP_SOURCES)
LOCAL_C_INCLUDES := $(CORE_DIR)/src $(CORE_DIR)/third_party/miniz \
                    $(CORE_DIR)/third_party/lzma $(PXTAPP_DIR)

# 与 core/Makefile 保持同一套开关：
#   -fno-rtti/-fno-exceptions：pxt VM 不用异常/RTTI
#   -fwrapv：VM 依赖有符号整数回绕语义
#   -fno-threadsafe-statics：pxt 的 SINGLETON 是裸 static，防引入 __cxa_guard
COMMON_FLAGS := -O2 -fwrapv -fno-strict-aliasing -ffunction-sections
LOCAL_CFLAGS   := $(COMMON_FLAGS) -std=c99
LOCAL_CPPFLAGS := $(COMMON_FLAGS) -fno-rtti -fno-exceptions -fno-unwind-tables \
                  -fno-threadsafe-statics -std=c++14 -Wno-unused-parameter

# max-page-size=16384：Android 15 起有 16KB 页设备，4KB 对齐的 .so 无法 dlopen
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384

include $(BUILD_SHARED_LIBRARY)

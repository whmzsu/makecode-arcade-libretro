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

# VM 内核 + 引擎 + core 宿主：清单只有一份，与 core/Makefile、tools/build_*.sh
# 共用 core/sources.mk（加源文件只改那一个地方）。
include $(CORE_DIR)/sources.mk

VM_SOURCES := $(foreach d,$(MCA_VM_DIRS),$(sort $(wildcard $(PXTAPP_DIR)/$(d)/*.cpp)))
VM_SOURCES += $(foreach f,$(MCA_VM_FILES),$(PXTAPP_DIR)/$(f))

# core 自身：只有两个 C++ 源（v0.3.5 起 PNG 解码链路整体移除，只认 .pxt64）
CORE_CPP_SOURCES := $(foreach f,$(MCA_CORE_FILES),$(CORE_DIR)/$(f))

LOCAL_SRC_FILES  := $(VM_SOURCES) $(CORE_CPP_SOURCES)
LOCAL_C_INCLUDES := $(CORE_DIR)/src $(PXTAPP_DIR)

# 与 core/Makefile 保持同一套开关：
#   -fno-rtti/-fno-exceptions：pxt VM 不用异常/RTTI
#   -fwrapv：VM 依赖有符号整数回绕语义
#   -fno-threadsafe-statics：pxt 的 SINGLETON 是裸 static，防引入 __cxa_guard
COMMON_FLAGS := -O2 -fwrapv -fno-strict-aliasing -ffunction-sections
LOCAL_CPPFLAGS := $(COMMON_FLAGS) -fno-rtti -fno-exceptions -fno-unwind-tables \
                  -fno-threadsafe-statics -std=c++14 -Wno-unused-parameter

# max-page-size=16384：Android 15 起有 16KB 页设备，4KB 对齐的 .so 无法 dlopen
# build-id：ndk-build 安装时会 strip 掉 .so，只有 build-id 能把崩溃地址
# 对回源码行（ndk-stack 依赖它）。与 core/Makefile / build_target.sh 保持一致。
# -z nodelete（DF_1_NODELETE）：pxt 运行时带 static 析构，RetroArch 卸载核心时
# dlclose 之后退出进程仍会跳进已解除映射的代码段 → 退出即 139（0.3.7 修的就是
# 这个）。**这里是第四个链接入口**：Android 发行包走 ndk-build，前三个都管不到，
# 0.3.7 当时漏了这处，Android 包仍会崩。
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1 -Wl,-z,nodelete

include $(BUILD_SHARED_LIBRARY)

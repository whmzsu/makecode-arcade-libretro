# 用 devkitPro 官方 switch_rules 编探针（而不是裸 gcc 调用）
#
# 存在的理由：裸 gcc 编出来的 probe.nro 在真机上起不来，而同一容器里用
# switch_rules 编的 ra_plain.nro 能起来。两者的差别不只是编译选项，还有
# 链接脚本、段布局、__nx_exit 收尾这些东西。这个 Makefile 走同一条官方路径，
# 用来判定"是不是我们的裸 gcc 调用姿势不对"。
#
# 由 build-diag.sh 调用：它会先把 probe.c 拷到本目录再 make。
# 这里只写规则，不要放 probe.c 本体（避免两份源码）。

TARGET   := probe_rules
BUILD    := build
SOURCES  := .
INCLUDES :=
LIBDIRS  :=

APP_TITLE  := NX probe
APP_AUTHOR := makecode-arcade
APP_VERSION := diag

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) -D__SWITCH__ \
            -DBUILD_LIBNX_VER=\"switch_rules\"
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
LDFLAGS  := -g $(ARCH) -specs=$(DEVKITPRO)/libnx/switch.specs
LIBS     := -lnx

include $(DEVKITPRO)/libnx/switch_rules

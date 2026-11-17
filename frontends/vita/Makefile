# MCA 最小前端（PS Vita 测试包）：链 core 的 libretro_vita.a 出 .vpk
# 用法：export VITASDK=/path/to/vitasdk 后 make
ifeq ($(VITASDK),)
$(error 需要 VITASDK 环境变量（vitaSDK 根目录，bin/ 含 arm-vita-eabi-*）)
endif
PREFIX := $(VITASDK)/bin/arm-vita-eabi-

TARGET     := mca_vita
TITLE_ID   := MCA00001
APP_VER    := 00.10
TITLE      := MCA Core Test
CORE_A     := ../../core/makecode_arcade_libretro_vita.a

# 与 core/Makefile platform=vita 同 ABI：ARMv7-A hard-float NEON
ARCH       := -march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon
CFLAGS     := $(ARCH) -O2 -Wall -Wno-unused-parameter -I../../core
# core 静态库引用 pthread（PTE 实现，需要 SceKernelThreadMgr）；文件 I/O 走
# newlib -> SceIofilemgr。缺 stub 链接器会报 undefined，按报错补。
LIBS       := -lpthread -lSceKernelThreadMgr_stub \
              -lSceDisplay_stub -lSceCtrl_stub -lSceAudio_stub \
              -lSceIofilemgr_stub -lm

OBJS       := main.o

all: $(TARGET).vpk

$(TARGET).velf: $(TARGET).elf
	$(VITASDK)/bin/vita-elf-create $< $@

$(TARGET).vpk: $(TARGET).velf param.sfo
	$(VITASDK)/bin/vita-pack-vpk -s param.sfo -b $(TARGET).velf $@

param.sfo:
	$(VITASDK)/bin/vita-mksfoex -s TITLE_ID=$(TITLE_ID) -s APP_VER=$(APP_VER) "$(TITLE)" $@

$(TARGET).elf: $(OBJS) $(CORE_A)
	$(PREFIX)g++ $(ARCH) $(OBJS) $(CORE_A) $(LIBS) -o $@

%.o: %.c
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).velf param.sfo $(TARGET).vpk

.PHONY: all clean

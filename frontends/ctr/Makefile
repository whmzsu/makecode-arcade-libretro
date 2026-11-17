# MCA 最小前端（3DS 测试包）：链 core 的 libretro_ctr.a 出 .3dsx
# 用法：source /etc/profile.d/devkit-env.sh（或 DEVKITPRO=/path）后 make
ifeq ($(DEVKITPRO),)
$(error 需要 DEVKITPRO 环境变量（devkitPro 根目录，含 devkitARM/tools）)
endif
DEVKITARM := $(DEVKITPRO)/devkitARM
PREFIX    := $(DEVKITARM)/bin/arm-none-eabi-
TOOLBIN   := $(DEVKITPRO)/tools/bin

TARGET    := mca_ctr
CORE_A    := ../../core/makecode_arcade_libretro_ctr.a

# 与 core/Makefile platform=ctr 同 ABI：ARMv6k hard-float vfp
ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp
CTR_INC   := -I$(DEVKITPRO)/libctru/include
CTR_LIB   := -L$(DEVKITPRO)/libctru/lib
CFLAGS    := $(ARCH) -O2 -Wall -Wno-unused-parameter $(CTR_INC) -I../../core
LIBS      := $(CTR_LIB) -lsysbase -lctru -lm

OBJS      := main.o

all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf
	$(TOOLBIN)/3dsxtool $< $@

$(TARGET).elf: $(OBJS) $(CORE_A)
	$(PREFIX)g++ $(ARCH) -specs=3dsx.specs $(OBJS) $(CORE_A) $(LIBS) -o $@

%.o: %.c
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).3dsx

.PHONY: all clean

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

# CIA 打包（需要 tools/bin 里有 makerom/bannertool：dkp-pacman -S 3ds-cia-tools；
# devkitarm 容器默认不带，CI 的 ctr job 会先装）
APP_TITLE := MCA Core Test

all: $(TARGET).3dsx
cia: $(TARGET).cia

$(TARGET).cia: $(TARGET).elf $(TARGET).smdh $(TARGET).bnr app.rsf
	$(TOOLBIN)/makerom -f cia -o $@ -elf $(TARGET).elf \
	  -icon $(TARGET).smdh -banner $(TARGET).bnr \
	  -rsf app.rsf -DAPP_TITLE="$(APP_TITLE)"

$(TARGET).smdh: icon48.png
	# bannertool(diasurgical fork) 的标题/发布者参数按语言走：
	# -es=英文短标题 -el=英文长标题 -ep=英文发布者
	$(TOOLBIN)/bannertool makesmdh \
	  -es "$(APP_TITLE)" \
	  -el "MCA libretro core minimal frontend (PXT32 static)" \
	  -ep "mca-arcade" -i $< -o $@

$(TARGET).bnr: banner256.png
	$(TOOLBIN)/bannertool makebanner -i $< -o $@

$(TARGET).3dsx: $(TARGET).elf
	$(TOOLBIN)/3dsxtool $< $@

$(TARGET).elf: $(OBJS) $(CORE_A)
	$(PREFIX)g++ $(ARCH) -specs=3dsx.specs $(OBJS) $(CORE_A) $(LIBS) -o $@

%.o: %.c
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).3dsx \
	      $(TARGET).smdh $(TARGET).bnr $(TARGET).cia

.PHONY: all cia clean

# MakeCode Arcade libretro core（路线 B）
# 本机构建:  make
# 交叉构建:  make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++
TARGET  := makecode_arcade_libretro

CC       ?= gcc
CXX      ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra -fPIC -std=c++14
CFLAGS   ?= -O2 -Wall -Wextra -fPIC -std=c99
LDFLAGS  := -shared
LDLIBS   :=

# 平台检测
ifeq ($(OS),Windows_NT)
  EXT := dll
else
  UNAME := $(shell uname -s)
  ifeq ($(UNAME),Darwin)
    EXT := dylib
    LDFLAGS := -shared -undefined dynamic_lookup
  else
    EXT := so
    LDFLAGS += -Wl,--no-undefined
  endif
endif

INC := -Isrc -Ithird_party/miniz -Ithird_party/lzma

# C++ 核心 + vendor 的 C 组件（cart/decompress 自 mcarcade-core，MIT）
# 注意：miniz 只编 miniz_tinfl.c（tinfl inflate 单元，cart.c 仅用此功能），
# 编整个 miniz.c 会引入 zip/archive 实现且部分发行版缺 miniz_zip.c。
CPP_SOURCES := src/libretro_core.cpp src/png_loader.cpp
C_SOURCES   := src/cart.c src/decompress.c \
               third_party/miniz/miniz_tinfl.c \
               third_party/lzma/LzmaDec.c
OBJECTS := $(CPP_SOURCES:.cpp=.o) $(C_SOURCES:.c=.o)
DEPS    := $(CPP_SOURCES:.cpp=.d) $(C_SOURCES:.c=.d)

all: $(TARGET).$(EXT)

$(TARGET).$(EXT): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -MMD -MP -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -MMD -MP -c -o $@ $<

-include $(DEPS)

# zig 构建（无 gcc/make 环境时用 zig cc/c++ 当驱动，已在本机验证）：
#   ZIG="<zig.exe 路径>"; cd core
#   $ZIG cc -O2 -c -Isrc -Ithird_party/miniz -Ithird_party/lzma src/cart.c -o cart.o
#   $ZIG cc -O2 -c -Isrc -Ithird_party/miniz -Ithird_party/lzma src/decompress.c -o decompress.o
#   $ZIG cc -O2 -c -Isrc -Ithird_party/miniz -Ithird_party/lzma third_party/miniz/miniz_tinfl.c -o miniz_tinfl.o
#   $ZIG cc -O2 -c -Isrc -Ithird_party/miniz -Ithird_party/lzma third_party/lzma/LzmaDec.c -o LzmaDec.o
#   $ZIG c++ -O2 -std=c++14 -Isrc -c src/libretro_core.cpp -o libretro_core.o
#   $ZIG c++ -O2 -std=c++14 -Isrc -c src/png_loader.cpp -o png_loader.o
#   $ZIG c++ -shared -o makecode_arcade_libretro.dll libretro_core.o png_loader.o cart.o decompress.o miniz_tinfl.o LzmaDec.o
# （Linux/macOS 把 .dll 换 .so/.dylib，加 -fPIC；zig c++ 一次只能编一个源文件）

clean:
	rm -f $(OBJECTS) $(DEPS) $(TARGET).$(EXT) *.o

.PHONY: all clean

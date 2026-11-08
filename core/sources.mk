# core/sources.mk —— 源文件清单的唯一真源（5 个构建入口共用）
#
# 谁在用：
#   core/Makefile           make（unix / win64 / osx），libretro buildbot 走的也是这条路
#   core/jni/Android.mk     ndk-build（官方 android-jni 模板）
#   tools/build_core.sh     本地 zig 开发构建（DLL + 三个测试程序）
#   tools/build_target.sh   发版矩阵（zig 全目标 + NDK clang）
#   tools/build_musl.sh     Lakka musl 全静态
#
# shell 侧不解析 make 语法：tools/lib/sources.sh 用
#     sed -n 's/^<变量名>[ \t]*:=[ \t]*//p'
# 读下面三行。所以**只保留"变量名 := 空格分隔的值"这一种写法**：
# 不要用 +=、续行、$(call) 或注释混排，shell 读不懂。
#
# 加一个源文件只改这里。以前 5 处各列一份清单，v0.3.5 删 cart/decompress/miniz/lzma
# 那轮漏改了两处，靠 CI 链接失败才发现——那还是"多编了已删文件"的方向，
# 反过来漏列一个源文件会静默少符号或漏掉补丁，更难查。

# pxtapp 下要编的子目录（相对 runtime/pxtapp）。
# main.cpp 有自己的 main()，所以不在这里：它只属于官方 sim 宿主，不进程内链接。
MCA_VM_DIRS := base core---vm game mixer---ext screen---ext settings---files

# 相对 runtime/pxtapp 的散装源文件。pointers.cpp 是 codegen 生成物，
# 里面带着 PATCH 2（aspxt:: 笔误），重新生成 pxtapp 时最容易漏。
MCA_VM_FILES := pointers.cpp

# 相对 core/ 的宿主源文件。v0.3.5 起 PNG 解码链路（cart/decompress/miniz/lzma）
# 整体移除，只认 .pxt64 镜像，所以只剩这三个。
# nx_diag.cpp 在非 Switch 平台编成空 TU（零符号），摆在清单里不必加平台条件。
MCA_CORE_FILES := src/libretro_core.cpp src/vm_host.cpp src/nx_diag.cpp

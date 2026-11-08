#!/usr/bin/env bash
# sources.sh —— 把 core/sources.mk 读成 shell 列表（被 build_*.sh source）
#
# 唯一真源是 core/sources.mk；这里不存任何清单，只负责"读"。
# 三个 shell 构建脚本以前各自硬列一遍目录，删源文件时漏改过两处。
#
# 调用前脚本必须设好：MCA_SOURCES_MK（清单文件路径）、CORE、PXTAPP。

# mca_src_var <变量名> —— 取 "名字 := 值" 里 := 之后的部分
mca_src_var() {
  sed -n "s/^$1[[:space:]]*:=[[:space:]]*//p" "${MCA_SOURCES_MK:?MCA_SOURCES_MK 未设置}"
}

# pxtapp 下的 VM/引擎源文件（绝对路径；目录顺序照清单，目录内按字节序排）
#
# 用 find 而不是 ls：`ls *.cpp` 在没有匹配时返回非 0，在 set -e 的脚本里
# 会让整步静默失败（目录被改名/清空时表现为"编译 0 个"然后链接才报错）。
mca_vm_sources() {
  local d
  for d in $(mca_src_var MCA_VM_DIRS); do
    LC_ALL=C find "$PXTAPP/$d" -maxdepth 1 -name '*.cpp' | LC_ALL=C sort
  done
  for d in $(mca_src_var MCA_VM_FILES); do
    printf '%s\n' "$PXTAPP/$d"
  done
}

# core/ 下的宿主源文件（绝对路径）
mca_core_sources() {
  local f
  for f in $(mca_src_var MCA_CORE_FILES); do
    printf '%s\n' "$CORE/$f"
  done
}

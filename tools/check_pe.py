#!/usr/bin/env python3
# check_pe.py — PE 产物检查（CI 的 zig job 与本地自检共用）
#
# 为什么不用 objdump：binutils 的 x86_64-w64-mingw32-objdump 解不了 aarch64
# PE 的导出表（CI 上误报"缺少导出符号"，实际 25 个 retro_* 都在）。
# pefile 架构无关，x64 / arm64 一套代码通吃。依赖：pip install pefile
#
# 用法：python tools/check_pe.py <path.dll> [<path.dll> ...]
# 检查项（发现即非零退出）：
#   * 导入表不得依赖 libstdc++-6 / libgcc_s_* / libwinpthread-1
#     （玩家机器上通常没有，分发 DLL 必须全静态）
#   * 必需导出：retro_init / retro_run / retro_get_system_info / retro_load_game
import sys

try:
    import pefile
except ImportError:
    sys.exit("pefile 未安装：pip install pefile")

BAD_DLLS = {"libstdc++-6.dll", "libgcc_s_seh-1.dll", "libgcc_s_dw2-1.dll",
            "libwinpthread-1.dll"}
REQUIRED = ["retro_init", "retro_run", "retro_get_system_info", "retro_load_game"]

fail = 0
for path in sys.argv[1:]:
    print(f"== {path} ==")
    pe = pefile.PE(path)
    imports = sorted({e.dll.decode() for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", [])})
    print("   导入:", ", ".join(imports) or "(无)")
    hit = sorted(set(imports) & BAD_DLLS)
    if hit:
        print(f"::error::{path} 依赖 {','.join(hit)}，玩家机器上会加载失败")
        fail = 1
    exports = [e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name]
    missing = [s for s in REQUIRED if s not in exports]
    if missing:
        print(f"::error::{path} 缺少导出符号 {','.join(missing)}")
        fail = 1
    else:
        print(f"   导出: retro_* 共 {sum(1 for n in exports if n.startswith('retro_'))} 个，"
              f"必需 {len(REQUIRED)} 个全在")
    pe.close()

sys.exit(fail)

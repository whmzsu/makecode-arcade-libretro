#!/usr/bin/env bash
# coverage.sh — 覆盖率一键：插桩构建 → 跑 golden 三例（顺带验哈希）→ gcovr 报告
#
# 为什么需要：contract_test 断言的是"契约边界"，golden hash 断言的是"画面对不对"。
# 两者都不回答"这 4 千行里有多少从没被执行过"——视频 bpp 分支、音频重采样、
# 存档 stub、错误码路径现在全靠人读代码猜。覆盖率是唯一能把这件事实测出来的东西。
#
# 用法：
#   tools/coverage.sh                     # 构建 + 3 个 golden 用例 + 出报告
#   tools/coverage.sh --corpus DIR        # 额外把 DIR 里的卡带全跑一遍（慢，覆盖率高）
#   tools/coverage.sh --report-dir D      # 换报告目录（默认 out/coverage）
#   tools/coverage.sh --keep              # 不清 .gcda，多次运行数据合并
#
# 产物：<report>/summary.txt（按目录汇总）、coverage.xml（CI/IDE）、
#       index.html（逐行标红，直接浏览器打开）、uncovered-core.txt（核心侧未执行行）
#
# 前提：Linux + gcc/g++ + gcovr（工具机已装）+ python3（宿主是 ctypes 写的）。
# 注意：-O0 插桩版跑得慢（golden 三例约 1~2 分钟）；别拿它做性能判断。

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"

BUILD="$PROJ/core/build/make/unix-cov"
CORE="$BUILD/makecode_arcade_libretro.so"
REPORT="$PROJ/out/coverage"
KEEP=0
CORPUS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --report-dir) REPORT="$2"; shift 2 ;;
    --keep)       KEEP=1; shift ;;
    --corpus)
         if [ $# -ge 2 ] && [ "${2#-}" = "$2" ]; then CORPUS="$2"; shift 2
         else CORPUS="$PROJ/out/testcarts"; shift; fi ;;
    -h|--help)    sed -n '2,19p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)            echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

command -v gcovr >/dev/null 2>&1 || {
  echo "缺 gcovr：apt-get install -y gcovr（工具机已装）" >&2; exit 2; }
command -v make >/dev/null 2>&1 || { echo "缺 make" >&2; exit 2; }

echo "== 1/4 插桩构建（-O0 --coverage）"
# 不 --keep 就先清干净：旧 .gcda 与新行号对不上，报告会串行
[ "$KEEP" = 1 ] || rm -rf "$BUILD"
# 必须在仓库根用 -f core/Makefile 编：make -C core 会把源码记成 `../runtime/...`，
# gcovr 无法把它解析回源文件（"Cannot open source file ../runtime/pxtapp/..."）。
# 在根目录编，路径是 `core/src/...` 与 `core/../runtime/...`，都能从根解析。
(cd "$PROJ" && make -f core/Makefile platform=unix COVERAGE=1 -j"$(nproc)" >/dev/null)
[ -f "$CORE" ] || { echo "没找到插桩产物: $CORE" >&2; exit 1; }
echo "   $CORE"

echo "== 2/4 跑 golden 三例（插桩只是慢，末帧哈希必须仍逐位一致）"
bash "$PROJ/tools/ci_regress.sh" "$CORE"

if [ -n "$CORPUS" ]; then
  echo "== 2b/4 追加语料：$CORPUS"
  python3 "$PROJ/tools/sweep_carts.py" "$CORPUS" --core "$CORE" -n 300 --turbo \
    -o "$REPORT/../sweep-coverage.json" | tail -5
fi

echo "== 3/4 gcovr 汇总"
mkdir -p "$REPORT"
COMMON=(-r "$PROJ" --object-directory "$BUILD"
        --filter "$PROJ/core/src/" --filter "$PROJ/runtime/pxtapp/"
        --exclude-unreachable-branches)
gcovr "${COMMON[@]}" --txt -o "$REPORT/summary.txt" >/dev/null
gcovr "${COMMON[@]}" --xml-pretty -o "$REPORT/coverage.xml" >/dev/null
gcovr "${COMMON[@]}" --html-details -o "$REPORT/index.html" >/dev/null
gcovr "${COMMON[@]}" --json -o "$REPORT/coverage.json" >/dev/null

echo "== 4/4 核心侧未执行行 → uncovered-core.txt"
python3 - "$PROJ" "$REPORT" <<'PY'
import json, os, sys
proj, report = sys.argv[1], sys.argv[2]
data = json.load(open(os.path.join(report, "coverage.json")))
out, total, dead = [], 0, 0
for f in data["files"]:
    rel = os.path.relpath(f["file"], proj)
    lines = [ln for ln in f["lines"] if ln.get("count") is not None]
    total += len(lines)
    dead_lines = [ln["line_number"] for ln in lines if ln["count"] == 0]
    if not dead_lines:
        continue
    dead += len(dead_lines)
    # 连续行压成区间，读起来比几百个孤立行号强
    spans, start, prev = [], dead_lines[0], dead_lines[0]
    for n in dead_lines[1:]:
        if n == prev + 1:
            prev = n
            continue
        spans.append((start, prev)); start = prev = n
    spans.append((start, prev))
    txt = ",".join(f"{a}" if a == b else f"{a}-{b}" for a, b in spans)
    out.append(f"{rel}\t{len(dead_lines)} 行\t{txt}")

out.sort(key=lambda s: -int(s.split("\t")[1].split()[0]))
with open(os.path.join(report, "uncovered-core.txt"), "w") as fp:
    fp.write("# 未执行行（count=0），行号对应当前源码；格式：文件<TAB>行数<TAB>行号区间\n")
    fp.write(f"# 合计 {dead}/{total} 行未执行（{100.0*dead/total:.1f}%）\n")
    fp.write("\n".join(out) + "\n")
print(f"   未执行 {dead}/{total} 行（{100.0*dead/total:.1f}%），明细见 uncovered-core.txt")
PY

echo
echo "报告目录：$REPORT（summary.txt / index.html / uncovered-core.txt）"
grep -E "^(TOTAL|core|runtime|tools)" "$REPORT/summary.txt" | tail -6 || tail -6 "$REPORT/summary.txt"

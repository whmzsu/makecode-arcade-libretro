#!/usr/bin/env bash
# package_cartconv.sh — 把 cartconv 打成"解压即用"的 Windows 便携包
#
# 产物：dist/cartconv/cartconv/  （目录）
#       dist/cartconv/makecode-cartconv-<ver>-windows-x64.zip
#
# 包里自带 node 运行时和 pxt 编译链，玩家不需要装 node / npm / python。
#
# 用法：
#   tools/cartconv/package_cartconv.sh            # 完整打包
#   tools/cartconv/package_cartconv.sh --no-zip   # 只建目录，不压缩（调试用）

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
VM_BUILD="$PROJ/tools/vm-build"
NODE_MODULES="$VM_BUILD/node_modules"
STAGE_ROOT="$PROJ/dist/cartconv"
STAGE="$STAGE_ROOT/cartconv"
ZIG="${ZIG:-$PROJ/../common tools/zig/zig.exe}"

# node 运行时：优先用环境变量，否则找 workbuddy 自带的
NODE_EXE="${NODE_EXE:-}"
if [ -z "$NODE_EXE" ]; then
  for c in \
    "$(command -v node 2>/dev/null || true)" \
    "$HOME/.workbuddy/binaries/node/versions/"*/node.exe \
    "/c/Program Files/nodejs/node.exe"; do
    [ -n "$c" ] && [ -f "$c" ] && NODE_EXE="$c" && break
  done
fi
if [ -z "$NODE_EXE" ] || [ ! -f "$NODE_EXE" ]; then
  echo "找不到 node.exe，请用 NODE_EXE=/path/to/node.exe 指定" >&2
  exit 2
fi

# 版本号从 package.json 读。
# 注意：不能写 require('$HERE/package.json') —— Git Bash 的 /c/... 路径
# 在 Windows 版 node 里不合法，会静默失败退回默认值（曾经一直打出 1.0.0）。
PKGJSON="$HERE/package.json"
if command -v cygpath >/dev/null 2>&1; then PKGJSON="$(cygpath -w "$PKGJSON")"; fi
VER="$(node -e "console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).version)" "$PKGJSON" 2>/dev/null || true)"
if [ -z "$VER" ]; then
  echo "读不到 $HERE/package.json 的 version" >&2
  exit 2
fi

echo "== 清理 =="
# 不直接删旧目录：保留一份可以回滚，也避免动辄上万文件的批量删除。
if [ -d "$STAGE" ]; then
  BACKUP="$STAGE_ROOT/.prev-$(date +%Y%m%d-%H%M%S)"
  mv "$STAGE" "$BACKUP"
  echo "  旧目录已备份为 $(basename "$BACKUP")（确认新包可用后可删）"
fi
mkdir -p "$STAGE/runtime" "$STAGE/fakebin"

echo "== 1/6 编译拖拽入口 =="
"$ZIG" cc -target x86_64-windows-gnu -municode -O2 \
  -o "$STAGE/cartconv.exe" "$HERE/launcher.c"
ls -l "$STAGE/cartconv.exe" | awk '{printf "   cartconv.exe  %d KB\n", $5/1024}'

echo "== 2/6 主程序 =="
cp "$HERE/cartconv.js" "$HERE/cartdec.js" "$HERE/precheck.js" "$STAGE/"
cp "$HERE/fakebin/docker.cmd" "$STAGE/fakebin/" 2>/dev/null || cp "$PROJ/tools/fakebin/docker.cmd" "$STAGE/fakebin/"

echo "== 3/6 node 运行时 =="
cp "$NODE_EXE" "$STAGE/runtime/node.exe"
ls -l "$STAGE/runtime/node.exe" | awk '{printf "   node.exe      %d MB\n", $5/1024/1024}'

echo "== 4/6 pxt 编译链（按白名单复制）=="
# 只复制实测用到的顶层包（keep_modules.txt），不做"全量复制再删"。
# 后者要先落 477MB 再删掉一半，既慢又容易误删；正向挑包的好处是
# 目录里从来不会出现多余文件。
#
# 每个包内部用 tar 管道复制，顺手排掉用不到的东西：
#   *.map      调试符号，运行不需要
#   proj_*     开发时留下的 pxt 工程
#   webapp / common-docs   只有网页版用得到
mkdir -p "$STAGE/runtime/node_modules"

copy_pkg() {
  local pkg="$1"
  local src="$NODE_MODULES/$pkg"
  local dst="$STAGE/runtime/node_modules/$pkg"
  [ -d "$src" ] || return 1
  mkdir -p "$dst"
  tar -C "$src" \
      --exclude='*.map' \
      --exclude='proj_*' \
      --exclude='testproj' \
      --exclude='webapp' \
      --exclude='common-docs' \
      -cf - . 2>/dev/null | tar -C "$dst" -xf - 2>/dev/null
  return 0
}

missing_pkgs=""
while IFS= read -r pkg; do
  # 白名单可能是 CRLF（Python 在 Windows 上写文件会把 \n 转成 \r\n），
  # 行尾的 \r 会让路径判定永远为假
  pkg="${pkg%$'\r'}"
  [ -z "$pkg" ] && continue
  case "$pkg" in \#*) continue ;; esac
  copy_pkg "$pkg" || missing_pkgs="$missing_pkgs $pkg"
done < "$HERE/keep_modules.txt"

if [ -n "$missing_pkgs" ]; then
  echo "  白名单里的包在源目录中不存在:$missing_pkgs" >&2
  echo "  白名单可能过期了，见 README「依赖白名单怎么来的」" >&2
  exit 3
fi

rm -f "$STAGE"/*.pdb
echo "   $(du -sm "$STAGE/runtime/node_modules" 2>/dev/null | cut -f1) MB"

echo "== 5/6 预生成编译缓存（省掉玩家首次的 30 秒）=="
mkdir -p "$STAGE/.cache"
# 临时目录放在 dist 下而不是 /tmp：Git Bash 的 mktemp 给的是 /tmp/... ，
# 后续 rm 会被路径校验判成无效路径
TMP_CART="$STAGE_ROOT/.verify"
rm -rf "$TMP_CART" 2>/dev/null || true
mkdir -p "$TMP_CART"
cp "$PROJ/png_examples/arcade-ping.png" "$TMP_CART/t.png"
( cd "$STAGE" && "$STAGE/runtime/node.exe" "$STAGE/cartconv.js" "$TMP_CART/t.png" -o "$TMP_CART/out" -q ) \
  || { echo "预生成缓存失败" >&2; exit 3; }
rm -rf "$TMP_CART" 2>/dev/null || true
echo "   缓存 $(du -sm "$STAGE/.cache" 2>/dev/null | cut -f1) MB"

echo "== 6/6 说明文件 =="
cat > "$STAGE/README.txt" <<'EOF'
MakeCode Arcade 卡带转换器 cartconv
===================================

把 MakeCode Arcade 保存的 .png 卡带转换成 .pxt64，供掌机上的
MakeCode Arcade 核心直接运行。


最简单的用法：把 .png 卡带拖到 cartconv.exe 上
------------------------------------------------
* 拖一个：转换这一个
* 拖多个：依次全部转换
* 拖整个文件夹：递归转换里面所有 .png

转换结果和原图放在同一个目录，文件名改后缀为 .pxt64。
把这个 .pxt64 拷进掌机，在 RetroArch 里加载即可。


命令行用法
----------
    cartconv.exe game.png                 单张
    cartconv.exe a.png b.png             多张
    cartconv.exe 卡带文件夹\              整个目录
    cartconv.exe 卡带文件夹\ -o 输出\      统一输出到指定目录

选项：
    -o, --out DIR       输出目录（默认与卡带同目录）
        --force         已存在同名 .pxt64 时也重新转换
    -v, --verbose       显示编译细节
        --keep-project  保留中间工程（排查问题用）
    -q, --quiet         只输出结果
    -h, --help          帮助


常见问题
--------
Q: 提示"不是 MakeCode Arcade 卡带"？
A: 那不是 MakeCode Arcade 保存的卡带图。只有游戏里用
   "保存为 PNG" 导出的图才带源码数据。

Q: 转换很慢？
A: 单张约 5~10 秒，因为卡带里存的是源码，每次都要现场编译。
   批量转换时前面几张会慢一点，之后稳定在 8 秒左右。

Q: 转换后的 .pxt64 有多大？
A: 通常 150~250 KB，比原图小。

Q: 需要联网吗？
A: 不需要。全部在本机完成。

Q: 需要装 Node.js / Python 吗？
A: 不需要。运行时已经打包在这个目录里。


目录说明
--------
    cartconv.exe        入口（拖拽用这个）
    cartconv.js         主程序
    cartdec.js          卡带解码
    precheck.js         编译前预检
    runtime\node.exe    内置 node 运行时
    runtime\node_modules  pxt 编译链
    .cache\             编译缓存（可删除，删了首次会慢 30 秒）
EOF

echo
echo "== 完成 =="
du -sm "$STAGE" | awk '{printf "   目录体积：%d MB\n", $1}'

if [ "${1:-}" != "--no-zip" ]; then
  ZIP="$STAGE_ROOT/makecode-cartconv-$VER-windows-x64.zip"
  rm -f "$ZIP"
  echo "   压缩中（约 2~3 分钟）..."
  python - "$STAGE_ROOT" "$ZIP" <<'PY'
import os, sys, zipfile, time
root, dst = sys.argv[1], sys.argv[2]
base = 'cartconv'
t0 = time.time()
n = 0
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for dirpath, _dirs, files in os.walk(os.path.join(root, base)):
        for f in files:
            p = os.path.join(dirpath, f)
            z.write(p, os.path.relpath(p, root))
            n += 1
print(f'   {n} 个文件，{time.time() - t0:.0f}s，%s' %
      (f'{os.path.getsize(dst) / 1048576:.1f} MB'))
PY
fi

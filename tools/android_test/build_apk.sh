#!/usr/bin/env bash
# build_apk.sh —— 把核心打成"无头回归宿主 APK"（不需要 Gradle，不需要 Java 源码）
#
#   ./build_apk.sh [--core <core.so>] [--abi arm64-v8a] [--api 21] [--out <apk>]
#
# 产物默认 out/android_test/mca-coretest-<abi>.apk，里面：
#   lib/<abi>/libmain.so      ← runner.c + native_app_glue
#   lib/<abi>/libmca_core.so  ← 被测核心（原样拷进去，名字必须是这个，runner 按名 dlopen）
#
# 依赖（Windows Git Bash 下的位置都能自动找到）：
#   NDK   $ANDROID_NDK_HOME 或 ../common tools/android-ndk-r27c
#   SDK   $ANDROID_HOME 或 ~/AppData/Local/Android/Sdk（要 build-tools + platforms/android-30）
#   JDK   命令行里有 java/keytool（apksigner 要用）

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"

ABI="arm64-v8a"
API=21
CORE=""
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --abi)  ABI="$2"; shift 2 ;;
    --api)  API="$2"; shift 2 ;;
    --core) CORE="$2"; shift 2 ;;
    --out)  OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

case "$ABI" in
  arm64-v8a)  TRIPLE=aarch64-linux-android ;;
  x86_64)     TRIPLE=x86_64-linux-android ;;
  *) echo "暂不支持 ABI: $ABI" >&2; exit 2 ;;
esac
[ -n "$CORE" ] || CORE="$PROJ/core/dist/${TRIPLE}${API}/makecode_arcade_libretro.so"
[ -n "$OUT" ]  || OUT="$PROJ/out/android_test/mca-coretest-$ABI.apk"

[ -f "$CORE" ] || { echo "找不到核心: $CORE（先跑 tools/build_target.sh ${TRIPLE}${API}）" >&2; exit 2; }

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$PROJ/../common tools/android-ndk-r27c}}"
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/AppData/Local/Android/Sdk}}"
[ -d "$NDK" ] || { echo "找不到 NDK: $NDK" >&2; exit 2; }
[ -d "$SDK" ] || { echo "找不到 SDK: $SDK" >&2; exit 2; }

PREBUILT="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"
CLANG="$PREBUILT/bin/${TRIPLE}${API}-clang"
[ -x "$CLANG" ] || CLANG="$CLANG.cmd"
[ -x "$CLANG" ] || { echo "找不到 clang 包装器: ${TRIPLE}${API}-clang" >&2; exit 2; }

ANDROID_JAR="$(ls -1 "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1)"
BT="$(ls -1d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
[ -n "$ANDROID_JAR" ] || { echo "SDK 里没有 platforms/android-*/android.jar" >&2; exit 2; }
[ -n "$BT" ] || { echo "SDK 里没有 build-tools" >&2; exit 2; }
AAPT2="$BT/aapt2"; [ -f "$AAPT2.exe" ] && AAPT2="$AAPT2.exe"
ZIPALIGN="$BT/zipalign"; [ -f "$ZIPALIGN.exe" ] && ZIPALIGN="$ZIPALIGN.exe"
APKSIGNER="$BT/apksigner"
[ -f "$APKSIGNER.bat" ] && APKSIGNER="$APKSIGNER.bat"
[ -f "$APKSIGNER" ] || APKSIGNER="$(ls -1 "$BT"/apksigner* 2>/dev/null | head -1)"

WORK="$PROJ/out/android_test/build-$ABI"
rm -rf "$WORK"
mkdir -p "$WORK/lib/$ABI"

echo "[1/5] 编译 libmain.so（runner + native_app_glue，targetSdk 28 见 AndroidManifest）"
"$CLANG" -O2 -fPIC -shared -Wall \
  -I"$PROJ/core" -I"$PROJ/tools" -I"$NDK/sources/android/native_app_glue" \
  "$HERE/runner.c" "$NDK/sources/android/native_app_glue/android_native_app_glue.c" \
  -o "$WORK/lib/$ABI/libmain.so" \
  -landroid -llog -ldl -Wl,-z,max-page-size=16384

echo "[2/5] 放入被测核心 -> libmca_core.so（$(stat -c%s "$CORE" 2>/dev/null || wc -c <"$CORE") 字节）"
cp "$CORE" "$WORK/lib/$ABI/libmca_core.so"

echo "[3/5] aapt2 link 生成未签名 APK"
"$AAPT2" link -o "$WORK/unsigned.apk" \
  -I "$ANDROID_JAR" \
  --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version 21 --target-sdk-version 28 \
  --version-code 1 --version-name 1.0

echo "[4/5] 塞入 native 库并 zipalign"
# 这台机器没有 zip 命令，用 python 的 zipfile 追加（zipalign 之后会重排对齐）
PY="$(command -v python3 || command -v python)"
[ -n "$PY" ] || { echo "需要 python3 或 python 来打包" >&2; exit 2; }
"$PY" - "$WORK/unsigned.apk" "$WORK" "$ABI" <<'PYEOF'
import os, sys, zipfile
apk, work, abi = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_DEFLATED) as z:
    for name in ('libmain.so', 'libmca_core.so'):
        p = os.path.join(work, 'lib', abi, name)
        if not os.path.isfile(p):
            sys.exit('缺文件: ' + p)
        z.write(p, 'lib/%s/%s' % (abi, name))
PYEOF
"$ZIPALIGN" -f 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"

echo "[5/5] 签名（首次会自动生成测试密钥）"
KS="$PROJ/out/android_test/mca-test.jks"
# keytool 常常不在 PATH：按 JAVA_HOME 和常见安装位置找一遍
KEYTOOL="$(command -v keytool || true)"
if [ -z "$KEYTOOL" ]; then
  for c in "${JAVA_HOME:-}/bin/keytool" \
           "/c/Program Files/Java"/*/bin/keytool.exe \
           "/c/Program Files/Eclipse Adoptium"/*/bin/keytool.exe; do
    if [ -x "$c" ] || [ -x "$c.exe" ]; then KEYTOOL="$c"; break; fi
  done
fi
[ -n "$KEYTOOL" ] || { echo "找不到 keytool（装个 JDK，或设置 JAVA_HOME）" >&2; exit 2; }
if [ ! -f "$KS" ]; then
  "$KEYTOOL" -genkeypair -keystore "$KS" -storepass android -keypass android \
    -alias mca -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=MCA Core Test, O=mca, C=CN" >/dev/null 2>&1
  echo "      已生成测试密钥: $KS（storepass/keypass 都是 android，仅测试用）"
fi
mkdir -p "$(dirname "$OUT")"
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
  --min-sdk-version 21 --out "$OUT" "$WORK/aligned.apk"
"$APKSIGNER" verify --min-sdk-version 21 "$OUT" >/dev/null

echo
echo "OK  $OUT  ($(wc -c <"$OUT") 字节)"
echo "    ABI=$ABI targetSdk=28 core=$(basename "$CORE")"

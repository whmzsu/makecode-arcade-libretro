#!/usr/bin/env python3
"""png2pxt64.py — 卡带(.png) → VM 字节码镜像(.pxt64) 全自动转换

流程：
  .png 卡带 --(cart 隐写解码)--> 工程源文件表 --(pxtc --hwvariant vm)--> binary.pxt64

依赖（本机已具备）：
  * python 3（纯标准库）
  * node + npm，且已安装 pxt-arcade（tools/vm-build/node_modules）
  * tools/vm-build/node_modules/pxt-arcade/ 下存在 pxtarget.json（target 根）

用法：
  python tools/png2pxt64.py png_examples/arcade-ping.png out/ping.pxt64
  python tools/png2pxt64.py cart.png out/game.pxt64 --name jumpy --keep

产物：
  * <out> 字节码镜像（含尾部 SRCM 源码映射；宿主需按 section 链截断后喂 VM）
  * 可选 --dump-project DIR 保留中间工程，便于复现与调试
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TARGET = os.path.join(ROOT, "tools", "vm-build", "node_modules", "pxt-arcade")
PXT_JS = os.path.join(ROOT, "tools", "vm-build", "node_modules", "pxt-core", "built", "pxt.js")
FAKEBIN = os.path.join(ROOT, "tools", "fakebin")

sys.path.insert(0, HERE)
from extract_png_meta import png_to_rgba, extract_payload, payload_to_project  # noqa: E402


def decode_cart(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"{path}: not a PNG cartridge")
    w, h, rgba = png_to_rgba(data)
    bpp, payload = extract_payload(w, h, rgba)
    _comp, obj, sources = payload_to_project(payload)
    if not sources:
        raise SystemExit(f"{path}: no source files in cartridge")
    return obj, sources, bpp


def write_project(proj_dir, sources):
    if os.path.isdir(proj_dir):
        shutil.rmtree(proj_dir)
    os.makedirs(proj_dir, exist_ok=True)
    for name, content in sources.items():
        # 卡带里的路径可能含子目录（例如 assets/）
        dst = os.path.join(proj_dir, name.replace("\\", "/"))
        parent = os.path.dirname(dst)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(dst, "w", encoding="utf8", newline="\n") as f:
            f.write(content)
    return sorted(sources)


def ensure_docker_stub(proj_dir):
    """pxt 在 --hwvariant vm 最后一步会调 docker 交叉编译原生宿主，并读取
    built/dockercross/bld/all.tgz.b64（docker 产物包）来 patch hex 信息。
    我们只要编译器产出的 binary.pxt64，所以造一个最小的占位包让它顺利通过。"""
    import base64
    import io
    import tarfile

    bld = os.path.join(proj_dir, "built", "dockercross", "bld")
    if not os.path.isdir(os.path.join(proj_dir, "built", "dockercross")):
        return False
    os.makedirs(bld, exist_ok=True)
    stub = os.path.join(bld, "all.tgz.b64")
    if os.path.isfile(stub):
        return True
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        data = b"stub\n"
        info = tarfile.TarInfo("stub.txt")
        info.size = len(data)
        tar.addfile(info, io.BytesIO(data))
    with open(stub, "w", encoding="ascii") as f:
        f.write(base64.b64encode(buf.getvalue()).decode("ascii") + "\n")
    return True


def run_pxt(args, proj_dir, env_extra=None):
    env = os.environ.copy()
    env["PXT_FORCE_LOCAL"] = "1"
    env["PXT_NODOCKER"] = "1"
    # docker 空壳：pxt 在 --hwvariant vm 的最后一步会尝试 docker 交叉编译原生宿主，
    # 我们只要编译器产物（binary.pxt64），让 docker 直接成功返回即可。
    if os.path.isdir(FAKEBIN):
        env["PATH"] = FAKEBIN + os.pathsep + env.get("PATH", "")
    if env_extra:
        env.update(env_extra)
    cmd = ["node", PXT_JS] + args
    proc = subprocess.run(cmd, cwd=proj_dir, env=env, capture_output=True, text=True,
                          encoding="utf8", errors="replace")
    return proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cart", help="输入 .png 卡带")
    ap.add_argument("out", help="输出 .pxt64 路径")
    ap.add_argument("--name", help="工程目录名（默认取卡带文件名）")
    ap.add_argument("--dump-project", metavar="DIR", help="把中间工程复制到 DIR")
    ap.add_argument("--verbose", action="store_true", help="打印 pxt 完整输出")
    args = ap.parse_args()

    if not os.path.isfile(PXT_JS):
        raise SystemExit(f"pxt core not found: {PXT_JS}\n先运行 tools/vm-build/setup_toolchain.sh")
    if not os.path.isfile(os.path.join(TARGET, "pxtarget.json")):
        raise SystemExit(f"pxt target not found: {TARGET}")

    name = args.name or os.path.splitext(os.path.basename(args.cart))[0]
    name = "".join(c if c.isalnum() or c in "-_" else "_" for c in name)

    obj, sources, bpp = decode_cart(args.cart)
    total = sum(len(v) for v in sources.values())
    print(f"[1/4] 卡带解码: bpp={bpp} 文件 {len(sources)} 个 / {total} 字符")
    for f in sorted(sources):
        print(f"        {f} ({len(sources[f])})")

    # 工程必须放在 target 目录内部：pxt CLI 靠向上查找 pxtarget.json 定位目标
    proj_dir = os.path.join(TARGET, "proj_" + name)
    files = write_project(proj_dir, sources)
    print(f"[2/4] 工程就位: {proj_dir} ({len(files)} 个文件)")

    proc = run_pxt(["install", "--hwvariant", "vm"], proj_dir)
    if proc.returncode != 0 or not os.path.isdir(os.path.join(proj_dir, "pxt_modules", "device")):
        sys.stderr.write(proc.stdout[-3000:] + "\n" + proc.stderr[-3000:])
        raise SystemExit("[2/4] pxt install 失败")
    print("[2/4] 依赖安装完成 (device + hw---vm)")

    proc = run_pxt(["build", "--hwvariant", "vm"], proj_dir)
    image = os.path.join(proj_dir, "built", "binary.pxt64")
    if not os.path.isfile(image):
        # 首次失败通常卡在 docker 产物检查：dockercross 目录此时已生成，
        # 补一个占位 bld/all.tgz.b64 再编一次（编译器产物只依赖前几步）。
        if ensure_docker_stub(proj_dir):
            print("[3/4] 补齐 docker 占位产物，重跑编译器...")
            proc = run_pxt(["build", "--hwvariant", "vm"], proj_dir)
    if args.verbose:
        sys.stderr.write(proc.stdout[-4000:])
    if not os.path.isfile(image):
        sys.stderr.write(proc.stdout[-3000:] + "\n" + proc.stderr[-3000:])
        raise SystemExit("[3/4] 编译未产出 binary.pxt64")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    shutil.copyfile(image, args.out)
    size = os.path.getsize(args.out)
    print(f"[3/4] 字节码镜像: {args.out} ({size} 字节)")

    # 顺手记录镜像里 section 链的真实长度（宿主必须截断尾部 SRCM 才能喂 VM）
    with open(args.out, "rb") as f:
        data = f.read()
    off = 0
    while off + 8 <= len(data):
        sect_size = int.from_bytes(data[off + 4:off + 8], "little")
        if sect_size == 0 or (sect_size & 7) or off + sect_size > len(data):
            break
        off += sect_size
    print(f"[4/4] 镜像 section 链: {off} 字节，尾部附加 {len(data) - off} 字节（源码映射，宿主需截断）")

    if args.dump_project:
        dst = args.dump_project
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.makedirs(os.path.dirname(os.path.abspath(dst)) or ".", exist_ok=True)
        shutil.copytree(proj_dir, dst, ignore=shutil.ignore_patterns("pxt_modules", "built"))
        print(f"      中间工程已导出: {dst}")

    meta = {
        "cart": os.path.abspath(args.cart),
        "image": os.path.abspath(args.out),
        "image_bytes": size,
        "image_section_bytes": off,
        "bpp": bpp,
        "files": {k: len(v) for k, v in sources.items()},
        "project": proj_dir,
    }
    with open(os.path.join(os.path.dirname(os.path.abspath(args.out)), name + ".pxt64.json"),
              "w", encoding="utf8") as f:
        json.dump(meta, f, indent=1, ensure_ascii=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())

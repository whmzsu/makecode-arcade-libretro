#!/usr/bin/env python3
"""prune_modules.py — 按白名单裁剪便携包里的 node_modules

白名单不需要手工维护：用 NODE_OPTIONS 劫持 Module._load，跑一次真实转换，
记录所有被 require 的模块，再取包名即可（见 package_cartconv.sh 里
--trace-deps 的说明）。

只删**顶层**包：pxt-common-packages/node_modules 这类嵌套依赖原样保留，
因为它们是各自包的私有依赖，路径解析不走顶层。

用法：
  python tools/cartconv/prune_modules.py <node_modules目录> <keep列表>
"""

import os
import shutil
import sys


def top_level_packages(root):
    """返回顶层包名列表（含 @scope/name 形式）。"""
    out = []
    for name in sorted(os.listdir(root)):
        full = os.path.join(root, name)
        if not os.path.isdir(full) or name.startswith('.'):
            continue
        if name.startswith('@'):
            for sub in sorted(os.listdir(full)):
                if os.path.isdir(os.path.join(full, sub)):
                    out.append(name + '/' + sub)
        else:
            out.append(name)
    return out


def dir_size(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    root = sys.argv[1]
    keep_file = sys.argv[2]

    with open(keep_file, encoding='utf8') as f:
        keep = {line.strip() for line in f if line.strip() and not line.startswith('#')}

    pkgs = top_level_packages(root)
    removed = 0
    freed = 0
    for pkg in pkgs:
        if pkg in keep:
            continue
        full = os.path.join(root, *pkg.split('/'))
        size = dir_size(full)
        try:
            shutil.rmtree(full)
            removed += 1
            freed += size
        except OSError as e:
            print(f'  跳过 {pkg}: {e}')

    # 清掉空的 scope 目录
    for name in os.listdir(root):
        if name.startswith('@'):
            full = os.path.join(root, name)
            if os.path.isdir(full) and not os.listdir(full):
                os.rmdir(full)

    print(f'  保留 {len(pkgs) - removed} 个包，删除 {removed} 个，释放 {freed / 1048576:.0f} MB')
    return 0


if __name__ == '__main__':
    sys.exit(main())

"""muOS 部署脚本：安装 MakeCode Arcade 核心并注册系统。

用法：把本脚本与 makecode_arcade_libretro.so / .info / *.pxt64 放进同一目录，
在设备上以 root 运行 `python3 install.py`。幂等：重复运行只是覆盖同名条目。

注意：本脚本只做"注册"（让系统出现在前端列表里）。**每台设备还要在前端 UI 里
手动分配一次核心**：选项 → 内容选项 → 命名空间 Game Engine → MakeCode Arcade。
详见 docs/muos-2026-11-05.md。
"""

import json
import os
import shutil
import sys

SYS = "MakeCode Arcade"
CORE = "makecode_arcade_libretro.so"
INFO = "makecode_arcade_libretro.info"
ENTRY = {
    "name": SYS,
    "namespace": "Game Engine",
    "catalogue": SYS,
    "lookup": 0,
    "default": "makecode-arcade",
    "friendly": ["makecode", "makecode-arcade", "makecode arcade", "pxt", "pxt64"],
    "bios": [],
    "libretro": {
        "makecode-arcade": {
            "name": SYS,
            "core": CORE,
            "bios_required": 0,
        }
    },
}
# 注意：不要加 "extra" 字段——带 extra=1 的系统（如 WASM-4）不会出现在
# muOS 前端的 Assign（分配核心）列表里。

CORE_DIR = "/opt/muos/share/core"
INFO_DIR = "/opt/muos/share/emulator/retroarch/info"
MANIFEST = "/opt/muos/share/info/manifest/core.json"
# SD 卡上的用户 manifest：一旦存在就优先于内置那份被前端读取。
# 所以两边都要写，否则重启后我们的系统会"消失"。
SD_MANIFEST = "/mnt/mmc/MUOS/info/manifest/core.json"
ASSIGN = "/mnt/mmc/MUOS/info/manifest/assign.json"
FOLDER = "/mnt/mmc/MUOS/info/name/folder.json"
ROM_DIR = "/mnt/mmc/ROMS/" + SYS
CAT_DIR = "/mnt/mmc/MUOS/info/catalogue/" + SYS
CAT_SUB = ["box", "grid", "manual", "overlay", "preview", "splash", "text"]


def backup(path):
    bak = path + ".bak-predeploy"
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print("backup:", bak)


def patch_json(path, mutate):
    backup(path)
    with open(path) as f:
        d = json.load(f)
    mutate(d)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(d, f, indent=2, ensure_ascii=False)
        f.write("\n")
    json.load(open(tmp))  # 自检
    os.replace(tmp, path)
    print("patched:", path, "systems =", len(d))


def main():
    src = os.path.dirname(os.path.abspath(__file__))
    for name, dst_dir in ((CORE, CORE_DIR), (INFO, INFO_DIR)):
        s = os.path.join(src, name)
        if not os.path.exists(s):
            sys.exit("missing source: " + s)
        d = os.path.join(dst_dir, name)
        shutil.copy2(s, d)
        os.chmod(d, 0o755 if name.endswith(".so") else 0o644)
        print("installed:", d, os.path.getsize(d), "bytes")

    def add_entry(d):
        if SYS in d:
            print("core.json: entry exists, replacing")
        d[SYS] = ENTRY

    patch_json(MANIFEST, add_entry)
    # 内置那份写完，再同步到 SD 用户 manifest（前端实际读的是这份）
    if os.path.exists(SD_MANIFEST):
        patch_json(SD_MANIFEST, add_entry)
    else:
        os.makedirs(os.path.dirname(SD_MANIFEST), exist_ok=True)
        shutil.copy2(MANIFEST, SD_MANIFEST)
        print("created:", SD_MANIFEST)

    alias = {k: SYS for k in ["makecode", "makecode arcade", "makecode-arcade", "pxt", "pxt64"]}
    patch_json(ASSIGN, lambda d: d.update(alias))
    patch_json(FOLDER, lambda d: d.update(alias))

    os.makedirs(ROM_DIR, exist_ok=True)
    for sub in CAT_SUB:
        os.makedirs(os.path.join(CAT_DIR, sub), exist_ok=True)
    print("rom dir:", ROM_DIR)
    print("catalogue:", CAT_DIR)

    carts = sorted(p for p in os.listdir(src) if p.endswith(".pxt64"))
    for c in carts:
        shutil.copy2(os.path.join(src, c), os.path.join(ROM_DIR, c))
    print("carts copied:", len(carts))

    print("OK")


if __name__ == "__main__":
    main()

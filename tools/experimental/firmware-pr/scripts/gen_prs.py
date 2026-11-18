#!/usr/bin/env python3
"""生成五份固件收录 PR 的最终文件树（out/pr-final/<n>/）。

不复用旧 patch：旧 patch 有两个会直接导致上游构建失败的问题 ——
  * knulli .mk 把 LDFLAGS 当命令行变量传，会覆盖 Makefile 里的 `LDFLAGS += -shared`；
  * VERSION 用了 tag 对象 SHA（0c97b12…），应当用 commit b4aff55…。
另外 muOS 需要预编译 zip + data/manifest 条目，arkos4clone 需要 8 个 es_systems 变体。
"""
import json
import os
import re
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FRESH = os.path.join(ROOT, "out", "pr-fresh")
OUT = os.path.join(ROOT, "out", "pr-final")
BIN = os.path.join(OUT, "bin")

COMMIT = "b4aff558fa6fa00600b140f71802d432223752f1"
SHORT = "b4aff55"
VERSION = "0.3.7"
DATE = "2026-11-09"
BUILT = "2026-11-09T15:33:00Z"
SOURCE = "https://github.com/whmzsu/makecode-arcade-libretro"
ZIP = "makecode_arcade_libretro.so.zip"
CORE = "makecode_arcade_libretro.so"
ZIP_SHA256 = "4bbbb88a31ef200ea9051b87b092e0264d45d5c71055c30a7965364236132a04"
ZIP_SIZE = 284554
ZIP_CKSUM = "434604e5"      # cksum(zip) -> %08x，muOS .index-extended 用
SO_CRC32 = "0b4565c2"       # crc32(未压缩 .so)，ArkOS 系 .index-extended 用


def read(*parts):
    with open(os.path.join(FRESH, *parts), "r", encoding="utf-8", newline="") as f:
        return f.read()


def stage(name):
    d = os.path.join(OUT, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "files"), exist_ok=True)
    return d


def put(stage_dir, rel, data):
    p = os.path.join(stage_dir, "files", rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    if isinstance(data, str):
        data = data.encode("utf-8")
    with open(p, "wb") as f:
        f.write(data)


def put_bin(stage_dir, rel, src):
    p = os.path.join(stage_dir, "files", rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    shutil.copyfile(os.path.join(BIN, src), p)


def must(cond, msg):
    if not cond:
        raise SystemExit("断言失败: " + msg)


def localkey(name):
    """上游索引是 `sort` 出的：大小写不敏感、忽略标点（locale collation）。
    用这个 key 重排原文件能逐行复现，才敢在中间插行而不搅动既有顺序。"""
    return re.sub(r"[^0-9a-z]", "", name.casefold())


# ---------------------------------------------------------------- 01 muOS
def pr_muos():
    d = stage("01-muos-extra")
    files = []

    # data/core.json：按字典序插在 "mame" 之前
    core = read("muos", "core.json")
    must('"makecode-arcade"' not in core, "core.json 已含 makecode-arcade")
    entry = (
        '  "makecode-arcade": {\n'
        f'    "source": "{SOURCE}",\n'
        f'    "branch": "{SHORT}",\n'
        '    "directory": "makecode-arcade-libretro",\n'
        '    "architectures": ["aarch64"],\n'
        f'    "output": "core/{CORE}",\n'
        '    "make": {\n'
        '      "file": "core/Makefile",\n'
        '      "args": "",\n'
        '      "target": ""\n'
        '    },\n'
        '    "symbols": 0\n'
        '  },\n'
    )
    anchor = '  "mame": {'
    must(core.count(anchor) == 1, "core.json 的 mame 锚点不唯一")
    core_new = core.replace(anchor, entry + anchor, 1)
    json.loads(core_new)  # 语法自检
    put(d, "data/core.json", core_new)
    files.append("data/core.json")

    # data/manifest/aarch64.json：按 key 排序插入
    mani_text = read("muos", "manifest-aarch64.json")
    mani = json.loads(mani_text)
    must(ZIP not in mani, "manifest 已含 makecode")
    mani[ZIP] = {
        "built": BUILT,
        "commit": SHORT,
        "core": "makecode_arcade",
        "sha256": ZIP_SHA256,
        "size": ZIP_SIZE,
        "source": SOURCE,
        "architecture": "aarch64",
    }
    # 这个文件不是排序输出（新构建的条目追加在末尾），所以只在 makecode 该在的
    # 位置插一条，绝不整体重排 —— 否则会出现几百行无意义位移。
    roundtrip = json.dumps(json.loads(mani_text), indent=2, ensure_ascii=False) + "\n"
    if roundtrip != mani_text:
        raise SystemExit("manifest 重写格式与原文件不一致，改用文本插入")
    out = {}
    inserted = False
    nk = localkey(ZIP)
    for k, v in mani.items():
        if not inserted and localkey(k) > nk:
            out[ZIP] = mani[ZIP]
            inserted = True
        out[k] = v
    must(inserted, "manifest 插入点未找到")
    put(d, "data/manifest/aarch64.json",
        json.dumps(out, indent=2, ensure_ascii=False) + "\n")
    files.append("data/manifest/aarch64.json")

    # 预编译核心
    put_bin(d, f"core/aarch64/{ZIP}", ZIP)
    files.append(f"core/aarch64/{ZIP}")

    # .index（整行排序） / .index-extended（按第 3 字段排序）
    idx = read("muos", "index")
    must(ZIP not in idx, ".index 已含条目")
    lines = [l for l in idx.splitlines() if l.strip()]
    must(lines == sorted(lines, key=localkey), ".index 原文件不满足 locale 排序，需改用插入法")
    lines = lines + [ZIP]
    put(d, "core/aarch64/.index", "\n".join(sorted(lines, key=localkey)) + "\n")
    files.append("core/aarch64/.index")

    ext = read("muos", "index-extended")
    must(ZIP not in ext, ".index-extended 已含条目")
    elines = [l for l in ext.splitlines() if l.strip()]
    must([l.split()[2] for l in elines] == sorted((l.split()[2] for l in elines), key=localkey),
         ".index-extended 原文件不满足 locale 排序")
    elines.append(f"{DATE} {ZIP_CKSUM} {ZIP}")
    elines.sort(key=lambda l: localkey(l.split()[2]))
    put(d, "core/aarch64/.index-extended", "\n".join(elines) + "\n")
    files.append("core/aarch64/.index-extended")

    # 前端清单 + 系统目录占位
    manifest = {
        "catalogue": "MakeCode Arcade",
        "lookup": 0,
        "default": "makecode_arcade",
        "friendly": ["makecode", "makecode arcade", "pxt", "pxt64"],
        "bios": [],
        "libretro": {
            "makecode_arcade": {
                "name": "MakeCode Arcade",
                "core": CORE,
                "config": "makecode_arcade",
                "bios_required": 0,
            }
        },
        "extra": 1,
        "runtime": "libretro",
    }
    put(d, "manifest/core/Game Engine/MakeCode Arcade.json",
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    files.append("manifest/core/Game Engine/MakeCode Arcade.json")
    put(d, "system/MakeCode Arcade/.gitkeep", b"")
    files.append("system/MakeCode Arcade/.gitkeep")

    return d, files


# ---------------------------------------------------------------- 02 Knulli
def pr_knulli():
    d = stage("02-knulli")
    files = []

    cfg_in = read("knulli", "Config.in")
    anchor = '    source "$BR2_EXTERNAL_KNULLI_PATH/package/cores/libretro-parallel-n64/Config.in"\n'
    must(cfg_in.count(anchor) == 1, "Config.in 锚点不唯一")
    must("libretro-makecode-arcade" not in cfg_in, "Config.in 已含本包")
    put(d, "Config.in", cfg_in.replace(
        anchor,
        anchor + '    source "$BR2_EXTERNAL_KNULLI_PATH/package/cores/libretro-makecode-arcade/Config.in"\n',
        1))
    files.append("Config.in")

    sys_cfg = read("knulli", "knulli-system.Config.in")
    anchor2 = "\tselect BR2_PACKAGE_LEXALOFFLE_PICO8\t# ALL\n"
    must(sys_cfg.count(anchor2) == 1, "knulli-system Config.in 锚点不唯一")
    must("MAKECODE_ARCADE" not in sys_cfg, "knulli-system 已含本核心")
    put(d, "package/system/knulli-system/Config.in", sys_cfg.replace(
        anchor2,
        anchor2
        + "\t# MakeCode Arcade runs PXT64 bytecode only; the official compiler has\n"
          "\t# no 32-bit output, so this is 64-bit only.\n"
          "\tselect BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE\tif BR2_aarch64 || BR2_x86_64\n",
        1))
    files.append("package/system/knulli-system/Config.in")

    es = read("knulli", "es_systems.yml")
    must("\nmakecode:" not in es, "es_systems.yml 已含 makecode")
    block = (
        "\n"
        "makecode:\n"
        "  name:       MakeCode Arcade\n"
        "  manufacturer: Microsoft\n"
        "  release: 2019\n"
        "  hardware: console\n"
        "  extensions: [pxt64]\n"
        "  emulators:\n"
        "    libretro:\n"
        "      makecode_arcade: { requireAnyOf: [BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE] }\n"
    )
    anchor3 = "\nwasm4:\n"
    must(es.count(anchor3) == 1, "es_systems.yml 的 wasm4 锚点不唯一")
    put(d, "package/emulationstation/knulli-es-system/es_systems.yml",
        es.replace(anchor3, block + anchor3, 1))
    files.append("package/emulationstation/knulli-es-system/es_systems.yml")

    put(d, "package/cores/libretro-makecode-arcade/Config.in", """config BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE
\tbool "libretro-makecode-arcade"
\tdepends on BR2_INSTALL_LIBSTDCPP
\tdepends on BR2_aarch64 || BR2_x86_64
\thelp
\t  MakeCode Arcade libretro core (unofficial).

\t  Runs Microsoft MakeCode Arcade games compiled to PXT64 bytecode.
\t  Cartridges must be .pxt64 images; the .png files the MakeCode editor
\t  produces hold source code only and cannot be run.

\t  64-bit only: the official PXT compiler emits no 32-bit bytecode and the
\t  runtime's PXT32 image patching is an unimplemented upstream TODO, so a
\t  32-bit build loads but cannot run anything.

\t  https://github.com/whmzsu/makecode-arcade-libretro

comment "libretro-makecode-arcade needs a toolchain w/ C++, 64-bit target"
\tdepends on !BR2_INSTALL_LIBSTDCPP || !(BR2_aarch64 || BR2_x86_64)
""")
    files.append("package/cores/libretro-makecode-arcade/Config.in")

    put(d, "package/cores/libretro-makecode-arcade/libretro-makecode-arcade.mk", f"""\
################################################################################
#
# libretro-makecode-arcade
#
################################################################################

LIBRETRO_MAKECODE_ARCADE_VERSION = {COMMIT}
LIBRETRO_MAKECODE_ARCADE_SITE = $(call github,whmzsu,makecode-arcade-libretro,$(LIBRETRO_MAKECODE_ARCADE_VERSION))
LIBRETRO_MAKECODE_ARCADE_LICENSE = MIT
LIBRETRO_MAKECODE_ARCADE_LICENSE_FILES = LICENSE

# The libretro core lives in core/, the PXT runtime it embeds in runtime/ —
# same tree, so the plain generic-package build is enough.
define LIBRETRO_MAKECODE_ARCADE_BUILD_CMDS
\t$(TARGET_CONFIGURE_OPTS) $(MAKE) CXX="$(TARGET_CXX)" CC="$(TARGET_CC)" \\
\t\t-C $(@D)/core -f Makefile platform=unix
endef

define LIBRETRO_MAKECODE_ARCADE_INSTALL_TARGET_CMDS
\t$(INSTALL) -D $(@D)/core/{CORE} \\
\t\t$(TARGET_DIR)/usr/lib/libretro/{CORE}
endef

$(eval $(generic-package))
""")
    files.append("package/cores/libretro-makecode-arcade/libretro-makecode-arcade.mk")

    return d, files


# ------------------------------------------------------- 03/04 ArkOS 索引
def pr_index(name, src):
    d = stage(name)
    files = []

    put_bin(d, f"aarch64/{ZIP}", ZIP)
    files.append(f"aarch64/{ZIP}")

    idx = read(src, "index")
    must(ZIP not in idx, f"{src} .index 已含条目")
    lines = [l for l in idx.splitlines() if l.strip()]
    put(d, "aarch64/.index", "\n".join(lines + [ZIP]) + "\n")   # addcore 是追加，不排序
    files.append("aarch64/.index")

    ext = read(src, "index-extended")
    must(ZIP not in ext, f"{src} .index-extended 已含条目")
    elines = [l for l in ext.splitlines() if l.strip()]
    put(d, "aarch64/.index-extended",
        "\n".join(elines + [f"{DATE} {SO_CRC32} {ZIP}"]) + "\n")
    files.append("aarch64/.index-extended")
    return d, files


# ------------------------------------------------------ 05 arkos4clone（8 变体）
def pr_a4c():
    d = stage("05-arkos4clone")
    files = []
    block = (
        "\t<system>\n"
        "\t\t<name>makecode</name>\n"
        "\t\t<fullname>MakeCode Arcade</fullname>\n"
        "\t\t<path>/roms/makecode/</path>\n"
        "\t\t<extension>.pxt64 .PXT64</extension>\n"
        "\t\t<command>sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/retroarch "
        f"-L /home/ark/.config/retroarch/cores/{CORE} %ROM%; sudo perfnorm</command>\n"
        "\t\t<platform>makecode</platform>\n"
        "\t\t<theme>makecode</theme>\n"
        "\t\t<manufacturer>Microsoft</manufacturer>\n"
        "\t\t<release>2019</release>\n"
        "\t\t<hardware>game engine</hardware>\n"
        "\t</system>\n"
    )
    anchor = "\t<system>\n\t\t<name>mame2000</name>"
    for tree in ("ArkOS", "dArkOS"):
        for suffix in (".cfg", ".cfg.sd1", ".cfg.sd2", ".cfg.dual"):
            variant = "main" if suffix == ".cfg" else suffix.split(".cfg.")[1]
            src = os.path.join(FRESH, "a4c", f"{tree}.{variant}.cfg")
            with open(src, "r", encoding="utf-8", newline="") as f:
                text = f.read()
            n = text.count(anchor)
            must(n >= 1, f"{tree}{suffix} 找不到 mame2000 锚点")
            must("<name>makecode</name>" not in text, f"{tree}{suffix} 已含 makecode")
            text = text.replace(anchor, block + anchor)
            rel = f"rootfs/{tree}/etc/emulationstation/es_systems.cfg" + \
                  ("" if suffix == ".cfg" else "." + suffix.split(".cfg.")[1])
            put(d, rel, text)
            files.append(f"{rel}  (插入 {n} 处)")

    put(d, "roms/makecode/.init", b"")
    files.append("roms/makecode/.init")

    info = subprocess.run(
        ["sed", f"s/@VERSION@/{VERSION}/", os.path.join(ROOT, "tools", "core.info.in")],
        capture_output=True, check=True).stdout
    for tree in ("ArkOS", "dArkOS"):
        base = f"rootfs/{tree}/home/ark/.config/retroarch/cores"
        put_bin(d, f"{base}/{CORE}", CORE)
        put(d, f"{base}/makecode_arcade_libretro.info", info)
        files.append(f"{base}/{CORE}")
        files.append(f"{base}/makecode_arcade_libretro.info")
    return d, files


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    report = []
    for fn in (pr_muos, pr_knulli,
               lambda: pr_index("03-arkos-retroarch-cores", "arkos"),
               lambda: pr_index("04-darkos-retroarch-cores", "darkos"),
               pr_a4c):
        d, files = fn()
        report.append((os.path.basename(d), files))
    for name, files in report:
        print(f"== {name}  ({len(files)} 个文件)")
        for f in files:
            print("   ", f)

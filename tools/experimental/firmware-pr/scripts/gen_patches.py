#!/usr/bin/env python3
"""Generate git-apply-able patches for four firmware repositories.

Reads pristine copies fetched from the upstream repos (out/pr-patches/orig and
out/pr-work) and writes unified diffs under out/pr-patches/<target>/.
All output is LF-only; the .info / .mk / .yml files in these repos are LF too.
"""
import difflib
import json
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WORK = os.path.join(ROOT, "out", "pr-work")
OUT = os.path.join(ROOT, "out", "pr-patches")

CORE_VERSION = "0.3.7"
CORE_SHA = "0c97b121211479c32b18091b9224b6578a636383"
CORE_SHA_SHORT = CORE_SHA[:7]


def read(p):
    with open(p, "r", encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def write(p, text):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def mod_patch(path, old, new, note=""):
    """Unified diff for a modified file."""
    old_lines = old.splitlines(keepends=True)
    new_lines = new.splitlines(keepends=True)
    body = "".join(
        difflib.unified_diff(
            old_lines, new_lines, fromfile="a/" + path, tofile="b/" + path, n=3
        )
    )
    if not body:
        raise SystemExit("no change produced for " + path)
    head = "diff --git a/%s b/%s\n" % (path, path)
    if note:
        head = "# %s\n" % note + head
    return head + body


def new_patch(path, content, mode="100644"):
    """Unified diff for a brand new file (git apply compatible)."""
    lines = content.splitlines(keepends=True)
    if content and not content.endswith("\n"):
        lines[-1] += "\n"
    body = "@@ -0,0 +1,%d @@\n" % len(lines) + "".join("+" + l for l in lines)
    return (
        "diff --git a/{p} b/{p}\n"
        "new file mode {m}\n"
        "index 0000000..0000000\n"
        "--- /dev/null\n"
        "+++ b/{p}\n".format(p=path, m=mode)
        + body
    )


# --------------------------------------------------------------------------- muOS
def build_muos():
    core_json = read(os.path.join(WORK, "muos-core.json"))
    anchor = '  "mame": {\n'
    assert anchor in core_json, "muos anchor missing"
    entry = (
        '  "makecode-arcade": {\n'
        '    "source": "https://github.com/whmzsu/makecode-arcade-libretro",\n'
        '    "branch": "%s",\n'
        '    "directory": "makecode-arcade-libretro",\n'
        '    "architectures": ["aarch64", "x86_64"],\n'
        '    "output": "makecode_arcade_libretro.so",\n'
        '    "make": {\n'
        '      "file": "Makefile",\n'
        '      "args": "",\n'
        '      "target": ""\n'
        "    },\n"
        '    "symbols": 0,\n'
        '    "commands": {\n'
        '      "pre-make": [\n'
        '        "cd core"\n'
        "      ]\n"
        "    }\n"
        "  },\n" % CORE_SHA_SHORT
    )
    new_core_json = core_json.replace(anchor, entry + anchor, 1)
    json.loads(new_core_json)  # must stay valid JSON

    manifest = json.dumps(
        {
            "catalogue": "MakeCode Arcade",
            "lookup": 0,
            "default": "makecode_arcade",
            "friendly": ["makecode", "makecode arcade", "pxt", "pxt64"],
            "bios": [],
            "libretro": {
                "makecode_arcade": {
                    "name": "MakeCode Arcade",
                    "core": "makecode_arcade_libretro.so",
                    "config": "makecode_arcade",
                    "bios_required": 0,
                }
            },
            "extra": 1,
            "runtime": "libretro",
        },
        indent=2,
        ensure_ascii=False,
    ) + "\n"

    patch = mod_patch("data/core.json", core_json, new_core_json)
    patch += new_patch(
        "manifest/core/Game Engine/MakeCode Arcade.json", manifest
    )
    patch += (
        "diff --git a/system/MakeCode Arcade/.gitkeep b/system/MakeCode Arcade/.gitkeep\n"
        "new file mode 100644\n"
        "index 0000000..e69de29\n"
    )
    write(os.path.join(OUT, "01-muos-extra", "0001-core-makecode-arcade.patch"), patch)


# ------------------------------------------------------------------------- Knulli
KN_CONFIG_IN = "config"


def build_knulli():
    cfg_in = read(os.path.join(WORK, "kn-Config.in"))
    anchor = (
        '    source "$BR2_EXTERNAL_KNULLI_PATH/package/cores/libretro-parallel-n64/Config.in"\n'
    )
    assert anchor in cfg_in
    new_cfg_in = cfg_in.replace(
        anchor,
        anchor
        + '    source "$BR2_EXTERNAL_KNULLI_PATH/package/cores/libretro-makecode-arcade/Config.in"\n',
        1,
    )

    sys_cfg = read(os.path.join(WORK, "kn-knulli-system-Config.in"))
    sys_anchor = "\tselect BR2_PACKAGE_LEXALOFFLE_PICO8\t# ALL\n"
    assert sys_anchor in sys_cfg
    sel = (
        "\t# MakeCode Arcade is 64-bit only (PXT has no 32-bit bytecode output)\n"
        "\tselect BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE\tif BR2_aarch64 || BR2_x86_64\n"
    )
    new_sys_cfg = sys_cfg.replace(sys_anchor, sys_anchor + sel, 1)

    es = read(os.path.join(WORK, "kn-es_systems.yml"))
    es_anchor = "\nwasm4:\n"
    assert es_anchor in es
    block = """
makecode:
  name:       MakeCode Arcade
  manufacturer: Microsoft
  release: 2019
  hardware: console
  extensions: [pxt64]
  emulators:
    libretro:
      makecode_arcade: { requireAnyOf: [BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE] }

"""
    new_es = es.replace(es_anchor, block + "\nwasm4:\n", 1)

    cfg_in_pkg = """config BR2_PACKAGE_LIBRETRO_MAKECODE_ARCADE
\tbool "libretro-makecode-arcade"
\tdepends on BR2_PACKAGE_RETROARCH
\tdepends on BR2_INSTALL_LIBSTDCPP
\tdepends on BR2_aarch64 || BR2_x86_64
\thelp
\t  MakeCode Arcade libretro core (unofficial).

\t  Runs Microsoft MakeCode Arcade games compiled to PXT64 bytecode.
\t  Cartridges must be .pxt64 images; the .png files produced by the
\t  MakeCode editor contain source only and cannot be run.

\t  64-bit only: the official PXT compiler has no 32-bit output and the
\t  runtime's PXT32 image patching is an unimplemented upstream TODO, so
\t  32-bit builds load but cannot run anything.

\t  https://github.com/whmzsu/makecode-arcade-libretro

comment "libretro-makecode-arcade needs a toolchain w/ C++, 64-bit target"
\tdepends on !BR2_INSTALL_LIBSTDCPP || !(BR2_aarch64 || BR2_x86_64)
"""

    mk = """################################################################################
#
# libretro-makecode-arcade
#
################################################################################

LIBRETRO_MAKECODE_ARCADE_VERSION = %s
LIBRETRO_MAKECODE_ARCADE_SITE = $(call github,whmzsu,makecode-arcade-libretro,$(LIBRETRO_MAKECODE_ARCADE_VERSION))
LIBRETRO_MAKECODE_ARCADE_LICENSE = MIT
LIBRETRO_MAKECODE_ARCADE_LICENSE_FILES = LICENSE

# The core lives in core/, the PXT runtime it embeds in runtime/. Both are part
# of the same tree, so a plain generic-package build is enough.
define LIBRETRO_MAKECODE_ARCADE_BUILD_CMDS
\t$(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D)/core platform=unix \\
\t\tCC="$(TARGET_CC)" CXX="$(TARGET_CXX)" AR="$(TARGET_AR)" \\
\t\tLDFLAGS="$(TARGET_LDFLAGS) -static-libstdc++"
endef

define LIBRETRO_MAKECODE_ARCADE_INSTALL_TARGET_CMDS
\t$(INSTALL) -D $(@D)/core/makecode_arcade_libretro.so \\
\t\t$(TARGET_DIR)/usr/lib/libretro/makecode_arcade_libretro.so
endef

$(eval $(generic-package))
""" % CORE_SHA

    patch = mod_patch("Config.in", cfg_in, new_cfg_in)
    patch += mod_patch(
        "package/system/knulli-system/Config.in", sys_cfg, new_sys_cfg
    )
    patch += mod_patch("package/emulationstation/knulli-es-system/es_systems.yml", es, new_es)
    patch += new_patch("package/cores/libretro-makecode-arcade/Config.in", cfg_in_pkg)
    patch += new_patch(
        "package/cores/libretro-makecode-arcade/libretro-makecode-arcade.mk", mk
    )
    write(os.path.join(OUT, "02-knulli", "0001-libretro-makecode-arcade.patch"), patch)


# ------------------------------------------------------------------- ArkOS 系索引
def build_index_patch(tag, target_dir, label):
    idx = read(os.path.join(OUT, "orig", tag, "aarch64-.index"))
    idxe = read(os.path.join(OUT, "orig", tag, "aarch64-.index-extended"))
    name = "makecode_arcade_libretro.so.zip"
    assert name not in idx and name not in idxe, "core already indexed in " + tag

    new_idx = idx if idx.endswith("\n") else idx + "\n"
    new_idx += name + "\n"
    new_idxe = idxe if idxe.endswith("\n") else idxe + "\n"
    # crc32 is taken over the uncompressed .so, exactly like addcore does
    new_idxe += "2026-11-09 0b4565c2 " + name + "\n"

    patch = mod_patch("aarch64/.index", idx, new_idx)
    patch += mod_patch("aarch64/.index-extended", idxe, new_idxe)
    write(os.path.join(OUT, target_dir, "0001-index-makecode-arcade.patch"), patch)
    return label




# ---------------------------------------------------- arkos4clone / dArkOS4Clone
def build_arkos4clone():
    block = """\t<system>
\t\t<name>makecode</name>
\t\t<fullname>MakeCode Arcade</fullname>
\t\t<path>/roms/makecode/</path>
\t\t<extension>.pxt64 .PXT64</extension>
\t\t<command>sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/retroarch -L /home/ark/.config/retroarch/cores/makecode_arcade_libretro.so %ROM%; sudo perfnorm</command>
\t\t<platform>makecode</platform>
\t\t<theme>makecode</theme>
\t\t<manufacturer>Microsoft</manufacturer>
\t\t<release>2019</release>
\t\t<hardware>console</hardware>
\t</system>
"""
    anchor = "\t<system>\n\t\t<name>wasm-4</name>\n"
    patch = ""
    targets = [
        ("arkos", "rootfs/ArkOS/etc/emulationstation/es_systems.cfg", "a4c-es_systems.cfg"),
        ("arkos", "rootfs/ArkOS/etc/emulationstation/es_systems.cfg.sd1", "a4c-es-sd1.cfg"),
        ("darkos", "rootfs/dArkOS/etc/emulationstation/es_systems.cfg", "a4c-es_systems-darkos.cfg"),
        ("darkos", "rootfs/dArkOS/etc/emulationstation/es_systems.cfg.sd1", None),
    ]
    for tag, path, src in targets:
        if src is None:
            continue
        text = read(os.path.join(WORK, src))
        assert anchor in text, path
        patch += mod_patch(path, text, text.replace(anchor, block + anchor, 1))
    empty = "diff --git a/roms/makecode/.init b/roms/makecode/.init\nnew file mode 100644\nindex 0000000..e69de29\n"
    write(os.path.join(OUT, "05-arkos4clone-es", "0001-es-systems-makecode.patch"), patch + empty)


if __name__ == "__main__":
    build_muos()
    build_knulli()
    build_index_patch("arkos", "03-arkos-retroarch-cores", "christianhaitian/retroarch-cores")
    build_index_patch("darkos", "04-darkos-retroarch-cores", "lcdyk0517/retroarch-cores")
    build_arkos4clone()
    print("patches written under", OUT)

#!/usr/bin/env python3
"""Complete PR #43 (dArkOS): add core_info_files .info + 4 es_systems.cfg MAKECODE blocks.
Also: companion PR to christianhaitian/retroarch-cores base=rg503 (zip + 2 indexes).
"""
import base64, json, os, sys, urllib.request

T = os.environ["GITHUB_TOKEN"]
DARK_UP, DARK_ME = "christianhaitian/dArkOS", "whmzsu/dArkOS"
CORE_UP, CORE_ME = "christianhaitian/retroarch-cores", "whmzsu/retroarch-cores"
BR43 = "makecode-arcade-core"
INFO = open(r"C:\Users\whmzs\Desktop\workbuddy\makecode_arcade\out\rel038-linux\makecode_arcade_libretro.info", "rb").read()
ZIP = open(r"C:\Users\whmzs\Desktop\workbuddy\makecode_arcade\out\rel038-linux\makecode_arcade_libretro.so.zip", "rb").read()

BLOCK = (
    "\t<system>\n"
    "\t\t<name>makecode</name>\n"
    "\t\t<fullname>MakeCode Arcade</fullname>\n"
    "\t\t<path>/roms/makecode/</path>\n"
    "\t\t<extension>.pxt64 .PXT64</extension>\n"
    "\t\t<command>sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/retroarch -L /home/ark/.config/retroarch/cores/makecode_arcade_libretro.so %ROM%; sudo perfnorm</command>\n"
    "\t\t<platform>makecode</platform>\n"
    "\t\t<theme>makecode</theme>\n"
    "\t\t<manufacturer>Microsoft</manufacturer>\n"
    "\t\t<release>2019</release>\n"
    "\t\t<hardware>game engine</hardware>\n"
    "\t</system>\n"
)

def req(method, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    r = urllib.request.Request(f"https://api.github.com{path}", data=data, method=method,
        headers={"Authorization": "Bearer " + T, "Accept": "application/vnd.github+json"})
    try:
        with urllib.request.urlopen(r) as resp:
            body = resp.read()
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        print(f"{method} {path} -> {e.code}: {e.read().decode()[:300]}")
        sys.exit(1)

def get_text(repo, path, ref):
    d = req("GET", f"/repos/{repo}/contents/{path}?ref={ref}")
    return base64.b64decode(d["content"]).decode("utf-8"), d

def blob_text(repo, text):
    return req("POST", f"/repos/{repo}/git/blobs",
               {"content": text, "encoding": "utf-8"})["sha"]

def blob_bin(repo, data):
    return req("POST", f"/repos/{repo}/git/blobs",
               {"content": base64.b64encode(data).decode(), "encoding": "base64"})["sha"]

print("== part 1: complete dArkOS PR #43 ==")
master = req("GET", f"/repos/{DARK_UP}/git/ref/heads/master")["object"]["sha"]
tree_entries = []
existing = req("GET", f"/repos/{DARK_UP}/git/commits/{req('GET', f'/repos/{DARK_ME}/git/ref/heads/{BR43}')['object']['sha']}")
print("branch head:", existing["sha"][:10], existing["message"].splitlines()[0])

# idempotence: skip if part 1 already applied
r = urllib.request.Request(
    f"https://api.github.com/repos/{DARK_UP}/contents/core_info_files/makecode_arcade_libretro.info?ref={BR43}",
    headers={"Authorization": "Bearer " + T, "Accept": "application/vnd.github+json"})
try:
    with urllib.request.urlopen(r) as resp:
        json.load(resp)
    part1_done = True
    print("part 1 already applied, skipping")
except urllib.error.HTTPError as e:
    if e.code == 404:
        part1_done = False
    else:
        raise

if not part1_done:
    # 1) core_info_files/makecode_arcade_libretro.info
    tree_entries.append({"path": "core_info_files/makecode_arcade_libretro.info",
                         "mode": "100644", "type": "blob", "sha": blob_bin(DARK_ME, INFO)})

    # 2) 4 es_systems.cfg variants: insert BLOCK after the pico-8 system block
    #    (both are "game engine"; dArkOS cfg is NOT alphabetically sorted).
    for v in ["rk3326", "rk3326-64bit_Only", "rk3566", "rk3566-64bit_Only"]:
        path = f"Emulationstation/es_systems.cfg.{v}"
        text, _ = get_text(DARK_UP, path, master)
        assert "makecode" not in text, path
        nl = "\r\n" if "\r\n" in text else "\n"
        block = BLOCK.replace("\n", nl)
        i = text.index("<name>pico-8</name>")
        end = text.index("</system>", i) + len("</system>")
        line_end = text.find(nl, end)
        line_end = end if line_end == -1 else line_end + len(nl)
        new = text[:line_end] + block + text[line_end:]
        tree_entries.append({"path": path, "mode": "100644", "type": "blob",
                             "sha": blob_text(DARK_ME, new)})
        print(f"  {path}: inserted after pico-8 (newline={'CRLF' if nl=='\r\n' else 'LF'})")

    base_tree = existing["tree"]["sha"]  # parent commit's tree already has the txt line
    tree = req("POST", f"/repos/{DARK_ME}/git/trees", {"base_tree": base_tree, "tree": tree_entries})["sha"]
    commit = req("POST", f"/repos/{DARK_ME}/git/commits", {
        "message": "add .info + makecode system entries in 4 es_systems.cfg variants",
        "tree": tree, "parents": [existing["sha"]]})["sha"]
    req("PATCH", f"/repos/{DARK_ME}/git/refs/heads/{BR43}", {"sha": commit, "force": True})
    print("PR #43 branch updated:", commit[:10])

print("== part 2: rg503 branch of retroarch-cores ==")
# sync fork's rg503 with upstream, then commit on top
up_rg = req("GET", f"/repos/{CORE_UP}/git/ref/heads/rg503")["object"]["sha"]
try:
    mine = req("GET", f"/repos/{CORE_ME}/git/ref/heads/rg503")["object"]["sha"]
    if mine != up_rg:
        req("POST", f"/repos/{CORE_ME}/merge-upstream", {"branch": "rg503"})
        mine = req("GET", f"/repos/{CORE_ME}/git/ref/heads/rg503")["object"]["sha"]
except SystemExit:
    req("POST", f"/repos/{CORE_ME}/git/refs", {"ref": "refs/heads/rg503", "sha": up_rg})
    mine = up_rg
assert mine == up_rg, (mine, up_rg)

# index files on rg503 (locale-key insertion like master)
import re
def localkey(name):
    return re.sub(r"[^0-9a-z]", "", name.casefold())

entries = []
for path in ["aarch64/.index", "aarch64/.index-extended"]:
    text, _ = get_text(CORE_UP, path, up_rg)
    assert "makecode" not in text
    lines = text.split("\n")
    # rg503 branch index is append-ordered (not sorted like master) -> append at end
    while lines and lines[-1] == "":
        lines.pop()
    lines.append("makecode_arcade_libretro.so.zip")
    entries.append({"path": path, "mode": "100644", "type": "blob",
                    "sha": blob_text(CORE_ME, "\n".join(lines) + "\n")})
    print(f"  {path}: appended at end ({len(lines)} lines)")

entries.append({"path": "aarch64/makecode_arcade_libretro.so.zip", "mode": "100644",
                "type": "blob", "sha": blob_bin(CORE_ME, ZIP)})
base_tree = req("GET", f"/repos/{CORE_UP}/git/commits/{up_rg}")["tree"]["sha"]
tree = req("POST", f"/repos/{CORE_ME}/git/trees", {"base_tree": base_tree, "tree": entries})["sha"]
commit = req("POST", f"/repos/{CORE_ME}/git/commits", {
    "message": "aarch64: add makecode_arcade_libretro (rg503 branch)",
    "tree": tree, "parents": [up_rg]})["sha"]
BR_RG = "makecode-arcade-rg503"
try:
    req("POST", f"/repos/{CORE_ME}/git/refs", {"ref": f"refs/heads/{BR_RG}", "sha": commit})
except SystemExit:
    req("PATCH", f"/repos/{CORE_ME}/git/refs/heads/{BR_RG}", {"sha": commit, "force": True})

body_rg = (
    "Companion to #12 (same core, but for the **rg503 branch**).\n\n"
    "`dArkOS`'s `build_retroarch.sh` picks the core repo branch by chipset:\n"
    "rk3326 devices use `master`, while rk3566+ devices (RG353 / RG503 / RGB30 ...) "
    "download cores from the **`rg503` branch**:\n"
    "```sh\nif [ \"$CHIPSET\" == \"rk3326\" ]; then CORE_REPO=\"master\"; else CORE_REPO=\"rg503\"; fi\n```\n"
    "Without this, rk3566 devices would 404 when trying to install `makecode_arcade` "
    "(added to dArkOS's `retroarch_cores.txt` via christianhaitian/dArkOS#43).\n\n"
    "Same contents as #12: `aarch64/makecode_arcade_libretro.so.zip` + one line in "
    "`.index` / `.index-extended` (inserted in the existing locale sort order, no reordering). "
    "Core is 64-bit only (aarch64). Source: https://github.com/whmzsu/makecode-arcade-libretro (MIT)."
)
pr = req("POST", f"/repos/{CORE_UP}/pulls", {
    "title": "rg503: add makecode_arcade_libretro (companion to #12)",
    "head": f"whmzsu:{BR_RG}", "base": "rg503", "body": body_rg,
    "maintainer_can_modify": True})
print("rg503 PR:", pr["html_url"])
# update PR #43 body to reflect full contents
body43 = (
    "This makes dArkOS install the **MakeCode Arcade** libretro core, and ships everything "
    "needed for it to appear in EmulationStation. Together with "
    "christianhaitian/retroarch-cores#12 (core binaries, master branch) and its rg503-branch "
    "companion PR, the full chain is:\n\n"
    "1. **`retroarch-cores` repo** = distribution channel: "
    "`aarch64/makecode_arcade_libretro.so.zip` + `.index` / `.index-extended` "
    "(#12 for rk3326/`master`, companion PR for `rg503`).\n"
    "2. **`retroarch_cores.txt`** (this PR) = install list: `build_retroarch.sh` downloads "
    "each listed core at image build time.\n"
    "3. **`core_info_files/makecode_arcade_libretro.info`** (this PR) = core metadata; "
    "`build_retroarch.sh` falls back to this dir when the `.info` isn't in "
    "libretro's core-info repo (ours isn't, it's unofficial).\n"
    "4. **`Emulationstation/es_systems.cfg.{rk3326,rk3326-64bit_Only,rk3566,rk3566-64bit_Only}`** "
    "(this PR) = the EmulationStation menu entry. Installing a core does *not* "
    "auto-generate an ES system; without this the core is loadable only via RA's "
    "manual Load Content. The block follows the existing game-engine pattern "
    "(cf. pico-8), `.pxt64` extension, 12 lines per variant, inserted before `mame2000`.\n\n"
    "About the core: runs game ROMs built with [Microsoft MakeCode Arcade]"
    "(https://arcade.makecode.com) (`.pxt64` cart format), RGB565, 160x120 logical "
    "resolution. **64-bit only** (no 32-bit build; armhf is upstream TODO). "
    "Open source (MIT): https://github.com/whmzsu/makecode-arcade-libretro\n\n"
    "The `retroarch_cores.txt` / cfg / info changes are one line / 12 lines x4 / one new file; "
    "inserted in existing sort order, nothing reordered.\n\n"
    "Thanks for maintaining dArkOS!"
)
req("PATCH", f"/repos/{DARK_UP}/pulls/43", {"body": body43})
print("PR #43 body updated")

print("== part 3: refresh PR #12 zip to 0.3.8 ==")
head12 = req("GET", f"/repos/{CORE_ME}/git/ref/heads/add-makecode-arcade")["object"]["sha"]
c12 = req("GET", f"/repos/{CORE_ME}/git/commits/{head12}")
if "0.3.8" in c12["message"]:
    print("PR #12 already updated, skipping")
else:
    zip_blob = blob_bin(CORE_ME, ZIP)
    tree12 = req("POST", f"/repos/{CORE_ME}/git/trees", {
        "base_tree": c12["tree"]["sha"],
        "tree": [{"path": "aarch64/makecode_arcade_libretro.so.zip", "mode": "100644",
                  "type": "blob", "sha": zip_blob}]})["sha"]
    commit12 = req("POST", f"/repos/{CORE_ME}/git/commits", {
        "message": "update core to 0.3.8 (matches the published release; adds DF_1_NODELETE)",
        "tree": tree12, "parents": [head12]})["sha"]
    req("PATCH", f"/repos/{CORE_ME}/git/refs/heads/add-makecode-arcade", {"sha": commit12, "force": True})
    print("PR #12 branch updated:", commit12[:10])

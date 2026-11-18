#!/usr/bin/env python3
"""Post the dArkOS retroarch_cores.txt PR (fork -> branch -> commit -> PR)."""
import base64, json, os, sys, time, urllib.request

T = os.environ["GITHUB_TOKEN"]
UP = "christianhaitian/dArkOS"
ME = "whmzsu/dArkOS"
BRANCH = "makecode-arcade-core"

def req(method, path, payload=None, repo=UP):
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

master = req("GET", f"/repos/{UP}/git/ref/heads/master")["object"]["sha"]
mc = req("GET", f"/repos/{UP}/contents/retroarch_cores.txt?ref=" + master)
old = base64.b64decode(mc["content"]).decode()
lines = old.split("\n")
i = lines.index("lowresnx")
assert lines[i + 1] == "mame", lines[i + 1]
lines.insert(i + 1, "makecode_arcade")
new = "\n".join(lines)
assert old.split("\n") == [l for l in lines if l != "makecode_arcade"], "unexpected diff"

blob = req("POST", f"/repos/{ME}/git/blobs", {"content": new, "encoding": "utf-8"}, repo=ME)["sha"]
base_tree = req("GET", f"/repos/{UP}/git/commits/{master}")["tree"]["sha"]
tree = req("POST", f"/repos/{ME}/git/trees", {"base_tree": base_tree,
           "tree": [{"path": "retroarch_cores.txt", "mode": "100644", "type": "blob", "sha": blob}]}, repo=ME)["sha"]
commit = req("POST", f"/repos/{ME}/git/commits", {"message": "add makecode_arcade to retroarch_cores.txt",
             "tree": tree, "parents": [master]}, repo=ME)["sha"]
try:
    req("POST", f"/repos/{ME}/git/refs", {"ref": f"refs/heads/{BRANCH}", "sha": commit}, repo=ME)
except SystemExit:
    # ref may already exist from a retry; force-update instead
    req("PATCH", f"/repos/{ME}/git/refs/heads/{BRANCH}", {"sha": commit, "force": True}, repo=ME)

body = open("darkos-list-body.md", encoding="utf-8").read()
pr = req("POST", f"/repos/{UP}/pulls", {"title": "Add makecode_arcade to retroarch_cores.txt",
         "head": f"whmzsu:{BRANCH}", "base": "master", "body": body,
         "maintainer_can_modify": True})
print(pr["html_url"])

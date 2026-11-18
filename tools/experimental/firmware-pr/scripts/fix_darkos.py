#!/usr/bin/env python3
"""把 04-darkos 的 head 分支重接到 lcdyk0517/retroarch-cores:master 之上。

原因：whmzsu/retroarch-cores 是 christianhaitian 网络的 fork，GitHub 不允许同网络
再 fork 一份 darkOS 的，因此之前的分支父提交是 ArkOS 的 master，PR 里混进了两个
master 之间 43 个文件的差异。这里复用网络对象库：tree 直接以 darkOS master 的 tree
做 base_tree，父提交取 darkOS master，重建分支。
"""
import base64
import json
import os
import sys
import urllib.error
import urllib.request

TOKEN = os.environ["GITHUB_TOKEN"]
PKG = "04-darkos-retroarch-cores"
HERE = os.path.dirname(os.path.abspath(__file__))
FINAL = os.path.join(HERE, "..", "..", "out", "pr-final", PKG)
HEAD_REPO = "whmzsu/retroarch-cores"
BASE_REPO = "lcdyk0517/retroarch-cores"


def api(method, path, payload=None, expect=(200, 201, 204)):
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    req = urllib.request.Request("https://api.github.com" + path, data=data, method=method)
    req.add_header("Authorization", "Bearer " + TOKEN)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            body = r.read()
            return r.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")
        if e.code not in expect:
            raise SystemExit(f"{method} {path} -> {e.code}\n{detail[:800]}")
        return e.code, detail


def walk(root):
    out = []
    for dirpath, _, names in os.walk(root):
        for n in names:
            p = os.path.join(dirpath, n)
            out.append((os.path.relpath(p, root).replace("\\", "/"), p))
    return sorted(out)


def main():
    meta = json.load(open(os.path.join(FINAL, "meta.json"), encoding="utf-8"))
    files = walk(os.path.join(FINAL, "files"))

    _, ref = api("GET", f"/repos/{BASE_REPO}/git/ref/heads/{meta['base']}", expect=(200,))
    base_sha = ref["object"]["sha"]
    _, bc = api("GET", f"/repos/{BASE_REPO}/git/commits/{base_sha}", expect=(200,))
    base_tree = bc["tree"]["sha"]
    print(f"darkOS {meta['base']} = {base_sha[:10]}  tree {base_tree[:10]}")

    entries = []
    for rel, p in files:
        content = base64.b64encode(open(p, "rb").read()).decode("ascii")
        _, d = api("POST", f"/repos/{HEAD_REPO}/git/blobs",
                   {"content": content, "encoding": "base64"})
        entries.append({"path": rel, "mode": "100644", "type": "blob", "sha": d["sha"]})
        print(f"  blob {d['sha'][:10]}  {rel}")

    _, t = api("POST", f"/repos/{HEAD_REPO}/git/trees",
               {"base_tree": base_tree, "tree": entries})
    print(f"tree {t['sha'][:10]}")
    _, c = api("POST", f"/repos/{HEAD_REPO}/git/commits",
               {"message": meta["commit"], "tree": t["sha"], "parents": [base_sha]})
    print(f"commit {c['sha'][:10]}  parent {base_sha[:10]}")

    api("PATCH", f"/repos/{HEAD_REPO}/git/refs/heads/{meta['branch']}",
        {"sha": c["sha"], "force": True})
    print(f"分支 {meta['branch']} -> {c['sha'][:10]}（已强制更新）")


if __name__ == "__main__":
    main()

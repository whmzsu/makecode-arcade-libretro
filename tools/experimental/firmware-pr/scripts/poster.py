#!/usr/bin/env python3
"""把 out/pr-final/<pkg>/files 提交到 fork 并开 PR（不需要 clone 上 GB 的仓库）。

用法：  python poster.py [--dry-run] pkg1 pkg2 ...
每个包目录下有 meta.json：{upstream, fork, owner, base, branch, title, commit}
"""
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FINAL = os.path.join(ROOT, "out", "pr-final")
TOKEN = None  # 从环境变量 GITHUB_TOKEN 读，不落盘


def api(method, path, payload=None, expect=(200, 201, 204)):
    url = "https://api.github.com" + path
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method=method)
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


def head_of(fork, branch):
    _, d = api("GET", f"/repos/{fork}/git/ref/heads/{branch}")
    return d["object"]["sha"]


def tree_of(fork, sha):
    _, d = api("GET", f"/repos/{fork}/git/commits/{sha}")
    return d["tree"]["sha"]


def blob(fork, path, src):
    with open(src, "rb") as f:
        content = base64.b64encode(f.read()).decode("ascii")
    _, d = api("POST", f"/repos/{fork}/git/blobs", {"content": content, "encoding": "base64"})
    return d["sha"]


def walk(files_root):
    out = []
    for dirpath, _, names in os.walk(files_root):
        for n in names:
            p = os.path.join(dirpath, n)
            rel = os.path.relpath(p, files_root).replace("\\", "/")
            out.append((rel, p))
    return sorted(out)


def run(pkg, dry):
    d = os.path.join(FINAL, pkg)
    meta = json.load(open(os.path.join(d, "meta.json"), encoding="utf-8"))
    body = open(os.path.join(d, "body.md"), encoding="utf-8").read()
    files = walk(os.path.join(d, "files"))
    print(f"\n=== {pkg}  ({len(files)} 文件) -> {meta['upstream']}:{meta['base']} <- {meta['owner']}:{meta['branch']}")
    for rel, p in files:
        print(f"    {rel}  ({os.path.getsize(p)} B)")
    if dry:
        return None

    base_sha = head_of(meta["fork"], meta["base"])
    base_tree = tree_of(meta["fork"], base_sha)
    print(f"  基线 {meta['base']} = {base_sha[:8]} (tree {base_tree[:8]})")

    entries = []
    for rel, p in files:
        sha = blob(meta["fork"], rel, p)
        entries.append({"path": rel, "mode": "100644", "type": "blob", "sha": sha})
        print(f"    blob {sha[:10]}  {rel}")

    _, t = api("POST", f"/repos/{meta['fork']}/git/trees",
               {"base_tree": base_tree, "tree": entries})
    _, c = api("POST", f"/repos/{meta['fork']}/git/commits",
               {"message": meta["commit"], "tree": t["sha"], "parents": [base_sha]})
    print(f"  commit {c['sha'][:10]}")

    code, _ = api("POST", f"/repos/{meta['fork']}/git/refs",
                  {"ref": f"refs/heads/{meta['branch']}", "sha": c["sha"]}, expect=(201, 422))
    if code == 422:
        api("PATCH", f"/repos/{meta['fork']}/git/refs/heads/{meta['branch']}",
            {"sha": c["sha"], "force": True})
        print("  分支已存在，已强制更新")
    else:
        print(f"  分支 refs/heads/{meta['branch']} 已建")

    time.sleep(2)
    code, pr = api("POST", f"/repos/{meta['upstream']}/pulls",
                   {"title": meta["title"], "head": f"{meta['owner']}:{meta['branch']}",
                    "base": meta["base"], "body": body, "maintainer_can_modify": True},
                   expect=(201, 422))
    if code == 201:
        print(f"  PR #{pr['number']}  {pr['html_url']}")
        return pr["html_url"]
    print(f"  !! PR 创建失败: {pr[:400] if isinstance(pr, str) else pr}")
    return None


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--dry-run"]
    dry = "--dry-run" in sys.argv
    TOKEN = os.environ.get("GITHUB_TOKEN")
    if not TOKEN:
        raise SystemExit("需要环境变量 GITHUB_TOKEN")
    urls = []
    for pkg in args:
        u = run(pkg, dry)
        if u:
            urls.append(u)
    if urls:
        print("\n== 已创建 PR ==")
        for u in urls:
            print("  ", u)

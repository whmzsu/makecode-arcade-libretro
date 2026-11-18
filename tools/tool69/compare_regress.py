#!/usr/bin/env python3
"""比对 arm64 全语料回归结果，产出 report.md。

用法: compare_regress.py <报告目录> [帧数]
判据：
  1) ping / arcade-cube-land 的 120 帧哈希与真机 0.3.3 历史基准一致；
  2) 两个核心（真机 0.3.3 vs 工具机 zig 产物）在每张卡带上的末帧哈希一致；
  3) verdict 分布符合语料预期（Strawberry_Slam 允许 panic，其余应 ok）。
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# 真机 0.3.3 的历史基准（120 帧 / turbo / 不按键）
BASELINE = {
    "ping": "f93e4597e3503d50ea1374e2b87fba35",
    "arcade-cube-land": "23210fb3",  # 历史记录只留了前缀
}


def main() -> int:
    out = Path(sys.argv[1])
    frames = sys.argv[2] if len(sys.argv) > 2 else "600"
    lines: list[str] = []
    ok = True

    lines.append(f"# arm64 全语料逐帧回归（{frames} 帧）\n")
    lines.append(f"报告目录：`{out}`\n")

    # --- 1. 基准对齐 ---
    lines.append("## 1. 与真机 0.3.3 历史哈希对齐（120 帧）\n")
    lines.append("| 卡带 | 核心 | 末帧哈希 | 判定 |")
    lines.append("|---|---|---|---|")
    bl = out / "baseline.txt"
    if not bl.exists():
        lines.append("| - | - | baseline.txt 缺失 | FAIL |")
        ok = False
    else:
        for row in bl.read_text(encoding="utf8").splitlines():
            parts = row.split()
            if len(parts) < 4:
                lines.append(f"| - | - | {row or '空行'} | FAIL |")
                ok = False
                continue
            cart, core, h = parts[0], parts[1], parts[3]
            exp = BASELINE.get(cart)
            good = bool(exp) and h.startswith(exp)
            ok &= good
            lines.append(f"| {cart} | {core} | `{h}` | {'PASS' if good else 'FAIL'} |")
    lines.append("")

    # --- 2. 全量逐张比对 ---
    lines.append(f"## 2. 全量语料（{frames} 帧，turbo，第 60 帧按 A）\n")
    a_path = out / "core-stage033.json"
    b_path = out / "core-built.json"
    if not (a_path.exists() and b_path.exists()):
        lines.append("**FAIL**：缺少 sweep JSON，检查 sweep-*.log。\n")
        print("\n".join(lines))
        return 1
    A = {r["cart"]: r for r in json.loads(a_path.read_text(encoding="utf8"))}
    B = {r["cart"]: r for r in json.loads(b_path.read_text(encoding="utf8"))}

    lines.append("| 卡带 | 判定 | 帧 | 独立画面 | 非黑 | 末帧哈希(真机) | 双核心一致 |")
    lines.append("|---|---|---:|---:|---:|---|---|")
    verdicts: dict[str, int] = {}
    anomalies: list[str] = []
    for cart in sorted(set(A) | set(B)):
        a, b = A.get(cart), B.get(cart)
        if not a or not b:
            lines.append(f"| {cart} | 缺失 | - | - | - | - | FAIL |")
            ok = False
            continue
        same = a.get("hash") == b.get("hash") and a.get("hash") is not None
        ok &= same
        v = a["verdict"]
        verdicts[v] = verdicts.get(v, 0) + 1
        if v != "ok" and cart != "Strawberry_Slam.pxt64":
            anomalies.append(f"{cart}: {v} {a.get('note', '')}")
        h = (a.get("hash") or "-")[:12]
        lines.append(f"| {cart} | {v} | {a['frames']} | {a['distinct']} | "
                     f"{a['nonblack_pct']}% | `{h}` | {'OK' if same else '**FAIL**'} |")
    lines.append("")
    lines.append("判定分布：" + "，".join(f"{k} {v}" for k, v in sorted(verdicts.items())) + "。")
    if anomalies:
        ok = False
        lines.append("\n**非预期异常**：\n")
        lines += [f"- {x}" for x in anomalies]
    else:
        lines.append("\n非预期异常：无（Strawberry_Slam 的 panic 属已知游戏自身 bug，单独计）。")

    lines.append(f"\n## 结论：{'PASS' if ok else 'FAIL'}\n")
    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

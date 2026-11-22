#!/usr/bin/env python3
"""Compare two release asset directories by zip bytes and zip member hashes."""
import hashlib
import sys
import zipfile
from pathlib import Path


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def members(path: Path) -> dict:
    with zipfile.ZipFile(path) as z:
        return {
            i.filename: digest_bytes(z.read(i))
            for i in z.infolist()
            if not i.is_dir()
        }


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: compare_release_assets.py <local-dir> <ci-dir>", file=sys.stderr)
        return 2
    local, ci = Path(sys.argv[1]), Path(sys.argv[2])
    lnames = {p.name for p in local.glob("*.zip")}
    cnames = {p.name for p in ci.glob("*.zip")}
    common = sorted(lnames & cnames)

    print(f"local zips: {len(lnames)}  ci zips: {len(cnames)}  common: {len(common)}")
    for name in sorted(lnames - cnames):
        print(f"  local-only: {name}")
    for name in sorted(cnames - lnames):
        print(f"  ci-only:    {name}")

    byte_match = content_match = 0
    for name in common:
        lp, cp = local / name, ci / name
        same_bytes = digest_bytes(lp.read_bytes()) == digest_bytes(cp.read_bytes())
        same_members = members(lp) == members(cp)
        byte_match += int(same_bytes)
        content_match += int(same_members)
        if not same_members:
            lm, cm = members(lp), members(cp)
            detail = []
            for k in sorted(set(lm) | set(cm)):
                if lm.get(k) != cm.get(k):
                    detail.append(k)
            print(f"  MISMATCH {name}: members={','.join(detail)}")
        elif not same_bytes:
            print(f"  CONTENT-OK {name}: zip bytes differ (timestamps/compression)")

    print(f"summary: byte-identical={byte_match}/{len(common)} content-identical={content_match}/{len(common)}")
    return 0 if content_match == len(common) else 1


if __name__ == "__main__":
    raise SystemExit(main())

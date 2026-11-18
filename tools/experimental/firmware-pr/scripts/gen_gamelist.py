#!/usr/bin/env python3
# Generate an EmulationStation gamelist.xml for the MakeCode Arcade system.
#
# Usage:
#   python3 gen_gamelist.py [CARTPATH]
#   (default CARTPATH = current directory)
#
# For every *.pxt64 in CARTPATH it emits a <game> entry. The display name is
# taken from index.json / build_index.json titles when available, otherwise
# derived from the filename (underscores -> spaces). Output: <CARTPATH>/gamelist.xml
#
# This runs ON the device (or anywhere the carts live); no network needed.

import os
import sys
import json
import xml.sax.saxutils as su

EXT = ".pxt64"


def clean_name(fn):
    base = fn[:-len(EXT)] if fn.lower().endswith(EXT) else fn
    return base.replace("_", " ").strip()


def build_title_map(d):
    m = {}
    for name in ("index.json", "build_index.json"):
        p = os.path.join(d, name)
        if not os.path.exists(p):
            continue
        try:
            with open(p, encoding="utf-8") as fh:
                data = json.load(fh)
        except Exception:
            continue
        for entry in data.get("built", []):
            cart = entry.get("cart") or entry.get("pxt64")
            title = entry.get("name")
            if cart and title:
                m[cart] = title
    return m


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    tmap = build_title_map(d)
    games = sorted(f for f in os.listdir(d) if f.lower().endswith(EXT))

    out = ['<?xml version="1.0"?>', "<gameList>"]
    for f in games:
        title = tmap.get(f, clean_name(f))
        out.append("  <game>")
        out.append("    <path>%s</path>" % su.escape("./" + f))
        out.append("    <name>%s</name>" % su.escape(title))
        out.append("  </game>")
    out.append("</gameList>")

    dest = os.path.join(d, "gamelist.xml")
    with open(dest, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print("wrote %d games to %s" % (len(games), dest))
    titled = sum(1 for f in games if f in tmap)
    print("  %d had explicit titles, %d fell back to filename" % (titled, len(games) - titled))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Gzip web/ assets into <out>/<name>.gz and enforce the SP4 size cap (spec: <= 64 KB compressed total).
Usage: web_pack.py --src web --out build/web [--cap 65536] [--selftest]"""
import argparse, gzip, os, sys, tempfile

ASSETS = ("index.html", "app.js", "app.css")

def pack(src, out, cap):
    os.makedirs(out, exist_ok=True)
    total = 0
    for name in ASSETS:
        with open(os.path.join(src, name), "rb") as f:
            data = f.read()
        gz = gzip.compress(data, compresslevel=9, mtime=0)   # mtime=0 -> reproducible bytes -> stable ETag
        total += len(gz)
        with open(os.path.join(out, name + ".gz"), "wb") as f:
            f.write(gz)
        print(f"{name}: {len(data)} -> {len(gz)} bytes")
    print(f"total compressed: {total} / {cap}")
    return total <= cap

def selftest():
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "src"); os.makedirs(src)
        for n in ASSETS:
            with open(os.path.join(src, n), "w") as f: f.write("x" * 1000)
        assert pack(src, os.path.join(d, "out"), 65536) is True
        assert pack(src, os.path.join(d, "out2"), 10) is False
        print("selftest OK")

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", default="web"); ap.add_argument("--out", default="build/web")
    ap.add_argument("--cap", type=int, default=65536); ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest: selftest(); sys.exit(0)
    sys.exit(0 if pack(a.src, a.out, a.cap) else 1)

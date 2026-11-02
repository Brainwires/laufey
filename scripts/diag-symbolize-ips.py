#!/usr/bin/env python3
"""TEMPORARY (diag): symbolize a macOS .ips report's crashed and main threads
against CEF's release symbols with atos.

  scripts/diag-symbolize-ips.py <report.ips> <CEF DWARF file> <arch>
"""

import json
import subprocess
import sys

path, dwarf, arch = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8", errors="replace") as fh:
    header = fh.readline()
    report = json.loads(fh.read())
images = report.get("usedImages", [])
print(f"== {path}")
print("   ", header.strip()[:300])
print("    exception:", json.dumps(report.get("exception")))
crashed = report.get("faultingThread")
for i, t in enumerate(report.get("threads", [])):
    if i not in (0, crashed):
        continue
    label = "crashed" if i == crashed else "main"
    print(f"  thread {i} ({label}) {t.get('name', '')} {t.get('queue', '')}")
    for f in t.get("frames", [])[:80]:
        idx = f.get("imageIndex")
        image = images[idx] if idx is not None and idx < len(images) else {}
        name = image.get("name") or "???"
        off = f.get("imageOffset", 0)
        where = f.get("symbol")
        where = f"{where} + {f.get('symbolLocation', 0)}" if where else hex(off)
        if name == "Chromium Embedded Framework" and dwarf:
            base = 0x100000000  # the dSYM's own load address is irrelevant with -l
            try:
                out = subprocess.run(
                    ["atos", "-o", dwarf, "-arch", arch, "-l", hex(base),
                     hex(base + off)],
                    capture_output=True, text=True, timeout=60).stdout.strip()
                where = out or where
            except Exception as e:  # noqa: BLE001
                where += f" (atos: {e})"
        print(f"    {name:<36} {where}")

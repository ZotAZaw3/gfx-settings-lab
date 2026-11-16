#!/usr/bin/env python3
"""Fill measured-cost tables in docs/*.md from a --bench-settings CSV.

In a doc, put:
    <!-- bench: textures.quality textures.poolSizeMB -->
    <!-- /bench -->
and run:  python3 tools/fill_bench_tables.py docs/_measurements/<gpu>.csv [docs]
Everything between the two markers is replaced with a table of the measured deltas.
"""
import csv, pathlib, re, sys

csv_path = pathlib.Path(sys.argv[1])
docs = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "docs")
rows = list(csv.DictReader(open(csv_path)))
gpu = csv_path.stem.replace("_", " ").strip()
pat = re.compile(r"^<!-- bench:([^>]*)-->.*?^<!-- /bench -->", re.S | re.M)   # markers must start a line

def table(ids):
    out = [f"Measured on `{gpu}` (deltas vs the High preset; from `{csv_path.name}`):", "",
           "| Setting | Value | Δ GPU ms | Δ CPU ms | Δ frame ms | Δ VRAM MB | Δ RAM MB |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        if r["setting"] in ids:
            f = lambda k, p=3: f"{float(r[k]):+.{p}f}"
            out.append(f"| {r['label']} | {r['value']} | {f('d_gpu_ms')} | {f('d_cpu_ms')} | {f('d_frame_ms')} | {f('d_vram_mb',1)} | {f('d_ram_mb',1)} |")
    return "\n".join(out)

for md in docs.glob("*.md"):
    text = md.read_text()
    new = pat.sub(lambda m: f"<!-- bench:{m.group(1)}-->\n{table(m.group(1).split())}\n<!-- /bench -->", text)
    if new != text:
        md.write_text(new)
        print("updated", md)

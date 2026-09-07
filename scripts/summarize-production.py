"""Summarize sender --production-csv; durations in ms, no external dependencies."""
import argparse, csv, json, math
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("csv", type=Path)
p.add_argument("--output", type=Path)
a = p.parse_args()
rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(a.csv.open())]
if not rows: raise SystemExit("empty production trace")
def summary(values):
    v = sorted(values)
    return {**{f"p{q}": v[max(0, math.ceil(len(v)*q/100)-1)] for q in (50,95,99)}, "max": v[-1], "mean": sum(v)/len(v)}
stages = {"generation": ("source_end_ns", "acquisition_ns"), "source_wait_and_generation": ("source_end_ns", "source_begin_ns"), "conversion": ("conversion_end_ns", "source_end_ns"), "encode": ("encode_end_ns", "conversion_end_ns"), "dump_and_packetization": ("packetize_end_ns", "encode_end_ns"), "enqueue": ("enqueue_end_ns", "packetize_end_ns"), "nominal_lateness": ("acquisition_ns", "nominal_ns")}
result = {"frames": len(rows), "fps": len(rows)*1e9/(rows[-1]["enqueue_end_ns"]-rows[0]["source_begin_ns"]), "skipped_intervals": sum(r["skipped_intervals"] for r in rows), "encode_deadline_misses": sum(r["encode_deadline_missed"] for r in rows), "au_bytes": summary([r["au_bytes"] for r in rows]), "stages_ms": {s:summary([(r[e]-r[b])/1e6 for r in rows]) for s,(e,b) in stages.items()}}
text = json.dumps(result, indent=2)
if a.output: a.output.write_text(text + "\n")
print(text)

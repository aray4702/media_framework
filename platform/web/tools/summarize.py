# Prints run_spike.mjs output (one JSON object per line) as a short summary per scene.
import json
import sys

for line in sys.stdin:
    d = json.loads(line)
    m = d.get("metrics", {})
    print(d.get("scene"), d.get("state"), d.get("failed", ""), "events", d.get("events"))
    if d.get("environment"):
        print("  environment", d["environment"])
    print("  metrics", {k: v for k, v in m.items() if v})
    print("  frames", d.get("gpu"), "page rAF p50/p99 ms", round(d.get("rafIntervalP50Ms") or 0, 2), round(d.get("rafIntervalP99Ms") or 0, 2))
    logs = [x for x in d.get("logs", []) if "404" not in x]
    if logs:
        print("  logs", logs)

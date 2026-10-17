"""Summarise lob_bench JSON output as Markdown table rows.

Expects a run with --benchmark_repetitions=N and uses the median of the repetitions.

usage: python3 scripts/bench_summary.py <bench.json> [label]
"""

import json
import sys


def main() -> None:
    path = sys.argv[1]
    label = sys.argv[2] if len(sys.argv) > 2 else path
    with open(path) as f:
        data = json.load(f)
    runs = {
        b["run_name"].split("/")[0]: b
        for b in data["benchmarks"]
        if b.get("aggregate_name") == "median"
    }

    def pct(name: str, prefix: str = "") -> str:
        b = runs[name]
        return " / ".join(f"{b[prefix + k]:.0f}" for k in ("p50_ns", "p99_ns", "p99.9_ns"))

    ctx = data["context"]
    reps = data["benchmarks"][0].get("repetitions", 1)
    print(f"{ctx['num_cpus']} CPUs @ {ctx['mhz_per_cpu']} MHz, median of {reps} repetitions")
    print(f"Timer overhead p50: {runs['BM_TimerOverhead']['p50_ns']:.0f} ns\n")

    print("| Version | Add | Cancel | Match | Full-day replay |")
    print("|---|---:|---:|---:|---:|")
    ips = runs["BM_ReplayThroughput"]["items_per_second"]
    print(
        f"| {label} | {pct('BM_BookAdd')} | {pct('BM_BookCancel')} | {pct('BM_EngineMatch')} "
        f"| {ips / 1e6:.1f} M msg/s ({1e9 / ips:.0f} ns/msg) |"
    )

    print("\n| Version | Add | Execute | Cancel | Delete | Replace |")
    print("|---|---:|---:|---:|---:|---:|")
    cells = " | ".join(
        pct("BM_ReplayLatency", p) for p in ("add_", "exec_", "cancel_", "delete_", "replace_")
    )
    print(f"| {label} | {cells} |")


if __name__ == "__main__":
    main()

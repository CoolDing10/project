#!/usr/bin/env python3
"""Summarize one or more adaptive_sender CSV files using only Python stdlib."""

import argparse
import csv
import math
from pathlib import Path
from typing import Dict, Iterable, List


REQUIRED = {
    "interval_ms",
    "throughput_bps",
    "loss",
    "stall_events",
    "e2e_ms",
    "bitrate_bps",
    "compute_us",
}


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * fraction
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def mean(values: List[float]) -> float:
    return sum(values) / len(values) if values else math.nan


def read_metrics(path: Path) -> Dict[str, float]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        missing = REQUIRED.difference(reader.fieldnames or [])
        if missing:
            raise ValueError(f"missing columns: {', '.join(sorted(missing))}")
        rows = list(reader)
    if not rows:
        raise ValueError("contains no metric samples")

    def column(name: str) -> List[float]:
        return [float(row[name]) for row in rows]

    interval_ms = column("interval_ms")
    duration_minutes = sum(interval_ms) / 60000.0
    stalls = sum(column("stall_events"))
    compute = column("compute_us")
    e2e = column("e2e_ms")
    return {
        "samples": float(len(rows)),
        "duration_s": sum(interval_ms) / 1000.0,
        "throughput_kbps": mean(column("throughput_bps")) / 1000.0,
        "bitrate_kbps": mean(column("bitrate_bps")) / 1000.0,
        "mean_loss_pct": mean(column("loss")) * 100.0,
        "stall_events": stalls,
        "stall_events_per_min": stalls / duration_minutes if duration_minutes else math.nan,
        "e2e_mean_ms": mean(e2e),
        "e2e_p95_ms": percentile(e2e, 0.95),
        "compute_p50_us": percentile(compute, 0.50),
        "compute_p95_us": percentile(compute, 0.95),
        "compute_p99_us": percentile(compute, 0.99),
        "compute_max_us": max(compute),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Summarize adaptive video metric CSV files")
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--min-duration-s", type=float)
    parser.add_argument("--max-e2e-p95-ms", type=float)
    parser.add_argument("--max-control-p95-us", type=float)
    parser.add_argument("--max-stalls-per-min", type=float)
    args = parser.parse_args()
    headers = [
        "file", "seconds", "kbps(rx/target)", "loss%", "stalls/min",
        "e2e ms(mean/p95)", "control us(p50/p95/p99/max)",
    ]
    print(" | ".join(headers))
    print(" | ".join(["---"] * len(headers)))
    failed = False
    for path in args.csv:
        try:
            result = read_metrics(path)
        except (OSError, ValueError) as error:
            print(f"{path}: {error}", file=__import__("sys").stderr)
            failed = True
            continue
        print(" | ".join([
            path.name,
            f"{result['duration_s']:.1f}",
            f"{result['throughput_kbps']:.0f}/{result['bitrate_kbps']:.0f}",
            f"{result['mean_loss_pct']:.2f}",
            f"{result['stall_events_per_min']:.2f}",
            f"{result['e2e_mean_ms']:.1f}/{result['e2e_p95_ms']:.1f}",
            (f"{result['compute_p50_us']:.0f}/{result['compute_p95_us']:.0f}/"
             f"{result['compute_p99_us']:.0f}/{result['compute_max_us']:.0f}"),
        ]))
        violations = []
        if (args.min_duration_s is not None and
                result["duration_s"] < args.min_duration_s):
            violations.append(
                f"duration {result['duration_s']:.1f}s < {args.min_duration_s:.1f}s")
        if (args.max_e2e_p95_ms is not None and
                result["e2e_p95_ms"] > args.max_e2e_p95_ms):
            violations.append(
                f"e2e p95 {result['e2e_p95_ms']:.1f}ms > {args.max_e2e_p95_ms:.1f}ms")
        if (args.max_control_p95_us is not None and
                result["compute_p95_us"] > args.max_control_p95_us):
            violations.append(
                f"control p95 {result['compute_p95_us']:.0f}us > "
                f"{args.max_control_p95_us:.0f}us")
        if (args.max_stalls_per_min is not None and
                result["stall_events_per_min"] > args.max_stalls_per_min):
            violations.append(
                f"stalls {result['stall_events_per_min']:.2f}/min > "
                f"{args.max_stalls_per_min:.2f}/min")
        if violations:
            print(f"FAIL {path}: {'; '.join(violations)}", file=__import__("sys").stderr)
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())


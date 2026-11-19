#!/usr/bin/env python3
"""Parse multiclient log → steady-state MOPS + latency percentiles (µs)."""
from __future__ import annotations

import argparse
import re
import statistics
import sys
from pathlib import Path

RECV_RE = re.compile(r"recv/s=([0-9.+-eE]+)")
# Rank 0 latency percentiles (ns): p50=... p90=... p99=...
LAT_RE = re.compile(
    r"latency percentiles \(ns\):\s*"
    r"p50=(\d+)\s+p90=(\d+)\s+p99=(\d+)",
    re.IGNORECASE,
)


def first_plateau(rates: list[float], frac: float = 0.50) -> list[float]:
    """Keep the first high-throughput plateau; stop once rate falls below frac*peak."""
    if not rates:
        return []
    peak = max(rates)
    if peak <= 0:
        return []
    floor = max(peak * frac, 1e3)
    plateau: list[float] = []
    started = False
    for r in rates:
        if r >= floor:
            plateau.append(r)
            started = True
        elif started:
            break
    return plateau if plateau else [peak]


def parse(path: Path, trim_head: int, trim_tail: int) -> tuple[float, float, float, float]:
    text = path.read_text(errors="replace")
    rates = [float(m.group(1)) for m in RECV_RE.finditer(text)]
    if not rates:
        raise SystemExit(f"no recv/s samples in {path}")

    # Detect the healthy plateau on the FULL series first. Trimming before this
    # drops the only good seconds of short WRITE bursts that then collapse.
    plateau = first_plateau(rates, frac=0.50)
    if not plateau:
        raise SystemExit(f"throughput collapse in {path}: empty plateau")

    # Light trim only inside a long plateau (not across the collapse boundary).
    th, tt = trim_head, trim_tail
    if len(plateau) < 8:
        th = min(th, 1)
        tt = 0
    if len(plateau) < 4:
        th = 0
        tt = 0
    if len(plateau) > th + tt:
        plateau = plateau[th : len(plateau) - tt if tt else None]

    mops = statistics.median(plateau) / 1e6
    if mops < 0.05:
        raise SystemExit(f"throughput too low in {path}: mops={mops:.6f}")

    p50 = p90 = p99 = float("nan")
    m = LAT_RE.search(text)
    if m:
        p50 = int(m.group(1)) / 1e3  # ns → µs
        p90 = int(m.group(2)) / 1e3
        p99 = int(m.group(3)) / 1e3

    # Detect "sent a lot, got almost nothing" (e.g. UDP port mismatch).
    sent_vals = [int(x) for x in re.findall(r"\bsent=(\d+)", text)]
    recv_vals = [int(x) for x in re.findall(r"\brecv=(\d+)", text)]
    if sent_vals and recv_vals:
        sent = max(sent_vals)
        recv = max(recv_vals)
        if sent >= 1000 and recv < max(10, sent // 100):
            raise SystemExit(
                f"throughput collapse in {path}: sent={sent} recv={recv} "
                f"(likely server/client UDP port or reader-count mismatch)"
            )

    return mops, p50, p90, p99


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--trim-head", type=int, default=3)
    ap.add_argument("--trim-tail", type=int, default=2)
    ap.add_argument("--csv", action="store_true", help="print mops,p50,p90,p99")
    args = ap.parse_args()
    mops, p50, p90, p99 = parse(Path(args.log), args.trim_head, args.trim_tail)
    if args.csv:
        print(f"{mops:.6f},{p50:.3f},{p90:.3f},{p99:.3f}")
    else:
        # default one-line for bash capture
        print(f"{mops:.6f} {p50:.3f} {p90:.3f} {p99:.3f}")


if __name__ == "__main__":
    main()

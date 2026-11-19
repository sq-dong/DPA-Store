#!/usr/bin/env python3
"""Build paper-repro traces from artifact bulk + read traces.

Outputs under --out-dir (default: scripts/paper_repro/out/traces):
  get.trace          — GET keys (from artifact read.trace, verified ⊂ bulk)
  update.trace       — UPDATE/INSERT-op on existing keys (same keys as get)
  insert.trace       — brand-new keys (not in bulk) for INSERT-only / YCSB-D
  zipf_get.trace     — Zipf(α) over get keys (paper α=0.99)
  range.trace        — RANGE 10-key scans over get keys
"""
from __future__ import annotations

import argparse
import math
import random
from pathlib import Path


def load_keys_from_trace(path: Path) -> list[int]:
    keys: list[int] = []
    with path.open() as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] in ("GET", "READ", "INSERT", "UPDATE", "PUT", "RANGE", "SCAN"):
                keys.append(int(parts[1]))
            else:
                # bare key
                try:
                    keys.append(int(parts[0]))
                except ValueError:
                    continue
    return keys


def load_bulk_key_set(path: Path) -> set[int]:
    s: set[int] = set()
    with path.open() as f:
        for line in f:
            parts = line.split()
            if parts and parts[0] == "INSERT":
                s.add(int(parts[1]))
            elif parts:
                try:
                    s.add(int(parts[0]))
                except ValueError:
                    pass
    return s


def zipf_ranks(n: int, alpha: float, samples: int, rng: random.Random) -> list[int]:
    """Classic Zipf over ranks 1..n (1 = hottest). Returns 0-based indices into a key list."""
    # weights ∝ 1/k^α
    weights = [1.0 / (k ** alpha) for k in range(1, n + 1)]
    total = sum(weights)
    cdf: list[float] = []
    acc = 0.0
    for w in weights:
        acc += w / total
        cdf.append(acc)

    def draw() -> int:
        u = rng.random()
        lo, hi = 0, n - 1
        while lo < hi:
            mid = (lo + hi) // 2
            if u <= cdf[mid]:
                hi = mid
            else:
                lo = mid + 1
        return lo

    return [draw() for _ in range(samples)]


def write_lines(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {path} ({len(lines)} ops)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bulk", required=True)
    ap.add_argument("--read", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--zipf-alpha", type=float, default=0.99)
    ap.add_argument("--zipf-samples", type=int, default=2_000_000)
    ap.add_argument("--insert-count", type=int, default=1_000_000)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    read_keys = load_keys_from_trace(Path(args.read))
    if not read_keys:
        raise SystemExit("empty read trace")
    print(f"read keys: {len(read_keys)}")

    bulk = load_bulk_key_set(Path(args.bulk))
    print(f"bulk keys: {len(bulk)}")
    hits = sum(1 for k in read_keys if k in bulk)
    print(f"read⊂bulk hit-rate: {hits}/{len(read_keys)} = {hits/len(read_keys):.4f}")

    # GET / UPDATE (existing keys)
    write_lines(out / "get.trace", [f"GET {k}" for k in read_keys])
    write_lines(
        out / "update.trace",
        [f"UPDATE {k} {k * 2}" for k in read_keys],
    )

    # INSERT: keys guaranteed absent from bulk
    inserts: list[int] = []
    while len(inserts) < args.insert_count:
        k = rng.getrandbits(64)
        if k not in bulk:
            inserts.append(k)
            bulk.add(k)  # avoid duplicates within insert set
    write_lines(
        out / "insert.trace",
        [f"INSERT {k} {k * 2}" for k in inserts],
    )

    # Zipf GET over read key universe
    order = list(read_keys)
    rng.shuffle(order)
    idxs = zipf_ranks(len(order), args.zipf_alpha, args.zipf_samples, rng)
    write_lines(out / "zipf_get.trace", [f"GET {order[i]}" for i in idxs])

    # RANGE length 10 (paper §4.3)
    write_lines(
        out / "range.trace",
        [f"RANGE {k} 10" for k in read_keys],
    )

    # Convenience copies with bare keys for override-op clients
    write_lines(out / "keys_existing.trace", [f"GET {k}" for k in read_keys])
    print("done")


if __name__ == "__main__":
    main()

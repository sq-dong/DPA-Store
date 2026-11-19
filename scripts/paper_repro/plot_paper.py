#!/usr/bin/env python3
"""Plot paper-aligned figures from paper_repro CSVs.

Layouts follow DPA-Store (arXiv:2601.06231) Fig.9–15 / Table 1:
  - dual Y axes (MOPS bars + latency markers)
  - hatched bars, p50/p90 overlays
Absolute MOPS will be lower than the paper on a 1-client cluster; axes
auto-scale to data while keeping the same visual structure.
"""
from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

C_BAR1 = "#4C78A8"
C_BAR2 = "#54A24B"
C_BAR3 = "#E45756"
C_P50 = "#F58518"
C_P90 = "#E45756"


def _style() -> None:
    plt.rcParams.update(
        {
            "font.size": 10,
            "axes.labelsize": 11,
            "axes.titlesize": 11,
            "legend.fontsize": 8,
            "figure.dpi": 160,
            "savefig.dpi": 160,
            "axes.grid": True,
            "grid.linestyle": ":",
            "grid.alpha": 0.4,
        }
    )


def save(fig: plt.Figure, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out.with_suffix(".png"))
    fig.savefig(out.with_suffix(".pdf"))
    plt.close(fig)
    print(f"wrote {out.with_suffix('.png')}")


def _agg(rows: list[dict], key_fields: list[str], metric_fields: list[str]) -> list[dict]:
    """Average metrics over runs for identical keys."""
    buckets: dict[tuple, list[dict]] = defaultdict(list)
    for r in rows:
        k = tuple(r[f] for f in key_fields)
        buckets[k].append(r)
    out = []
    for k, rs in buckets.items():
        item = {f: k[i] for i, f in enumerate(key_fields)}
        item["n"] = len(rs)
        for m in metric_fields:
            vals = [float(r[m]) for r in rs if r.get(m) not in ("", "nan", None)]
            vals = [v for v in vals if math.isfinite(v)]
            item[m] = float(np.mean(vals)) if vals else float("nan")
        out.append(item)
    return out


def _read(path: Path) -> list[dict]:
    if not path.is_file():
        print(f"skip missing {path}")
        return []
    with path.open() as f:
        return list(csv.DictReader(f))


def plot_fig9(data_dir: Path) -> None:
    left = _agg(_read(data_dir / "fig09_traverser.csv"), ["traversers"], ["mops", "p50_us", "p90_us"])
    right = _agg(
        _read(data_dir / "fig09_stitch_patch.csv"),
        ["stitchers", "patchers", "op"],
        ["mops", "p50_us", "p90_us"],
    )
    if not left and not right:
        return
    _style()
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 3.6))

    if left:
        left = sorted(left, key=lambda r: int(r["traversers"]))
        xs = [int(r["traversers"]) for r in left]
        xpos = np.arange(len(xs))
        ax = axes[0]
        ax.bar(xpos, [r["mops"] for r in left], color=C_BAR1, hatch="//", edgecolor="k", width=0.7, label="throughput")
        ax.set_xticks(xpos)
        ax.set_xticklabels([str(x) for x in xs], rotation=45)
        ax.set_xlabel("# traverser threads")
        ax.set_ylabel("Throughput (MOPS)")
        ax.set_ylim(bottom=0)
        ax2 = ax.twinx()
        ax2.plot(xpos, [r["p50_us"] for r in left], "o-", color=C_P50, label="lat p50")
        ax2.plot(xpos, [r["p90_us"] for r in left], "x-", color=C_P90, label="lat p90")
        ax2.set_ylabel("Latency (µs)")
        ax.set_title("Fig. 9 left: traverser scaling (GET)")
        h1, l1 = ax.get_legend_handles_labels()
        h2, l2 = ax2.get_legend_handles_labels()
        ax.legend(h1 + h2, l1 + l2, loc="upper left", frameon=False)

    if right:
        # group by op; x labels like 1s/4p
        updates = [r for r in right if r["op"] == "update"]
        inserts = [r for r in right if r["op"] == "insert"]

        def order_key(r):
            return (int(r["stitchers"]), int(r["patchers"]))

        updates = sorted(updates, key=order_key)
        inserts = sorted(inserts, key=order_key)
        labels = [f"{r['stitchers']}s/{r['patchers']}p" for r in (updates or inserts)]
        ax = axes[1]
        n = len(labels)
        x = np.arange(n)
        w = 0.35
        if updates:
            ax.bar(x - w / 2, [r["mops"] for r in updates], width=w, color=C_BAR1, hatch="//", edgecolor="k", label="UPDATE MOPS")
        if inserts:
            ax.bar(x + w / 2, [r["mops"] for r in inserts], width=w, color=C_BAR2, hatch="xx", edgecolor="k", label="INSERT MOPS")
        ax.set_xticks(x)
        ax.set_xticklabels(labels, rotation=30)
        ax.set_ylabel("Throughput (MOPS)")
        ax.set_xlabel("stitcher / patcher")
        ax.set_ylim(bottom=0)
        ax.set_title("Fig. 9 right: stitcher/patcher (176 trav)")

        # Latency overlays (same dual-Y style as left panel).
        ax2 = ax.twinx()
        if updates:
            ax2.plot(x, [r["p50_us"] for r in updates], "o-", color=C_P50, label="UPDATE lat p50")
            ax2.plot(x, [r["p90_us"] for r in updates], "x-", color=C_P90, label="UPDATE lat p90")
        if inserts:
            ax2.plot(x, [r["p50_us"] for r in inserts], "s--", color=C_P50, alpha=0.85, label="INSERT lat p50")
            ax2.plot(x, [r["p90_us"] for r in inserts], "+--", color=C_P90, alpha=0.85, label="INSERT lat p90")
        ax2.set_ylabel("Latency (µs)")
        h1, l1 = ax.get_legend_handles_labels()
        h2, l2 = ax2.get_legend_handles_labels()
        ax.legend(h1 + h2, l1 + l2, loc="upper left", fontsize=7, frameon=False, ncol=2)

    save(fig, data_dir / "fig09_thread_scaling")


def plot_fig10(data_dir: Path) -> None:
    raw = _read(data_dir / "fig10_queue_depth.csv")
    if not raw:
        return
    # Prefer paper_queue_depth (scaled-load runs); fall back to queue_depth.
    key = "paper_queue_depth" if "paper_queue_depth" in raw[0] else "queue_depth"
    rows = _agg(raw, [key], ["mops", "p50_us", "p90_us"])
    rows = sorted(rows, key=lambda r: int(float(r[key])))
    _style()
    fig, ax = plt.subplots(figsize=(6.2, 3.6))
    xs = [int(float(r[key])) for r in rows]
    xpos = np.arange(len(xs))
    ax.bar(xpos, [r["mops"] for r in rows], color=C_BAR1, hatch="//", edgecolor="k", width=0.7, label="throughput")
    ax.set_xticks(xpos)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlabel("Client queue depth (paper-equivalent)")
    ax.set_ylabel("Throughput (MOPS)")
    ax.set_ylim(bottom=0)
    ax2 = ax.twinx()
    ax2.plot(xpos, [r["p50_us"] for r in rows], "o-", color=C_P50, label="lat p50")
    ax2.plot(xpos, [r["p90_us"] for r in rows], "x-", color=C_P90, label="lat p90")
    ax2.set_ylabel("Latency (µs)")
    ax.set_title("Fig. 10: GET vs client queue depth")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", frameon=False)
    save(fig, data_dir / "fig10_queue_depth")


def plot_fig11(data_dir: Path) -> None:
    rows = _agg(
        _read(data_dir / "fig11_get_datasets.csv"),
        ["dataset", "depth_label", "eps_label", "dist", "hot_cache"],
        ["mops", "p50_us", "p90_us"],
    )
    if not rows:
        return
    # one group per dataset+eps label; two bars uniform/zipf
    labels = []
    seen = set()
    for r in rows:
        lab = f"{r['dataset']} {r['depth_label']} {r['eps_label']}"
        if lab not in seen:
            seen.add(lab)
            labels.append(lab)
    _style()
    fig, ax = plt.subplots(figsize=(max(7.0, 1.2 * len(labels)), 3.8))
    x = np.arange(len(labels))
    w = 0.35
    uni = []
    zf = []
    p50u = []
    p90u = []
    for lab in labels:
        ds, depth, eps = lab.split()[0], lab.split()[1], lab.split()[2]
        def find(dist):
            for r in rows:
                if r["dataset"] == ds and r["depth_label"] == depth and r["eps_label"] == eps and r["dist"] == dist and str(r["hot_cache"]) in ("1", "1.0"):
                    return r
            return None
        u = find("uniform")
        z = find("zipf")
        uni.append(u["mops"] if u else 0.0)
        zf.append(z["mops"] if z else 0.0)
        p50u.append(u["p50_us"] if u else float("nan"))
        p90u.append(u["p90_us"] if u else float("nan"))
    ax.bar(x - w / 2, uni, width=w, color=C_BAR1, hatch="//", edgecolor="k", label="uniform")
    ax.bar(x + w / 2, zf, width=w, color=C_BAR2, hatch="xx", edgecolor="k", label="zipf")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=25, ha="right")
    ax.set_ylabel("Throughput (MOPS)")
    ax.set_ylim(bottom=0)
    ax2 = ax.twinx()
    ax2.plot(x, p50u, "o-", color=C_P50, label="lat p50 (uniform)")
    ax2.plot(x, p90u, "x-", color=C_P90, label="lat p90 (uniform)")
    ax2.set_ylabel("Latency (µs)")
    ax.set_title("Fig. 11: GET-only (uniform vs zipf)")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper right", frameon=False)
    save(fig, data_dir / "fig11_get_datasets")


def plot_fig12(data_dir: Path) -> None:
    """Dual-Y: thin throughput bars + connected p50/p90 latency markers."""
    rows = _agg(_read(data_dir / "fig12_learned_vs_btree.csv"), ["dataset", "variant"], ["mops", "p50_us", "p90_us"])
    if not rows:
        return
    datasets = []
    for r in rows:
        if r["dataset"] not in datasets:
            datasets.append(r["dataset"])

    learned_m, btree_m = [], []
    learned_p50, btree_p50, learned_p90, btree_p90 = [], [], [], []
    for ds in datasets:
        L = next((r for r in rows if r["dataset"] == ds and r["variant"] == "learned"), None)
        B = next((r for r in rows if r["dataset"] == ds and r["variant"] == "btree"), None)
        learned_m.append(float(L["mops"]) if L else 0.0)
        btree_m.append(float(B["mops"]) if B else 0.0)
        learned_p50.append(float(L["p50_us"]) if L else float("nan"))
        btree_p50.append(float(B["p50_us"]) if B else float("nan"))
        learned_p90.append(float(L["p90_us"]) if L else float("nan"))
        btree_p90.append(float(B["p90_us"]) if B else float("nan"))

    _style()
    # Extra width when only one dataset so thin bars don't look oversized.
    fig_w = 5.2 if len(datasets) == 1 else max(6.0, 1.8 * len(datasets) + 1.2)
    fig, ax = plt.subplots(figsize=(fig_w, 3.8))
    x = np.arange(len(datasets), dtype=float)
    w = 0.22
    ax.bar(x - w / 2, learned_m, width=w, color=C_BAR1, hatch="//", edgecolor="k", linewidth=0.7, label="Learned")
    ax.bar(x + w / 2, btree_m, width=w, color=C_BAR2, hatch="xx", edgecolor="k", linewidth=0.7, label="B+")
    ax.set_ylabel("Throughput (MOPS)")
    ax.set_ylim(0, max(learned_m + btree_m + [1.0]) * 1.25)
    ax.set_xticks(x)
    ax.set_xticklabels(datasets)
    # Leave side padding so thin bars don't fill the frame.
    ax.set_xlim(-0.7, len(datasets) - 1 + 0.7)

    ax2 = ax.twinx()
    # Connect p50 / p90 across variants (and across datasets if >1).
    # Order on x: Learned then B+ at each dataset group.
    xs_lat = []
    ys_p50 = []
    ys_p90 = []
    for i in range(len(datasets)):
        xs_lat.extend([x[i] - w / 2, x[i] + w / 2])
        ys_p50.extend([learned_p50[i], btree_p50[i]])
        ys_p90.extend([learned_p90[i], btree_p90[i]])
    ax2.plot(xs_lat, ys_p50, "o-", color=C_P50, markersize=6, linewidth=1.2, label="lat p50", zorder=3)
    ax2.plot(xs_lat, ys_p90, "x-", color=C_P90, markersize=7, linewidth=1.2, label="lat p90", zorder=3)
    ax2.set_ylabel("Latency (µs)")
    lat_vals = [v for v in ys_p50 + ys_p90 if math.isfinite(v)]
    ax2.set_ylim(0, max(lat_vals + [1.0]) * 1.25)

    ax.set_title("Fig. 12: Learned vs B+-tree")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    # Put legend above axes so it never covers bars.
    fig.legend(
        h1 + h2,
        l1 + l2,
        loc="upper center",
        ncol=4,
        frameon=False,
        bbox_to_anchor=(0.5, 1.02),
        columnspacing=1.0,
        handletextpad=0.4,
        fontsize=8,
    )
    out = data_dir / "fig12_learned_vs_btree"
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out.with_suffix(".png"), bbox_inches="tight")
    fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out.with_suffix('.png')}")


def plot_fig13(data_dir: Path) -> None:
    rows = _agg(_read(data_dir / "fig13_insert_update.csv"), ["dataset", "mix"], ["mops", "p50_us", "p90_us"])
    if not rows:
        return
    mix_order = [
        "update_100pct",
        "update_50pct",
        "update_5pct",
        "insert_100pct",
        "insert_50pct",
        "insert_5pct",
    ]
    mix_labels = ["100% upd", "50% upd", "5% upd", "100% ins", "50% ins", "5% ins"]
    datasets = []
    for r in rows:
        if r["dataset"] not in datasets:
            datasets.append(r["dataset"])
    colors = [C_BAR1, C_BAR2, C_BAR3]
    hatches = ["//", "xx", "++"]
    _style()
    fig, (ax_t, ax_l) = plt.subplots(2, 1, figsize=(8.5, 5.5), sharex=True)
    x = np.arange(len(mix_order))
    n = max(len(datasets), 1)
    w = 0.8 / n
    for i, ds in enumerate(datasets):
        thr = []
        lat = []
        for m in mix_order:
            r = next((rr for rr in rows if rr["dataset"] == ds and rr["mix"] == m), None)
            thr.append(r["mops"] if r else 0.0)
            lat.append(r["p50_us"] if r else float("nan"))
        ax_t.bar(x + i * w - 0.4 + w / 2, thr, width=w, color=colors[i % 3], hatch=hatches[i % 3], edgecolor="k", label=ds)
        ax_l.bar(x + i * w - 0.4 + w / 2, lat, width=w, color=colors[i % 3], hatch=hatches[i % 3], edgecolor="k", label=ds)
    ax_t.set_ylabel("Throughput (MOPS)")
    ax_t.set_ylim(bottom=0)
    ax_t.set_title("Fig. 13: UPDATE / INSERT mixes")
    ax_t.legend(frameon=False)
    ax_l.set_ylabel("p50 Latency (µs)")
    ax_l.set_ylim(bottom=0)
    ax_l.set_xticks(x)
    ax_l.set_xticklabels(mix_labels)
    save(fig, data_dir / "fig13_insert_update")


def plot_fig15(data_dir: Path) -> None:
    """Paper Fig.15: scatter X=log p50, Y=throughput; color=workload."""
    rows = _agg(
        _read(data_dir / "fig15_ycsb.csv"),
        ["dataset", "system", "workload"],
        ["mops", "p50_us", "p90_us"],
    )
    if not rows:
        return

    from matplotlib.lines import Line2D
    from matplotlib.ticker import LogLocator, NullFormatter

    wl_order = ["A", "B", "C", "insert100", "D", "range100", "E", "F"]
    wl_short = {
        "A": "A",
        "B": "B",
        "C": "C",
        "D": "D",
        "E": "E",
        "F": "F",
        "insert100": "ins",
        "range100": "rng",
    }
    wl_color = {
        "A": "#4C78A8",
        "B": "#E45756",
        "C": "#54A24B",
        "insert100": "#F58518",
        "D": "#EECA3B",
        "range100": "#9D755D",
        "E": "#B279A2",
        "F": "#79706E",
    }
    wl_legend = {
        "A": "A",
        "B": "B",
        "C": "C",
        "insert100": "ins",
        "D": "D",
        "range100": "rng",
        "E": "E",
        "F": "F",
    }
    # Per-label offsets (pt). Cluster A/B/D/F fanned hard.
    label_xy = {
        "A": (-18, 8),
        "B": (0, 16),
        "C": (10, 8),
        "D": (12, -14),
        "E": (8, 8),
        "F": (16, 0),
        "ins": (10, -12),
        "rng": (8, 10),
    }

    ds_pref = ["sparse", "amzn", "osmc"]
    datasets = [d for d in ds_pref if any(r["dataset"] == d for r in rows)]
    for r in rows:
        if r["dataset"] not in datasets:
            datasets.append(r["dataset"])

    present_wl = [wl for wl in wl_order if any(r["workload"] == wl for r in rows)]
    has_dpa = any(r["system"] == "dpastore" for r in rows)
    has_rolex = any(r["system"] == "rolex" for r in rows)

    _style()
    n_ds = max(len(datasets), 1)
    fig, axes = plt.subplots(1, n_ds, figsize=(3.8 * n_ds + 0.3, 3.7), sharey=True)
    if n_ds == 1:
        axes = [axes]

    all_lats: list[float] = []
    all_mops: list[float] = []

    for ax, ds in zip(axes, datasets):
        by_sys: dict[str, dict[str, dict]] = {}
        for r in rows:
            if r["dataset"] != ds:
                continue
            by_sys.setdefault(r["system"], {})[r["workload"]] = r

        if "dpastore" in by_sys and "rolex" in by_sys:
            for wl in present_wl:
                a = by_sys["dpastore"].get(wl)
                b = by_sys["rolex"].get(wl)
                if not a or not b:
                    continue
                la, ma = float(a["p50_us"]), float(a["mops"])
                lb, mb = float(b["p50_us"]), float(b["mops"])
                if not (la > 0 and lb > 0 and math.isfinite(ma) and math.isfinite(mb)):
                    continue
                ax.plot(
                    [la, lb],
                    [ma, mb],
                    color=wl_color.get(wl, "#888"),
                    linewidth=0.9,
                    alpha=0.45,
                    zorder=1,
                )

        for wl in present_wl:
            color = wl_color[wl]
            short = wl_short[wl]
            a = by_sys.get("dpastore", {}).get(wl)
            if a:
                lat, mops = float(a["p50_us"]), float(a["mops"])
                if lat > 0 and math.isfinite(mops):
                    ax.scatter(
                        [lat],
                        [mops],
                        s=64,
                        color=color,
                        marker="o",
                        edgecolors="k",
                        linewidths=0.6,
                        zorder=3,
                    )
                    ox, oy = label_xy.get(short, (8, 8))
                    ax.annotate(
                        short,
                        (lat, mops),
                        textcoords="offset points",
                        xytext=(ox, oy),
                        fontsize=8,
                        color=color,
                        fontweight="bold",
                        ha="center",
                        va="center",
                        zorder=4,
                    )
                    all_lats.append(lat)
                    all_mops.append(mops)
            b = by_sys.get("rolex", {}).get(wl)
            if b:
                lat, mops = float(b["p50_us"]), float(b["mops"])
                if lat > 0 and math.isfinite(mops):
                    ax.scatter(
                        [lat],
                        [mops],
                        s=56,
                        facecolors="none",
                        edgecolors=color,
                        marker="s",
                        linewidths=1.3,
                        zorder=3,
                    )
                    all_lats.append(lat)
                    all_mops.append(mops)

        ax.set_xscale("log")
        ax.set_xlabel("p50 latency (µs)")
        if ax is axes[0]:
            ax.set_ylabel("Throughput (MOPS)")
        ax.text(0.98, 0.97, ds, transform=ax.transAxes, ha="right", va="top", fontsize=10)
        if ax is axes[0]:
            ax.annotate(
                "better",
                xy=(0.07, 0.93),
                xytext=(0.22, 0.78),
                xycoords="axes fraction",
                textcoords="axes fraction",
                fontsize=7,
                style="italic",
                color="#777777",
                arrowprops=dict(arrowstyle="->", color="#777777", lw=0.8),
            )
        ax.xaxis.set_major_locator(LogLocator(base=10))
        ax.xaxis.set_minor_locator(LogLocator(base=10, subs=np.arange(2, 10) * 0.1))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.grid(True, which="major", linestyle=":", alpha=0.45)
        ax.grid(True, which="minor", axis="x", linestyle=":", alpha=0.2)
        ax.set_axisbelow(True)

    if all_lats:
        for ax in axes:
            ax.set_xlim(min(all_lats) / 1.5, max(all_lats) * 1.8)
            ax.set_ylim(0, max(all_mops) * 1.25)

    # Compact legend: short names; circles match DPA-Store markers.
    handles = [
        Line2D(
            [0],
            [0],
            marker="o",
            color="w",
            markerfacecolor=wl_color[wl],
            markeredgecolor="k",
            markersize=7,
            label=wl_legend[wl],
        )
        for wl in present_wl
    ]
    # Only show system markers when both systems exist (paper has both).
    if has_dpa and has_rolex:
        handles.append(
            Line2D([0], [0], marker="o", color="w", markerfacecolor="#555", markeredgecolor="k", markersize=7, label="DPA-Store")
        )
        handles.append(
            Line2D([0], [0], marker="s", color="w", markerfacecolor="none", markeredgecolor="k", markersize=7, label="ROLEX")
        )

    fig.legend(
        handles=handles,
        loc="upper center",
        ncol=min(len(handles), 5),
        frameon=False,
        bbox_to_anchor=(0.5, 1.02),
        columnspacing=0.7,
        handletextpad=0.2,
        fontsize=8,
    )
    out = data_dir / "fig15_ycsb"
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out.with_suffix(".png"), bbox_inches="tight")
    fig.savefig(out.with_suffix(".pdf"), bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out.with_suffix('.png')}")


def plot_table1(data_dir: Path) -> None:
    rows = _read(data_dir / "table1_memory.csv")
    if not rows:
        return
    _style()
    fig, ax = plt.subplots(figsize=(7.5, 3.6))
    labels = [f"{r['dataset']}\nε={r['eps']}\nN={int(float(r['n_keys']))//1_000_000}M" for r in rows]
    x = np.arange(len(rows))
    overhead = [float(r["rel_overhead_pct"]) for r in rows]
    dpa = [float(r["dpa_total_mb"]) for r in rows]
    ax.bar(x - 0.2, overhead, width=0.4, color=C_BAR1, hatch="//", edgecolor="k", label="rel. overhead %")
    ax.set_ylabel("Rel. overhead of index (%)")
    ax2 = ax.twinx()
    ax2.bar(x + 0.2, dpa, width=0.4, color=C_BAR2, hatch="xx", edgecolor="k", label="DPA mem (MB)")
    ax2.set_ylabel("DPA memory (MB)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_title("Table 1: index overhead & DPA memory")
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper left", frameon=False)
    save(fig, data_dir / "table1_memory")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default=str(Path(__file__).resolve().parent / "out"))
    ap.add_argument(
        "--fig",
        default="all",
        help="9|10|11|12|13|15|table1|all",
    )
    args = ap.parse_args()
    d = Path(args.data_dir)
    mapping = {
        "9": plot_fig9,
        "10": plot_fig10,
        "11": plot_fig11,
        "12": plot_fig12,
        "13": plot_fig13,
        "15": plot_fig15,
        "table1": plot_table1,
    }
    if args.fig == "all":
        for fn in mapping.values():
            fn(d)
    elif args.fig in mapping:
        mapping[args.fig](d)
    else:
        raise SystemExit(f"unknown --fig {args.fig}")


if __name__ == "__main__":
    main()

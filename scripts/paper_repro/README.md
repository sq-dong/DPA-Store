# DPA-Store paper figure reproduction

Reproduce **experimental** figures/tables from
[DPA-Store (arXiv:2601.06231)](https://arxiv.org/abs/2601.06231) on this
cluster (`memserver16` server + `bfhost_72` client).

Paper refs (rasterized eval pages) live in `paper_refs/` for layout matching.
Plot style: dual-axis MOPS bars + p50/p90 latency markers, hatched bars.

## What is / isn’t reproducible here

| Item | Reproducible? | Notes |
|------|---------------|-------|
| **Table 1** | Partial | Needs 50M SOSD bulks for all datasets; artifact has **25M sparse**. Script reports MB + relative overhead for whatever bulks you provide under `out/traces/datasets/<name>/`. |
| **Fig. 9** | Yes | Left: rebuild per traverser count + client `-num_server_readers=N`. Right: stitcher/patcher × UPDATE/INSERT. UDP reader base is fixed (independent of N). |
| **Fig. 10** | Yes | Queue-depth sweep; no rebuild. **Start here.** |
| **Fig. 11** | Partial | Artifact: sparse uniform + Zipf(α=0.99). Extra SOSD sets optional. |
| **Fig. 12** | Yes (sparse) | Learned vs `USE_BTREE_ALGORITHM` + ε→∞. |
| **Fig. 13** | Yes (sparse) | UPDATE/INSERT mixes via `--main_one_of`. |
| **Fig. 14** | **No** | Needs B3140L **and** B3220 + open-loop DPA client saturation. Single BF model here. |
| **Fig. 15** | Partial | YCSB A–F + insert/range for DPA-Store. ROLEX opt-in (`FIG15_ROLEX=1`, not automated). YCSB-F ≈ A (RMW≈update). |
| Fig. 1–8 | N/A | Architecture diagrams, not experiments. |

**Load matching:** paper = 6×31 threads, GET QD=32 → **5952** in-flight
(WRITE QD=18 → **3348**). Defaults here use **93 threads × 64/36 QD** on the
single bfhost_72 so **total in-flight matches**. Fig.10 further doubles each
paper QD tick (`our_qd = 2 × paper_qd`) so every X point has the same total
in-flight as the paper. Absolute MOPS can still differ (1 NIC/host vs 6).

## Prerequisites

1. Dataplane up (see `../README.md` / `docs/DPA-Store-experiments.md`):
   - Server NIC mode, `10.0.1.12` on `ens5f1np1`
   - Client `10.0.1.11` on `ens2f1np1`, DPDK PCI `0000:69:00.1`
2. Server + multiclient closed-loop path already works on this cluster
   (NIC mode on the server BF-3, Ethernet `10.0.1.0/24` up).
3. `matplotlib` on the plotting host (memserver16 has it).

## Quick start

```bash
export REPO=/home/dongshuqi/DPA-Store/dpa-store_branch_default
cd $REPO

# 1) Generate get/update/insert/zipf/range traces + sync client
bash scripts/paper_repro/run_all.sh prep

# 2) Recommended first experiment (no server rebuild)
#    Keep default server (176 trav) running, or let the script start it.
QUICK=1 bash scripts/paper_repro/run_all.sh fig10

# 3) Full paper-scale (long; Fig.9 alone rebuilds many times)
bash scripts/paper_repro/run_all.sh
```

Subset:

```bash
bash scripts/paper_repro/run_all.sh prep fig10 fig13 plot
```

Outputs: `scripts/paper_repro/out/`

| Path | Content |
|------|---------|
| `out/*.csv` | Raw per-run metrics |
| `out/fig*.png` + `.pdf` | Paper-style plots |
| `out/client_logs/` | multiclient stdout |
| `out/server_logs/` | dpastore_host logs |
| `out/traces/` | derived workloads |

## Knobs (`config.env` or env vars)

| Var | Default | Meaning |
|-----|---------|---------|
| `QUICK` | `0` | `1` → tiny ops, 1 run, fewer sweep points |
| `NUM_SHARD_THREADS` | `93` | ≈ half of paper’s 186 threads (1 host) |
| `NUM_INFLIGHT_GET` | `64` | 93×64=**5952** (= paper 6×31×32) |
| `NUM_INFLIGHT_WRITE` | `36` | 93×36=**3348** (= paper 6×31×18) |
| `OPS_PER_THREAD_GET` | `2e6` | long GET runs |
| `NUM_RUNS_LONG` / `SHORT` | `4` / `8` | paper averages |
| `ZIPF_ALPHA` | `0.99` | paper Zipf |
| `FIG15_SKIP_RANGE` | `0` | skip E/range100 (avoids RANGE rebuild) |
| `FIG11_HOTCACHE_OFF` | `0` | extra zipf × hot-cache-off point |

## Wall-clock estimate (full `run_all.sh`, non-QUICK)

Assumptions: warm rebuild ~1–2 min, server ready (FlexIO+25M bulk) ~2–5 min,
each GET run ~0.5–2 min (DPDK init + 93×2e6 ops), paper’s 4/8 repeats.

| Target | Approx. |
|--------|---------|
| prep | 5–15 min |
| Fig.10 | **0.5–1.5 h** |
| Fig.11 (sparse only) | **0.5–1 h** |
| Fig.9 left (10 trav rebuilds) | **2–4 h** |
| Fig.9 right (5 configs × upd/ins) | **1.5–3 h** |
| Fig.12 | **0.5–1 h** |
| Fig.13 | **0.5–1.5 h** |
| Fig.15 | **1–2.5 h** |
| Table 1 | **0.2–0.5 h** |
| **All together** | **~7–15 h** (median about **~10 h**) |

Fig.9 dominates wall time (many rebuilds + restarts). A `QUICK=1` full pass
is usually **1–2 h** (wiring check only; not paper-quality statistics).

## Extra SOSD datasets

Place under `out/traces/datasets/<name>/`:

```
bulk.trace   get.trace   [zipf_get.trace] [update.trace] [insert.trace] [range.trace]
```

Then re-run the relevant sweep; Fig.11/12/13/15/table1 auto-pick them up.

## Manual one-shot (debug)

```bash
source scripts/paper_repro/config.env
source scripts/paper_repro/lib.sh
ensure_bulk_installed
start_server                           # local sudo dpastore_host
RUN_TAG=debug MAIN_TRACE=$TRACE_WORK_DIR/get.trace \
  MAIN_OP=get LOOP_OP=get MAIN_ONE_OF=1 NUM_INFLIGHT=32 \
  OPS_PER_THREAD=100000 run_client
parse_client_log $CLIENT_LOG_DIR/debug.log
stop_server
```

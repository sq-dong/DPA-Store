# Local-cluster helper scripts

Cluster-specific wrappers for `memserver16` (server) + `bfhost_72` (client)
on Ethernet `10.0.1.0/24`. See also `/home/dongshuqi/docs/DPA-Store-experiments.md`.

| Script | Host | Purpose |
|--------|------|---------|
| `prepare_traces.sh` | either | Unpack `traces/*.tar.gz` into working `.trace` files |
| `setup_dataplane.sh` | memserver16 | `net` / `nic-mode` / `traces` |
| `setup_dataplane_72.sh` | bfhost_72 | Bring up client Ethernet + ping |
| `paper_repro/` | both | Paper-scale sweeps + plots (Fig.9–13/15, Table 1) |

```bash
export REPO=/home/dongshuqi/DPA-Store/dpa-store_branch_default

bash $REPO/scripts/prepare_traces.sh --install-tmp
sudo bash $REPO/scripts/setup_dataplane.sh net
sudo bash $REPO/scripts/setup_dataplane_72.sh   # on 72

# paper figures
bash $REPO/scripts/paper_repro/run_all.sh prep
QUICK=1 bash $REPO/scripts/paper_repro/run_all.sh fig10
```

Experiment CSVs/PNGs/PDFs land in `scripts/paper_repro/out/` (gitignored).

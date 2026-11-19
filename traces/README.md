# Traces

| File | In git? | Notes |
|------|---------|-------|
| `read_trace.tar.gz` | yes | Unpack → `read.trace` |
| `bulk_trace.tar.gz` | **no** (~254MB > GitHub 100MB limit) | Keep a local copy next to this README, or attach it as a GitHub Release asset / Git LFS |

```bash
# from repo root
bash scripts/prepare_traces.sh --install-tmp
# expects traces/bulk_trace.tar.gz and/or traces/read_trace.tar.gz
```

If `bulk_trace.tar.gz` is missing, copy it from the original artifact zip
(`dpa-store.zip`) or from another machine that still has it.

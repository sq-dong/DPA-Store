# DPA-Store Artifact

> **Artifact evaluation note.** DPA-Store relies on special hardware (NVIDIA BlueField-3 DPUs with DPAs) that might not commonly available be outside our lab. We therefore **invite reviewers to evaluate the artifact directly on our machines**: we will provide a pre-configured environment and a live walk-through. Please reach out via the artifact submission channel to arrange a meeting. Our institution provides a meeting platform that allows for alias-based anonymous attendance to guarantee **single-blind** evaluation ([bbb.rlp.net](https://bbb.rlp.net)).

Components: DPA-Store server + load generator clients.

| Component | Dir | Role |
|---|---|---|
| Server | `server/` | Server host (x86), BF-3 in **NIC mode** (Arm cores OFF) |
| Multiclient | `x86_client/` | 1..N x86 hosts; **latency + throughput** (closed-loop) |
| DPA client | `dpa_client/` | DPA-client host's BF-3 in **DPU mode**; **throughput only** |
| Trafgen | `trafgen/` | DPA-client host (x86); trigger stream for `dpa_client` |

Defaults match our lab setup. To use your setup see [Configure for your
site](#configure-for-your-site).

## Topology

```
   Server host                                1..N multiclient hosts
   ┌──────────────────────────┐               ┌──────────────────────────┐
   │  dpastore_host  (x86)    │               │  multiclient  (x86)      │   ← latency  +  throughput
   │   ↓ FlexIO RPCs          │ ◄──── net ────┤  end-to-end RTT and/or   │     (one or more hosts
   │  BF-3 NIC mode           │               │  closed-loop throughput, │      target the same
   │  (Arm cores OFF)         │               │  scale by adding hosts   │      server)
   │  DPA cores run the       │               └──────────────────────────┘
   │  DPA-Store tra-          │
   │  verser/stitcher         │                          OR
   │  threads                 │
   │                          │               DPA-client host
   │                          │               ┌──────────────────────────┐
   │                          │               │  trafgen  (x86)          │   ← throughput only
   │                          │ ◄──── net ────┤       ↓                  │     (server-saturation
   │                          │               │  BF-3 DPU mode           │      experiment;
   │                          │               │  (Arm cores ON)          │      open-loop, on-DPU
   │                          │               │  DPA cores run the       │      load generator)
   │                          │               │  dpa_client TX loop      │
   └──────────────────────────┘               └──────────────────────────┘
```

The two right-hand configurations measure different things and are
mutually exclusive per experiment.  Multiclient is closed-loop and
covers both latency CDFs and client-side throughput; DPA-client is
open-loop, has no RX path, and characterizes the *server's*
saturation point past what any reasonable x86 generator can drive.

## Components

**Server** (`server/`).
Host x86 builds the learned index and pushes it to DPA via stitch
commands; DPA cores run traverser / stitcher event handlers
steered by mlx5 DR rules (one reader thread per UDP port,
`DPASTORE_UDPPORT_READER + i`).  Hot cache enabled by default
(`DPASTORE_ENABLE_HOT_CACHE=1`).  Range queries disabled by default
(`DPASTORE_RANGE_REQUEST=0`) as enabling statically increases packet
size for all request types. Dynamically selecting packet size
remains as future work.

Stdin: `t` tree info, `l` partition layout, `s` stats, `q` quit.

**Multiclient** (`x86_client/`).  Closed-loop shards stamp
request id + TSC on TX, match on RX → per-op latency and aggregate
throughput in the same run.  Many client setup: pass `--src_ip --src_mac
--rank_id --world_size --barrier_file` and run on N hosts; flow rules
fan packets in to the same server.  Compile-time transport via
`-DMULTICLIENT_TARGET=`:
- `DPDK`: raw UDP, native DPA-Store wire protocol
- `rolex`: RDMA-UD baseline against the vendored ROLEX KV server
  (`third_party/rolex`); same trace-replay harness
Supports a
  `--main_one_of N` dual-trace pattern (main key every N ops, loop
  trace fills the rest).

**DPA client** (`dpa_client/`).  Each of
`DPASTORE_READER_THREAD_COUNT` (default 176) DPA cores blocks on its
RQ; on the first trafgen packet it rewrites the in-place dst MAC to
the server NIC and enters an **unbounded TX loop** that synthesises
keys (uniform / Zipf / trace-replay), hashes the key to a server
reader-thread UDP port, patches the packet template, and submits.
**One trafgen packet results in a stream of requests**, no x86
involvement at steady state.

**Trafgen** (`trafgen/test_distributed_trafgen.sh`). Emits
fixed-template UDP packets in
parallel from `READER_THREADS` instances (one per server reader
port).  Two modes:
- default: dst MAC = server NIC → bypasses the DPU, hits server
  reader threads directly (raw RX-capacity test);
- `-d`: dst MAC = local DPU → triggers the DPA-client TX loop.

## Build

Standalone CMake projects.
Dependencies (and tested Versions):
- DOCA 3.3.0 (includes FlexIO SDK 24.16)
- DPDK 25.07 (we used a standalone DPDK version, not DOCA-DPDK)
(`server`, `dpa_client`); `libdpdk` + `gflags` (multiclient DPDK);
`Boost::coroutine` + `third_party/rolex` (multiclient rolex);
`netsniff-ng` (trafgen).

```bash
# server   — on the server host
cd server   && cmake -S . -B build && cmake --build build -j

# multiclient — on each x86 client host
cd x86_client && cmake -S . -B build -DMULTICLIENT_TARGET=DPDK && cmake --build build -j
                                                # or =rolex

# dpa_client — on the DPA-client host
cd dpa_client && cmake -S . -B build && cmake --build build -j
```

## Run

```bash
# server
sudo ./server/build/dpastore_host

# multiclient (DPDK; trace-replay example, optional multi-host launcher args)
sudo ./x86_client/build/multiclient \
    --dst_ip=<server IP> --dst_mac=<server MAC> --src_mac=<client MAC> \
    --dev_name=mlx5_0 \
    --main-trace-path=/path/to/insert.trace --main-trace-op=insert \
    --loop-trace-path=/path/to/read.trace   --loop-trace-op=get \
    --main-one-of=1 --trace_wrap_main=true \
    --num_shard_threads=31 --num_inflight=44 \
    --ops_per_thread=250000000 --starvation_time=400 \
    [--rank_id=$R --world_size=$N --barrier_file=/tmp/start]

# dpa_client (trace-replay example: insert main + read loop, main every op)
sudo ./dpa_client/build/dpa_client_host \
    --main-trace-path=/path/to/read.trace --main-trace-op=get \
    --loop-trace-path=/path/to/read.trace   --loop-trace-op=get \
    --main-one-of=1 --trace-wrap-main

# trafgen   (sudo apt install netsniff-ng once)
cd trafgen
sudo ./test_distributed_trafgen.sh -d
```

Multiclient flags (`--help` for the full list; `-` and `_` are interchangeable):

| Flag | Default | |
|---|---|---|
| `--dst_ip` / `--dst_mac` | `10.0.1.12` / empty | server addressing |
| `--src_ip` / `--src_mac` | empty → `10.0.1.11` / NIC MAC | client addressing |
| `--dev_name` | `mlx5_0` | DPDK device |
| `--num_shard_threads` | 0 → `num_tx_threads` | TX+RX shard pairs |
| `--num_inflight` | 64 | outstanding ops per shard thread |
| `--ops_per_thread` | 8096 | 0 = infinite |
| `--starvation_time` | 100 | µs after which an unmatched slot is reclaimed |
| `--main-trace-path` / `--main-trace-op` | empty / inferred | main trace file + op override |
| `--loop-trace-path` / `--loop-trace-op` | empty / inferred | loop trace file (wraps) + op override |
| `--main-one-of` | 20 | issue main-trace op every N ops (`1` = main only, `0` = main disabled) |
| `--trace_wrap_main` / `--trace_no_wrap_main` | false / false | wrap main trace when exhausted vs. stop |
| `--rank_id` / `--world_size` / `--barrier_file` | 0 / 1 / empty | multi-host coordination (run on N hosts, wait on shared barrier file) |

DPA client trace-replay flags:

| Flag | Meaning |
|---|---|
| `--main-trace-path=PATH` | main trace file (one key per line) |
| `--main-trace-op={get,insert,range}` | op replayed for main-trace keys |
| `--loop-trace-path=PATH` | loop trace file (wraps when exhausted) |
| `--loop-trace-op={get,insert,range}` | op replayed for loop-trace keys |
| `--main-one-of=N` | issue a main-trace op once every N ops; `1` = main only |
| `--trace-wrap-main` / `--trace-no-wrap-main` | wrap main trace when exhausted vs. stop |

Trafgen flags: `-i INTERFACE -n TOTAL_PKTS -r RATE -t THREADS -d -h`.

### Trace files

This repository contains a traces directory, which includes bulk-load and runtime traces of a uint64 uniform-sparse-random dataset. Other datasets used in the paper are publicly available through the referenced papers, that initially introduced the usage of those datasets. Traces are text-based and separated by line.

Datasets are available through:
- https://zenodo.org/records/15240501
- https://github.com/learnedsystems/SOSD

## Configure for your site

Most site-specific values now live in **`common/dpastore_common_config.h`**:
the server, dpa_client, and (via the script) trafgen all source their
addressing from it.  Edit the `CLIENT_*` / `SERVER_*` block at the
bottom of that file (or re-point them to your own machine block).
Grep `CONFIGURE FOR YOUR SITE` to jump to each spot.

**`common/dpastore_common_config.h` — single source of truth**

| Symbol | Lab default | Used by |
|---|---|---|
| `CLIENT_IP` / `CLIENT_IP_HEX` / `CLIENT_MAC_HEX` | `dbar-a` → `10.0.1.11` / `0x0a00010b` / `0xa088c2d32031` | client host NIC |
| `CLIENT_DPU_MAC_HEX` | `dbar-a` DPU → `0xa088c2d32041` | dpa_client RX flow rule (this host's DPU MAC) |
| `CLIENT_DEVICE_NAME` | `"mlx5_1"` | dpa_client FlexIO context |
| `SERVER_IP` / `SERVER_IP_HEX` / `SERVER_MAC_HEX` | `dbar-b` → `10.0.1.12` / `0x0a00010c` / `0xa088c2d32ca9` | server host NIC; dpa_client TX dst MAC |
| `SERVER_DEVICE_NAME` | `"mlx5_0"` | server FlexIO context |

**Other source files (CLI-overridable at runtime)**

| File | Symbol | Lab default |
|---|---|---|
| `x86_client/targets/DPDK/target.cpp` ~L54, ~L298 | `-dst_ip` default, `-src_ip` fallback | `10.0.1.12`, `10.0.1.11` |
| `x86_client/targets/rolex/target_config.hpp` ~L23 | `ROLEX_SERVER_HOSTNAME` | `137.226.133.237` |
| `x86_client/targets/rolex/target.cpp` ~L42 | `-dst_ip` default | `137.226.133.237` |

**Trace path**

| File | Symbol | Default |
|---|---|---|
| `server/trace_load.hpp` ~L24 | `DPASTORE_BULK_LOAD_TRACE_PATH` | `/tmp/dpastore_bulk.trace` (synthetic fallback if missing; or pass `-DDPASTORE_BULK_LOAD_TRACE_PATH=...` to cmake) |

**Trafgen packet bytes** (`trafgen/test_distributed_trafgen.sh`,
inside `create_config_for_port()`)

| Field | Lab default |
|---|---|
| `INTERFACE` (~L14, or `-i`) | `enp65s0f1np1` |
| `dst_mac` (default mode) | `0xa0,0x88,0xc2,0xd3,0x2c,0xa9` (dbar-b NIC) |
| `dst_mac` (`-d` mode) | `0xa0,0x88,0xc2,0xd3,0x20,0x41` (dbar-a DPU) |
| Src MAC (heredoc) | `0xa0,0x88,0xc2,0xd3,0x20,0x31` (dbar-a NIC) |
| Src IP / Dst IP (heredoc) | `0x0a,0x00,0x01,0x0b` / `0x0c` (10.0.1.11 / .12) |

`x86_client/run.sh` is a lab-only build wrapper; the commands above
replace it.

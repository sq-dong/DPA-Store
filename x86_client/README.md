# DPAStore Multiclient

For benchmarking distributed key-value store clients. The build links a single backend implementation at configure time.

## Select a Target

Pick the backend by passing `-DMULTICLIENT_TARGET=<name>` when configuring with CMake. Example, using the bundled `test` target:

```sh
cmake -S . -B build -DMULTICLIENT_TARGET=DPDK && cmake --build build
./build/multiclient
```

Place backend sources under `targets/<name>/` with `target.cpp` and `target_config.hpp` implementing the hooks declared in `target_interface.hpp`.

## Building all targets

```sh
for target in $(echo 'rolex test DPDK'); do
	cmake -S . -B build_$target -DMULTICLIENT_TARGET=$target && cmake --build build_$target
done
```

### Bundled Targets

- `test`: Minimal stub that exercises the harness infrastructure.
- `rolex`: Integration of the Rolex RDMA client. Select with `-DMULTICLIENT_TARGET=rolex`. Configuration that Rolex normally reads from gflags is set via macros in `targets/rolex/target_config.hpp`; adjust those values to match your deployment (server address, NIC index, coroutine count, etc.). Building this target requires the Rolex third-party dependencies (gflags, ibverbs, Boost).

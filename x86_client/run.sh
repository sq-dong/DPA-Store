#!/bin/bash

clear
distro_name=$(awk -F= '/^NAME/ { print $2 }' /etc/os-release | tr -d '"')

if [[ $(hostname) == "dbar-a" ]]; then
    dir="build"
    traces="../../tracemaker/trace_0.log"
    dst_mac="a0:88:c2:d3:2c:a8"
    src_mac="a0:88:c2:d3:20:30"
else
    if [[ "$distro_name" == "Ubuntu" ]]; then
        dir="build_ubuntu"
    elif [[ "$distro_name" == "Rocky Linux" ]]; then
        dir="build_rocky"
    else
        echo "Unknown distro"
    fi
    traces="/home/sass/trace.log"
#    traces="/tmp/uniform_sparse_200M_uint64.uniform.YCSB-C.trace"
    # set values for oscar
    dst_mac="58:a2:e1:2f:9e:60"
    src_mac=""
fi

dev=mlx5_0
qd=8
tx_threads=8
rx_threads=8
rb_threads=8

# chose from rolex, DPDK
target=rolex

echo "Building into $dir"

# this build is only to satisfy vscodium path for .clangd file
if [[ "$distro_name" == "Rocky Linux" ]]; then
    cmake -S . -B build -DMULTICLIENT_TARGET=$target && \
    cmake --build build -j12
fi

set -o xtrace
cmake -S . -B $dir -DMULTICLIENT_TARGET=$target && \
cmake --build $dir -j12 && \
sudo ./$dir/multiclient -main_trace_path="/tmp/uniform_sparse_200M_uint64.uniform.YCSB-C.trace" -dst_ip="137.226.133.237" 
# sudo ./$dir/multiclient -- -dev_name=$dev -trace_file=$traces -main_trace_path="/tmp/uniform_sparse_200M_uint64.uniform.YCSB-C.trace" -num_inflight=$qd -num_rx_threads=$rx_threads -num_tx_threads=$tx_threads -num_rb_threads=$rb_threads -dst_ip="137.226.133.237" -dst_mac=$dst_mac -src_ip="1.1.1.1"  -src_mac=$src_mac

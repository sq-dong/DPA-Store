#!/bin/bash
sudo ./build/dpa_client_host \
--main-trace-path=/home/ubuntu/jan/dpastore/traces/read.trace \
--main-trace-op=get \
--loop-trace-path=/home/ubuntu/jan/dpastore/traces/read.trace \
--loop-trace-op=get \
--main-one-of=1 \
--trace-wrap-main

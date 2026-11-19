# NOTE

- Paper: https://www.usenix.org/system/files/fast23-li-pengfei.pdf
- Original Repo: https://github.com/iotlpf

Requirements:

Just for Ubuntu currently, translte if needed.
```
apt install cmake libboost-all-dev libgflags-dev
```

Then build using
```
cmake . && cmake --build . -j
```

Run server and client on seperate machines, there are no parameters to be set.


From the original repository, a lot of modifications are necessary, and at the time of writing this, not yet done.
- Implement server that does more than just local experiments.
- Imelement client that connects to the server and then runs remote experiments.

Dummy functions are somewhat given in `./rolex/rolex_{client,server}.hpp`. Still, not quite what's needed.

## ROLEX
ROLEX: A Scalable RDMA-oriented Learned Key-Value Store for Disaggregated Memory Systems

### Build
1. Dependency
```
MLNX_OFED_LINUX-5.4-1.0.3.0-ubuntu18.04-x86_64
Boost_1.65
Gflags_2.2.2
```
2. CMake
```
$ mkdir build
$ cd build
$ cmake ..
$ make
```
3. Create HugePage
### Run
```
./rolex --nkeys=100000 --non_nkeys=100000
```
   
| parameters | descriptions |
|  ----  | ----  |
| --nkeys  | the number of trained data |
| --non_nkeys  | the number of new data |
| --threads  | the number of threads |
| --coros  | the number of coroutines |
| --workloads  | workloads |
| --read_ratio  | the read ratio |
| --insert_ratio  | the write ratio |
| --update_ratio  | the update ratio |.

# wrk2

wrk2 has different setup than iperf/sockperf test but networking part can be reused.

## Client

```shell
./setup/wrk/build_wrk.sh
sed -e 's/^NODE_ID = 1/# NODE_ID = 1/' -e 's/^# NODE_ID = 0/NODE_ID = 0/' .env.template > .env
./setup/setup_node.sh
```

## Server

```shell
cd ~/code/umanet
echo '
NODE_ID=1
NIC=enp23s0f0np0
NIC_PCI=0000:17:00.0
OTHER_NODE_MAC=40:a6:b7:c3:4d:40

TMPDIR=/tmp
# TEST=iperf
# TEST=iperf-udp
# TEST=sockperf
TEST=wrk

# microvm = 1vcpu, 512MB, 2 queues (like TAP/DPDK tests)
OVS_VM_SIZE=microvm

ETH_RX_CORES=2
ETH_TX_CORES=1
VHOST_RX_CORES=1
VHOST_TX_CORES=3
ETH_RX_QUEUES=2
ETH_TX_QUEUES=1

######### multinode #########
BASE_DIR=~/code/umanet
USER=X
NODE1=er105.utah.cloudlab.us
NETWORK=tap
# NETWORK=dpdk
# NETWORK=ovs-dpdk' > .env
./setup/setup_node.sh
command -v cloud-hypervisor || (curl -L https://github.com/cloud-hypervisor/cloud-hypervisor/releases/download/v50.0/cloud-hypervisor-static -o ch && sudo install ch -m 0755 /usr/bin/cloud-hypervisor)
[ -f /tmp/noble-server-cloudimg-amd64.raw -a -f /tmp/vmlinux.bin ] || ./setup/img/download_img.sh
./setup/img/build_initramfs.sh
./setup/img/build_rw_disk.sh 32 512
./setup/cloudinit/gen-cloud-init.sh 32
./setup/img/build_wrk_image.sh
```

# TAP Test

## Server

```shell
NUM_VM=32
./setup/cpu/slice_cpu.sh tap
./setup/vm/setup_br_tap.sh $NUM_VM

for i in $(seq 0 $(( NUM_VM - 1 ))); do
    tmux new-session -s "vm$i" -d "./testing/scripts/wrk/server-start.sh $i"
done
```

## Client

```shell
# Config (Num VM and testname) via environment variable
./testing/scripts/wrk/start-experiment.sh
```

# Rates

`rates/*` contains rate each machine will run. These can be configured to create skewed workload.

The file is in format `rates/<expected_rps>` the content contains N line each line represents rate which wrk sent.

To generate constant workload across all 32 machines.

```shell
mkdir -p ./testing/wrk/rates
for A in $(seq 200 200 2400); do export A && perl -e 'print "$ENV{A}\n"x32' > ./testing/wrk/rates/$((A*32)); done
```

# Extract

Data extraction is done via `awk` script. You probably don't need this as it will be automated.

The percentile is calculated using interpolation.

The information extracted are `ActualRate` (actual rps perform by wrk, requested rps may not be equal to actual rps) and `Latency` (latency at requested percentile).

```shell
# awk -f ./testing/scripts/wrk/extract.awk <wrkresult> <percentile>

# Read median of rps of wrk instance 1
awk -f ./testing/scripts/wrk/extract.awk ./testing/wrk/out/ovsdpdk32vm/3200/1 0.5
```

# Transform

The script will scan `out` directory for test result and [extract given percentile](#extract).
Then, it will output csv file on `testing/wrk/agg/<testname>/<expectedrps>/<percentile>/each`.

```shell
# ./testing/scripts/wrk/transform.pl <percentile>
./testing/scripts/wrk/transform.pl 0.5
```

Example output `testing/wrk/agg/ovsdpdk32vm/102400/0.5/each`
```csv
ID,Latency,ActualRate
1,1.141,3198.29
10,1.100,3198.51
11,1.105,3198.36
12,1.122,3198.41
13,1.134,3198.42
14,1.161,3198.33
15,1.075,3198.30
16,1.155,3198.31
17,1.142,3198.40
18,1.196,3198.26
19,1.071,3198.33
2,1.131,3198.23
20,1.094,3198.36
21,1.121,3198.36
22,1.063,3198.47
23,1.104,3198.52
24,1.073,3198.44
25,1.157,3198.40
26,1.097,3198.36
27,1.119,3198.43
28,1.168,3198.54
29,1.141,3198.45
3,1.110,3198.22
30,1.132,3198.42
31,1.091,3198.30
32,1.087,3198.48
4,1.132,3198.47
5,1.140,3198.45
6,1.113,3198.43
7,1.084,3198.42
8,1.089,3198.30
9,1.114,3198.31
```

#!/usr/bin/env bash
set -eu

# disable Simultaneous multithreading (2 threads/core => 1 thread/core), SMT hurts latency and predictability
# before:
# core 0:
#   CPU 0  (thread 0)
#   CPU 28 (thread 1)
# core 0: cannot schedule two runnable threads on the same physical core, stronger isolation
#   CPU 0 only
echo off | sudo tee /sys/devices/system/cpu/smt/control

# give system cores 0-9
sudo mkdir -p /etc/systemd/system/system.slice.d
sudo cp ~/code/umanet/setup/cpu/system.conf /etc/systemd/system/system.slice.d/override.conf

# give datapath (TAP/DPDK) cores 0-7
# may have problems using TAP when DPDK is using cores 0-7
sudo cp ~/code/umanet/setup/cpu/datapath.conf /etc/systemd/system/datapath.slice
# TAP
# systemd-run \
#   --slice=datapath.slice \
#   iperf3 -s

# DPDK
# --lcores="0-7"
# --main-lcore=0

sudo systemctl daemon-reexec
echo "⚙️ system.slice: $(cat /sys/fs/cgroup/system.slice/cpuset.cpus)"
echo "⚙️ datapath.slice: $(cat /sys/fs/cgroup/datapath.slice/cpuset.cpus)"
echo "⚙️ user.slice: $(cat /sys/fs/cgroup/user.slice/cpuset.cpus)"
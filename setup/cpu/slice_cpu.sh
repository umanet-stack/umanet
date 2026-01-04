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

# give system cores 24-27
sudo mkdir -p /etc/systemd/system/system.slice.d
sudo cp ~/code/umanet/setup/cpu/system.conf /etc/systemd/system/system.slice.d/override.conf

# TAP/DPDK/vm can use cores 0-23
sudo mkdir -p /etc/systemd/system/vms.slice.d
sudo cp ~/code/umanet/setup/cpu/vms.conf /etc/systemd/system/vms.slice.d/override.conf

sudo systemctl daemon-reload
sudo systemctl daemon-reexec
echo "⚙️ system.slice: $(cat /sys/fs/cgroup/system.slice/cpuset.cpus)"
echo "⚙️ vms.slice: $(cat /sys/fs/cgroup/vms.slice/cpuset.cpus)"
# echo "⚙️ user.slice: $(cat /sys/fs/cgroup/user.slice/cpuset.cpus)"
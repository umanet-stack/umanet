```bash
# disable Simultaneous multithreading (2 threads/core => 1 thread/core), SMT hurts latency and predictability
# before:
# core 0:
#   CPU 0  (thread 0)
#   CPU 28 (thread 1)
# core 0: cannot schedule two runnable threads on the same physical core, stronger isolation
#   CPU 0 only
echo off | sudo tee /sys/devices/system/cpu/smt/control

sudo mkdir -p /etc/systemd/system/system.slice.d
sudo cp ~/code/umanet/setup/cpu/system.conf /etc/systemd/system/system.slice.d/override.conf

sudo systemctl daemon-reexec
cat /sys/fs/cgroup/system.slice/cpuset.cpus
cat /sys/fs/cgroup/user.slice/cpuset.cpus

# will show 0-7 only when something is running in the slice (will still show even after stopping the service)
cat /sys/fs/cgroup/datapath.slice/cpuset.cpus
systemctl status datapath.slice

sudo systemd-run --slice=datapath.slice iperf3 -s
# Running as unit: run-rabd36d9b8ff0438a95980e1f7334989a.service; invocation ID: 1dacaa5d1cb743a9bb0ad98c262a743f
sudo systemctl stop run-rabd36d9b8ff0438a95980e1f7334989a.service

./setup/cpu/slice_cpu.sh
```

| Cores     | Purpose                    |
| --------- | -------------------------- |
| **0–7**   | TAP / DPDK (8 cores)       |
| **8–9**   | Host / kernel / background |
| **10–11** | Guard / spare              |
| **12–27** | VMs (16 cores)             |

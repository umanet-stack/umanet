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

./setup/cpu/slice_cpu.sh
```

| Cores     | Purpose                    |
| --------- | -------------------------- |
| **0–9**   | system + TAP/DPDK          |
| **10–11** | Guard / spare              |
| **12–27** | VMs (16 cores)             |

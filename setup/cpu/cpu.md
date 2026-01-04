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


```

| Cores     | Purpose                    |
| --------- | -------------------------- |
| **0–7**   | TAP / DPDK (8 cores)       |
| **8–9**   | Host / kernel / background |
| **10–11** | Guard / spare              |
| **12–27** | VMs                        |

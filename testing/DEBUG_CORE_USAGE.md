# Debugging DPDK Core Usage

## Problem: Only Master Core (Core 4) at 100%, Fastpath Cores (5-7) Idle

### Root Cause

The fastpath cores are **correctly pinned** but **idle** because:
1. **No VMs are connected** - Check with: `ls -la /mnt/huge/vhost-user*`
2. **No packets to process** - Fastpath threads only work when VMs send/receive packets

### Verification Steps

```bash
# 1. Check if threads are correctly pinned
ps -eLo pid,tid,psr,comm,cmd | grep -E "vhost-switch|stcp-fp"

# Expected output:
# - Core 4: main thread (master)
# - Core 5: stcp-fp-0 (fastpath core 0)
# - Core 6: stcp-fp-1 (fastpath core 1)  
# - Core 7: stcp-fp-2 (fastpath core 2)

# 2. Check if VMs are connected
ls -la /mnt/huge/vhost-user*

# If empty, no VMs are connected yet

# 3. Check CPU usage per core
mpstat -P ALL 1

# 4. Check if fastpath threads are doing work
sudo perf top -p <stcp-fp-0-pid> -d 1

# If showing 0% or sleeping, threads are idle (no work)
```

### Why Master Core is at 100%

The master core (core 4) is busy because:
- It handles vhost-user device registration
- It waits for VMs to connect
- It manages device lifecycle

This is **normal** when no VMs are connected.

### When Fastpath Cores Become Active

Fastpath cores will show CPU usage when:
1. **VMs connect** - vhost-user sockets appear in `/mnt/huge/`
2. **Packets flow** - VMs send/receive network traffic
3. **Work is distributed** - Each fastpath core handles assigned VMs

### Code Flow

In `dataplane_loop()`:
```c
// If no devices, skip polling
if (current_device_num == 0) {
    was_idle = 1;
    continue;  // ← Fastpath threads hit this when no VMs connected
}
```

### Solution

1. **Start VMs** - Launch your VMs (QEMU/Cloud-Hypervisor/Firecracker)
2. **Connect to vhost-user** - VMs should connect to sockets in `/mnt/huge/`
3. **Generate traffic** - Run iperf or other network tests
4. **Monitor cores** - Fastpath cores should now show CPU usage

### Expected Behavior After VMs Connect

```bash
# With 16 VMs connected and iperf running:
mpstat -P ALL 1

# Expected:
# Core 4: ~20-30% (master, handling device management)
# Core 5: ~80-100% (fastpath, processing packets from VMs 0,3,6,9,12,15)
# Core 6: ~80-100% (fastpath, processing packets from VMs 1,4,7,10,13)
# Core 7: ~80-100% (fastpath, processing packets from VMs 2,5,8,11,14)
```

### Debugging Commands

```bash
# Check thread pinning
ps -eLo pid,tid,psr,comm | grep stcp-fp

# Check vhost-user sockets
ls -la /mnt/huge/vhost-user*

# Check per-thread CPU usage
top -H -p $(pgrep vhost-switch)

# Profile a fastpath thread
sudo perf top -p <stcp-fp-0-pid> -g

# Check if threads are sleeping (idle)
sudo strace -p <stcp-fp-0-pid> -e trace=nanosleep,usleep
```


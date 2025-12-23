# How to Read `perf top` Output

## Quick Start

```bash
# Profile a running process by PID
sudo perf top -p <PID>

# Profile all processes (system-wide)
sudo perf top

# Profile specific CPU cores (useful for DPDK)
sudo perf top -C 4,5,6,7

# Profile with call graphs (shows function call chains)
sudo perf top -g

# Profile with more samples per second (higher overhead, more accurate)
sudo perf top -F 1000
```

## Reading the Output

### Main Display Columns

```
Samples: 123K of event 'cycles', 4000 Hz, Event count (approx.): 123456789
Overhead  Shared Object       Symbol
  45.23%  vhost-switch        [.] poll_virtio_tx
  12.34%  vhost-switch        [.] rte_vhost_dequeue_burst
   8.90%  libc-2.31.so       [.] memcpy
   5.67%  vhost-switch        [.] dataplane_loop
   3.45%  [kernel]            [k] __softirq_entry
```

**Column Meanings:**

1. **Overhead** (e.g., `45.23%`):
   - Percentage of total CPU time spent in this function
   - Higher = more time spent = potential bottleneck
   - **For DPDK apps**: Look for functions >5% as optimization targets

2. **Shared Object** (e.g., `vhost-switch`, `libc-2.31.so`, `[kernel]`):
   - **Your binary** (`vhost-switch`): Your code
   - **Libraries** (`libc-2.31.so`, `librte_*.so`): System/DPDK libraries
   - **`[kernel]`**: Kernel code (syscalls, interrupts)
   - **`[unknown]`**: No debug symbols (rebuild with `-g`)

3. **Symbol** (e.g., `poll_virtio_tx`, `memcpy`):
   - Function name where CPU time is spent
   - **`[.]`** = userspace code
   - **`[k]`** = kernel code
   - **`[.]`** with `[unknown]` = missing debug symbols

### Header Line

```
Samples: 123K of event 'cycles', 4000 Hz, Event count (approx.): 123456789
```

- **Samples**: Number of samples collected
- **Event**: What's being measured (usually `cycles` = CPU cycles)
- **Hz**: Sampling frequency (samples per second)
- **Event count**: Approximate total events

## For Your DPDK App

### 1. Profile the Main Dataplane Loop

```bash
# Find your DPDK process
ps aux | grep vhost-switch

# Profile it (replace PID)
sudo perf top -p <PID> -g

# Or profile specific cores (DPDK usually pins cores)
sudo perf top -C 4,5,6,7 -g
```

### 2. What to Look For

**Good signs:**
- `dataplane_loop` or `poll_virtio_tx` at the top (your code is running)
- Low overhead in `[kernel]` (not syscall-bound)
- Most time in packet processing functions

**Bad signs (bottlenecks):**
- High overhead in `memcpy`/`memset` (memory copies)
- High overhead in `[kernel]` (too many syscalls)
- High overhead in `rte_pause` or `usleep` (waiting/idle)
- `[unknown]` symbols (rebuild with debug symbols)

### 3. Common DPDK Bottlenecks in `perf top`

```
# Memory copies (optimize with zero-copy)
  15.23%  libc-2.31.so       [.] memcpy

# Too much pausing/sleeping
   8.45%  vhost-switch        [.] usleep
   3.21%  vhost-switch        [.] rte_pause

# Kernel overhead (syscalls, interrupts)
  12.34%  [kernel]            [k] __softirq_entry
   5.67%  [kernel]            [k] do_syscall_64

# Lock contention
   7.89%  vhost-switch        [.] rte_spinlock_lock
```

## Useful Options

### Basic Options

```bash
# -g: Show call graphs (function call chains)
sudo perf top -g

# -F <Hz>: Sampling frequency (default ~4000)
# Higher = more accurate but more overhead
sudo perf top -F 1000

# -p <PID>: Profile specific process
sudo perf top -p 12345

# -C <cores>: Profile specific CPU cores
sudo perf top -C 4,5,6,7

# -e <event>: Profile different events
sudo perf top -e cycles          # CPU cycles (default)
sudo perf top -e instructions    # Instructions retired
sudo perf top -e cache-misses    # Cache misses
sudo perf top -e branch-misses   # Branch mispredictions
```

### Advanced Options

```bash
# Show kernel symbols (requires root)
sudo perf top -k

# Show source code annotations (requires debug symbols)
sudo perf top --source

# Sort by different metrics
sudo perf top --sort comm,overhead,symbol

# Save output to file
sudo perf top > perf_output.txt
```

## Example Workflow for Your DPDK App

```bash
# 1. Find your DPDK process
ps aux | grep vhost-switch
# Note the PID (e.g., 640674)

# 2. Profile with call graphs
sudo perf top -p 640674 -g

# 3. While running iperf test, watch for:
#    - Functions taking >5% overhead
#    - Memory copies (memcpy)
#    - Kernel overhead (syscalls)
#    - Lock contention

# 4. Press 'h' for help, 'q' to quit

# 5. For more detailed analysis, use perf record + report
sudo perf record -g -p 640674 sleep 30
sudo perf report -g
```

## Interpreting Results

### High Overhead in Your Code
```
  45.23%  vhost-switch        [.] poll_virtio_tx
```
✅ **Good**: Your code is running, but this function is a bottleneck
- **Action**: Optimize this function (reduce work, batch operations)

### High Overhead in Libraries
```
  15.23%  libc-2.31.so       [.] memcpy
```
⚠️ **Warning**: Too many memory copies
- **Action**: Use zero-copy techniques, reduce packet copies

### High Overhead in Kernel
```
  12.34%  [kernel]            [k] __softirq_entry
```
⚠️ **Warning**: Too many interrupts/syscalls
- **Action**: Reduce syscalls, use DPDK polling instead

### Unknown Symbols
```
   8.90%  vhost-switch        [.] [unknown]
```
❌ **Problem**: Missing debug symbols
- **Action**: Rebuild with `-g -O2` flags

## Tips

1. **Run during load**: Profile while your iperf test is running
2. **Multiple samples**: Run `perf top` multiple times to see consistency
3. **Compare**: Compare `perf top` before/after optimizations
4. **Call graphs**: Use `-g` to see which functions call the hot functions
5. **Focus on top 10**: Usually the top 5-10 functions account for most overhead

## Keyboard Shortcuts (in perf top)

- `h`: Show help
- `q`: Quit
- `+/-`: Zoom in/out
- `E`: Expand call chain
- `C`: Collapse call chain
- `P`: Toggle percent/absolute counts
- `K`: Toggle kernel symbols
- `Z`: Toggle zero samples


```bash
# check idle state enabled or not
cpupower idle-info

# disable all idle states
sudo cpupower idle-set -D 0

sudo perf stat -e power:cpu_idle -a sleep 10
# Performance counter stats for 'system wide':

#          6,306,040      power:cpu_idle  

sudo turbostat --quiet --show CPU,C1%,C6% --interval 1
# C1%, C6% should be 0
```
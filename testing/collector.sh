#!/usr/bin/env bash
set -euo pipefail

OUTDIR=testing/results
mkdir -p "$OUTDIR"

monitor_vms() {
  while true; do
    vm_count=$(pgrep -c cloud-hyp 2>/dev/null || true)
    echo -ne "\r[$(date '+%H:%M:%S')] Running VMs: $vm_count | Results collected: $(ls -1 "$OUTDIR"/*.json 2>/dev/null | wc -l)   " >&2
    sleep 2
  done
}

monitor_vms &
MONITOR_PID=$!

# Trap to cleanup monitor on exit
trap "kill $MONITOR_PID 2>/dev/null" EXIT

echo "" >&2
echo "🔥 Collector started. Listening on port 9000..." >&2
echo "" >&2

while true; do
  nc -l 9000 | jq -c '
    . as $obj
    | ($obj.vm) as $vm
    | ($obj | del(.vm)) 
    | {vm: $vm, result: .}
  ' | while read -r line; do
      vm=$(echo "$line" | jq -r '.vm')
      echo "$line" | jq '.result' > "$OUTDIR/$vm.json"
      echo -e "\n✅ wrote $OUTDIR/$vm.json" >&2
  done
done

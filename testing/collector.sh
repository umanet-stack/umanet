#!/usr/bin/env bash
set -euo pipefail

OUTDIR=results
mkdir -p "$OUTDIR"

while true; do
  nc -l 9000 | jq -c '
    . as $obj
    | ($obj.vm) as $vm
    | ($obj | del(.vm)) 
    | {vm: $vm, result: .}
  ' | while read -r line; do
      vm=$(echo "$line" | jq -r '.vm')
      echo "$line" | jq '.result' > "$OUTDIR/$vm.json"
      echo "✅ wrote $OUTDIR/$vm.json"
  done
done

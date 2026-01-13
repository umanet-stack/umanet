#!/usr/bin/env bash
set -e

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <base_dir>"
  exit 1
fi

BASE_DIR=$1

${BASE_DIR}/setup/cpu/slice_cpu.sh tap
${BASE_DIR}/setup/vm/setup_br_tap.sh 32
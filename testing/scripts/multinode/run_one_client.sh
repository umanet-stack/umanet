#!/bin/sh

./setup/vm/setup_br_tap.sh 32
./setup/vm/spawn_vms.sh tap 32 vm-client
python testing/process_logs/main.py tap vm-client
ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9

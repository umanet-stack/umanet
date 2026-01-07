#!/bin/bash

./setup/vm/setup_br_tap.sh $1
./setup/vm/spawn_vms.sh tap $1 vm-client
python testing/process_logs/main.py tap vm-client
ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9

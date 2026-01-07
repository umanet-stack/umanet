#!/bin/bash

./setup/vm/setup_br_tap.sh $1
./setup/vm/spawn_vms.sh tap $1 vm-server

ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9

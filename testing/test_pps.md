make sure to use UDP iperf options in `.env`
# tap
```bash
# both nodes
./setup/vm/setup_br_tap.sh 32 pps
# node 1
./setup/vm/spawn_vms.sh tap 32 vm-server
# node 0
./setup/vm/spawn_vms.sh tap 32 vm-client

# -b 0 = no rate limit, -l 64 = packet size
iperf3 -c 192.168.100.2 -u -b 0 -l 64 -P 4 --get-server-output
```
# dpdk
```bash



```
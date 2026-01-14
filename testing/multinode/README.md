# multinode testing
```bash
# start iperf3 servers
sudo ./testing/multinode/iperf_server.sh

# kill all iperf3 servers
sudo pkill -f "^iperf3 -s"

sudo chown -R X /users/X/code/umanet/testing
```
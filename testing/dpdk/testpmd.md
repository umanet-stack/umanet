```bash
sudo dpdk-testpmd -l 0-7 -n 4 -- -i --forward-mode=rxonly

# --txpkts=64: pkt size 64B
# --burst=32: Sends packets in batches of 32
# --nb-cores = number of forwarding worker cores
# --txq = number of TX queues per port, should be equal to --nb-cores so that each core handles one queue
sudo dpdk-testpmd -l 0-7 -n 4 -- -i --forward-mode=txonly --txpkts=64 --burst=32

# should see: ports=1 - cores=6 - streams=6
# 61 Mpps
sudo dpdk-testpmd \
  -l 0-7 \
  -n 4 \
  -- \
  -i \
  --stats-period=1 \
  --forward-mode=txonly \
  --txpkts=64 \
  --burst=32 \
  --nb-cores=6 \
  --txq=6 \
  --rxq=6 \
  --port-topology=chained \
  --eth-peer=0,02:00:00:00:00:00


start
show port stats all
```
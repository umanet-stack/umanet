### VHOST RX
- RX empty = producer idle → soft skip (idle_mask)
- if do real sleep, introduce hard latency spikes:
    - VM generates a packet right after you block
    - You refuse to poll for 10 µs
    - ACKs / control packets are delayed
    - TCP throughput collapses

### VHOST TX
- TX failure = consumer slow → hard stop
- pause for some time e.g. 10us

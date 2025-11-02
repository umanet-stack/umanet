# fahren
## Prerequisites
- use Linux (some syscalls in code are Linux-only)
- rustc 1.91.0

```bash
sudo ip tuntap add dev tap0 mode tap
sudo ip link set tap0 up

sudo env "PATH=$PATH" cargo run -- --net-backend tap=tap0,socket=/tmp/vhost-user-net.sock
```

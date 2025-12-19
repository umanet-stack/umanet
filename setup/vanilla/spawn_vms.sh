# iperf: vm{even} = server, vm{odd} = client

for i in {0..31}; do
    if [ $((i % 2)) -eq 0 ]; then
        # server
        sudo cloud-hypervisor \
        --cpus boot=1 \
        --memory size=512M \
        --kernel /proj/faasnetworkstack-PG0/testing/kernels/vm$i-kernel.bin \
        --cmdline "console=ttyS0 console=hvc0 root=/dev/vda1 rw systemd.mask=systemd-networkd-wait-online.service systemd.mask=snapd.service systemd.mask=snapd.seeded.service systemd.mask=snapd.socket" \
        --disk path=/proj/faasnetworkstack-PG0/testing/images/vm$i-img.raw path=/tmp/cloudinit/cloudinit-vm$i.img \
        --net "tap=tap$i,mac=12:34:56:78:90:$(printf "%02X" $i)" 
    else
        # client
    fi
done
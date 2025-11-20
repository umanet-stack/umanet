sudo modprobe nbd max_part=8
sudo qemu-nbd -c /dev/nbd0 -f raw /tmp/noble-server-cloudimg-amd64.raw
sudo mount /dev/nbd0p1 /mnt
sudo rm -rf /mnt/var/lib/cloud/instances /mnt/var/lib/cloud/instance /mnt/var/lib/cloud/data
sudo umount /mnt
sudo qemu-nbd -d /dev/nbd0
echo "✅ Cloud-init state cleaned successfully!"
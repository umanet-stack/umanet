sudo bash -c "ps aux | grep cloud-hypervisor | grep -v grep | awk '{print \$2}' | xargs kill -9 ; rm /tmp/vhost-user* ; rm -rf testing/ovs_dpdk"

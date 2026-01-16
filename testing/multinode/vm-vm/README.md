```bash
# get private key from local
scp -i ~/.ssh/cloudlab ~/.ssh/cloudlab X@er011.utah.cloudlab.us:~/.ssh/cloudlab
# try
ssh -i ~/.ssh/cloudlab X@er016.utah.cloudlab.us


sudo ./testing/multinode/vm-vm/run_all.sh 64
# if you run dpdk network, kill umanet process on both nodes before switching to other network
sudo bash -c "ps aux | grep umanet | grep -v grep | awk '{print \$2}' | xargs kill -9"

# export plots (run from local machine)
scp -i ~/.ssh/cloudlab -r X@er011.utah.cloudlab.us:~/code/umanet/testing/plot_reports ./plots
```
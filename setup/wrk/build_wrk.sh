#!/usr/bin/env bash

set -ex

TMP_DIR=$(mktemp -d)
cd $TMP_DIR
curl -LO https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
tar xzvf zlib-1.3.1.tar.gz
cd zlib-1.3.1/
./configure
make -j$(nproc)
sudo make install

cd $TMP_DIR
git clone https://github.com/giltene/wrk2 --depth=1 --filter=blob:none
cd wrk2
make -j$(nproc)
sudo install -m 0755 wrk /usr/local/bin

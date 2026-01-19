#!/usr/bin/env bash
set -ex

mkdir -p ~/debs
wget -O - https://openresty.org/package/pubkey.gpg | sudo gpg --dearmor -o /usr/share/keyrings/openresty.gpg
echo "deb [signed-by=/usr/share/keyrings/openresty.gpg] http://openresty.org/package/ubuntu $(lsb_release -sc) main" | sudo tee /etc/apt/sources.list.d/openresty.list
sudo apt update
sudo apt install --download-only -y openresty
sudo mv /var/cache/apt/archives/*.deb ~/debs/

echo 'events {
    worker_connections 2000;
}

http {
    server {
      listen 80;

      location / {
        default_type text/plain;

        content_by_lua_block {
          local n = 15

          local function fib(n)
            if n <= 1 then
              return n
            else
              return fib(n - 1) + fib(n - 2)
            end
          end

          ngx.say(tostring(fib(n)))
        }
      }
    }
}' > ~/nginx.conf

qemu-img convert -O qcow2 /tmp/noble-server-cloudimg-amd64.raw /tmp/modified.qcow2

sudo virt-customize \
  --format qcow2 \
  --add /tmp/modified.qcow2 \
  --run-command 'echo nameserver 8.8.8.8 > /etc/resolv.conf' \
  --root-password password:123456 \
  --run-command 'mkdir -p /etc/nginx /root/debs' \
  --copy-in $HOME/debs:/root \
  --run-command 'systemctl mask snapd.service' \
  --run-command 'systemctl mask snapd.seeded.service' \
  --run-command 'systemctl mask snapd.socket' \
  --run-command 'dpkg -i /root/debs/*.deb' \
  --upload ~/nginx.conf:/etc/openresty/nginx.conf

#!/bin/sh
# Installs the SpaghettiKart online relay on a Debian/Ubuntu Linux server (any VPS, or Oracle Cloud's free VM)
# and keeps it running across reboots. Run as root:
#   curl -fsSL https://raw.githubusercontent.com/Chrisbchick3n/SpaghettiKart/netplay/tools/netplay/install-relay.sh | sh
set -e
REPO="${REPO:-https://github.com/Chrisbchick3n/SpaghettiKart}"
BRANCH="${BRANCH:-netplay}"
PORT="${PORT:-25564}"

apt-get update
apt-get install -y --no-install-recommends ca-certificates git g++ cmake make

rm -rf /opt/spaghetti-relay-src
git clone --depth 1 --filter=blob:none --sparse -b "$BRANCH" "$REPO" /opt/spaghetti-relay-src
cd /opt/spaghetti-relay-src
git sparse-checkout set src/enhancements/netplay tools/netplay
cmake -S tools/netplay -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/netplay-loopback-test > /dev/null 2>&1 && echo "Self-test passed."
install -m 755 build/spaghetti-netplay-relay /usr/local/bin/spaghetti-netplay-relay

cat > /etc/systemd/system/spaghetti-relay.service <<EOF
[Unit]
Description=SpaghettiKart online relay
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/bin/spaghetti-netplay-relay $PORT
Restart=always
RestartSec=2
DynamicUser=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now spaghetti-relay
systemctl restart spaghetti-relay

# Open the port in the local firewall if one is active (cloud providers also have their own firewall:
# open TCP $PORT there too, e.g. Oracle Cloud "Security List" / AWS "Security Group").
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q active; then ufw allow "$PORT"/tcp; fi
if command -v iptables >/dev/null 2>&1 && iptables -S INPUT 2>/dev/null | grep -q "REJECT"; then
    iptables -I INPUT -p tcp --dport "$PORT" -j ACCEPT
    command -v netfilter-persistent >/dev/null 2>&1 && netfilter-persistent save || true
fi

IP=$(curl -fsS4 https://api.ipify.org 2>/dev/null || hostname -I | awk '{print $1}')
echo
echo "SpaghettiKart relay is running on TCP port $PORT."
echo "Set the GitHub repo variable NETPLAY_RELAY to:  $IP$( [ "$PORT" = 25564 ] || echo ":$PORT" )"

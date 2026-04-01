#!/bin/bash
# XTunnel PC — iniciar todo
# Uso: bash start.sh

WIFI=wlp2s0
USB=enxa622d9c52ba3
GW_IP=192.168.99.1

# Crear configs
cat > /tmp/hostapd-test.conf << EOF
interface=$WIFI
driver=nl80211
ssid=XTunnel-Test
hw_mode=g
channel=6
auth_algs=1
EOF

cat > /tmp/dnsmasq-test.conf << EOF
interface=$WIFI
dhcp-range=192.168.99.100,192.168.99.200,255.255.255.0,1h
dhcp-option=3,$GW_IP
dhcp-option=6,8.8.8.8
EOF

# Configurar interfaz WiFi
ip link set $WIFI down
ip link set $WIFI up
ip addr flush dev $WIFI
ip addr add $GW_IP/24 dev $WIFI

# NAT
sysctl -w net.ipv4.ip_forward=1
iptables -t nat -F
iptables -F FORWARD
iptables -t nat -A POSTROUTING -o $USB -j MASQUERADE
iptables -A FORWARD -i $WIFI -o $USB -j ACCEPT
iptables -A FORWARD -i $USB -o $WIFI -m state --state RELATED,ESTABLISHED -j ACCEPT

# Iniciar hostapd en ventana screen 0
screen -S xtunnel -dm bash -c "hostapd /tmp/hostapd-test.conf"

# Iniciar dnsmasq en ventana screen 1
screen -S xtunnel -X screen bash -c "dnsmasq -C /tmp/dnsmasq-test.conf --no-daemon"

echo ""
echo "=============================="
echo "  WiFi: XTunnel-Test"
echo "  Sin contrasena"
echo "  Gateway: $GW_IP"
echo "=============================="
echo ""
echo "Para ver las ventanas: screen -r xtunnel"
echo "Ctrl+A luego N para cambiar ventana"

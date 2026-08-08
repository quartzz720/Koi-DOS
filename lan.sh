#!/usr/bin/env bash
# Bring up a direct cable between this machine and the one running Koi-DOS.
#
# Touches enp7s0 and nothing else. The RNDIS interface carrying this machine's
# internet is checked before and after, and the script refuses to finish
# quietly if the default route moved.
set -u

ETH=enp7s0
HERE=192.168.50.1/24
THERE=192.168.50.2
PORT=5555

before=$(ip route show default)
echo "default route now : ${before:-none}"

case "$before" in
  *enp7s0*) echo "REFUSING: the default route is already on $ETH." >&2; exit 1 ;;
esac

nmcli device set "$ETH" managed no 2>/dev/null || true
ip addr flush dev "$ETH" 2>/dev/null
ip addr add "$HERE" dev "$ETH"
ip link set "$ETH" up
firewall-cmd --add-port=$PORT/udp >/dev/null 2>&1 || true

after=$(ip route show default)
echo "default route after: ${after:-none}"
if [ "$before" != "$after" ]; then
    echo "WARNING: the default route changed. Your internet may have moved." >&2
fi

echo
ip -br addr show "$ETH"
echo
echo "Now on Koi-DOS:  net set $THERE 255.255.255.0"
echo "Then here:       ping -c3 $THERE"
echo "To catch a log:  nc -lu -p $PORT > koi.log"

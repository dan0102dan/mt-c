#!/bin/sh
# Real isolated LAN -> router -> policy-selected egress integration test.
set -eu
CLIENT=mtci-client
SERVER=mtci-server
DEST=198.51.100.77
LAN=br-lan
OUT=mtci-out
log() { echo "mt-c routing smoke: $*" >&2; }
die() { log "FAIL: $*"; exit 1; }
cleanup() {
  ip netns del "$CLIENT" 2>/dev/null || :
  ip netns del "$SERVER" 2>/dev/null || :
  ip link del mtci-lan 2>/dev/null || :
  ip link del "$OUT" 2>/dev/null || :
  rm -rf /etc/netns/"$CLIENT"
  # Restore only the test-specific firewall sections on this disposable VM.
  uci -q delete firewall.mtci_smoke 2>/dev/null || :
  uci -q delete firewall.mtci_forward 2>/dev/null || :
  uci -q commit firewall 2>/dev/null || :
  /etc/init.d/firewall reload >/dev/null 2>&1 || :
  rm -f /etc/dnsmasq.d/mtci-routing.conf
  /etc/init.d/dnsmasq restart >/dev/null 2>&1 || :
}
trap cleanup EXIT

# Requires real virtual Ethernet and namespaces: do not silently simulate.
command -v ip >/dev/null || die "iproute2 missing"
ip netns add "$CLIENT"
ip netns add "$SERVER"
ip link add mtci-lan type veth peer name mtci-client0
ip link set mtci-client0 netns "$CLIENT"
ip link set mtci-lan master "$LAN"
ip link set mtci-lan up
ip -n "$CLIENT" link set lo up
ip -n "$CLIENT" link set mtci-client0 up
ip -n "$CLIENT" addr add 192.168.1.250/24 dev mtci-client0
ip -n "$CLIENT" route add default via 192.168.1.1
mkdir -p /etc/netns/"$CLIENT"
printf "nameserver 192.168.1.1\\n" > /etc/netns/"$CLIENT"/resolv.conf

ip link add "$OUT" type veth peer name mtci-server0
ip link set mtci-server0 netns "$SERVER"
ip link set "$OUT" up
ip addr add 192.0.2.1/30 dev "$OUT"
ip -n "$SERVER" link set lo up
ip -n "$SERVER" link set mtci-server0 up
ip -n "$SERVER" addr add 192.0.2.2/30 dev mtci-server0
ip -n "$SERVER" addr add "$DEST"/32 dev mtci-server0
ip -n "$SERVER" route add default via 192.0.2.1
# OpenWrt fw4 forwards LAN only to declared destination zones. Without
# this ephemeral zone, an otherwise-correct mt-c policy route hits fw4's
# inter-zone REJECT (wget reports "Operation not permitted").  Keep the
# application responsible for selecting mtci-out: do not add any main-table
# route to DEST and do not disable the firewall globally.
uci -q set firewall.mtci_smoke=zone
uci -q set firewall.mtci_smoke.name=mtci_smoke
uci -q add_list firewall.mtci_smoke.device="$OUT"
uci -q set firewall.mtci_smoke.input=REJECT
uci -q set firewall.mtci_smoke.output=ACCEPT
uci -q set firewall.mtci_smoke.forward=REJECT
uci -q set firewall.mtci_forward=forwarding
uci -q set firewall.mtci_forward.src=lan
uci -q set firewall.mtci_forward.dest=mtci_smoke
uci commit firewall
/etc/init.d/firewall reload
# Confirm the fw4 rule was generated and loaded, not just staged by uci.
nft list chain inet fw4 forward_lan | grep -q 'mtci_smoke' ||
  die "fw4 lan -> mtci_smoke forwarding rule missing"

# Deterministic DNS response provided by local dnsmasq. The mt-c DNS proxy
# still receives the client's query through the router's port-53 redirect.
mkdir -p /etc/dnsmasq.d
printf 'address=/example.com/%s\n' "$DEST" > /etc/dnsmasq.d/mtci-routing.conf
# Add explicit config include rather than relying on distro-specific includes.
uci set dhcp.@dnsmasq[0].confdir=/etc/dnsmasq.d
/etc/init.d/dnsmasq restart
log "isolated client/server links ready"

# Wait for caller to add domain rule over HTTP API after setup.
touch /tmp/mtci-ready
for i in $(seq 1 60); do
  [ -e /tmp/mtci-go ] && break
  sleep 1
done
[ -e /tmp/mtci-go ] || die "group not configured by host"
mkdir -p /tmp/mtci-www
printf "route-ok\\n" > /tmp/mtci-www/index.html
ip netns exec "$SERVER" uhttpd -p "$DEST":18080 -h /tmp/mtci-www
sleep 1
ip netns exec "$SERVER" wget -T 5 -q -O /tmp/mtci-local-body "http://$DEST:18080/" ||
  die "test HTTP server not reachable even from its own namespace"
grep -q "route-ok" /tmp/mtci-local-body || die "test HTTP server returned unexpected body"
ip netns exec "$CLIENT" nslookup example.com 192.168.1.1 > /tmp/mtci-dns.out 2>&1 || { cat /tmp/mtci-dns.out; die "client DNS failed"; }
grep -q "$DEST" /tmp/mtci-dns.out || { cat /tmp/mtci-dns.out; die "unexpected DNS answer"; }
ipset list -name | grep '^mt_' >/tmp/mtci-ipsets || die "no mt-c ipsets"
found=0
for setname in $(cat /tmp/mtci-ipsets); do
  if ipset test "$setname" "$DEST" >/dev/null 2>&1; then found=1; break; fi
done
[ "$found" = 1 ] || die "example.com IP not populated into ipset"
ip -4 rule show | grep 'fwmark' >/tmp/mtci-rules || die "no fwmark policy rule"
# The direct connected route for 192.0.2.0/30 is insufficient evidence:
# require a policy table default route selected by mt-c.
ip -4 rule show | grep -q 'fwmark' || die "missing policy rule"
ip -4 route show table all | grep -q "default dev $OUT" ||
  die "missing default route through selected egress"
# The HTTP request originates from the client namespace and must traverse
# PREROUTING, fwmark policy and the selected outbound veth into server ns.
if ! ip netns exec "$CLIENT" wget -T 8 -q -O /tmp/mtci-body "http://example.com:18080/"; then
  log "forwarding diagnostics:"
  ip -n "$CLIENT" route get "$DEST" >&2 || :
  ip -4 rule show >&2 || :
  ip -4 route show table all >&2 || :
  ip -s link show dev "$OUT" >&2 || :
  ip -n "$SERVER" -s link show dev mtci-server0 >&2 || :
  nft -a list chain inet fw4 forward_lan >&2 || :
  iptables -t mangle -nvL PREROUTING >&2 || :
  iptables -t filter -nvL FORWARD >&2 || :
  die "client packet did not reach policy egress"
fi
grep -q "route-ok" /tmp/mtci-body || die "unexpected HTTP response"
log "PASS: client DNS, ipset, policy route and HTTP delivery through $OUT"

#!/bin/sh
# Real LAN -> mt-c DNS/ipset -> marked policy route -> selected egress.
set -eu

CLIENT=mtci-client
SERVER=mtci-server
DEST=198.51.100.77
LAN=br-lan
OUT=mtci-out
SERVER_PID=
DNSMASQ_TOUCHED=0
OLD_CONFDIR_PRESENT=0
OLD_CONFDIR=

log() { echo "mt-c routing smoke: $*" >&2; }
die() { log "FAIL: $*"; exit 1; }
cleanup() {
  if [ -n "$SERVER_PID" ]; then
    kill "$SERVER_PID" 2>/dev/null || :
    wait "$SERVER_PID" 2>/dev/null || :
  fi
  ip -4 route del blackhole "$DEST"/32 2>/dev/null || :
  ip netns del "$CLIENT" 2>/dev/null || :
  ip netns del "$SERVER" 2>/dev/null || :
  ip link del mtci-lan 2>/dev/null || :
  ip link del "$OUT" 2>/dev/null || :
  rm -rf /etc/netns/"$CLIENT"
  uci -q delete firewall.mtci_smoke 2>/dev/null || :
  uci -q delete firewall.mtci_forward 2>/dev/null || :
  uci -q commit firewall 2>/dev/null || :
  /etc/init.d/firewall reload >/dev/null 2>&1 || :
  if [ "$DNSMASQ_TOUCHED" -eq 1 ]; then
    if [ "$OLD_CONFDIR_PRESENT" -eq 1 ]; then
      uci -q set "dhcp.@dnsmasq[0].confdir=$OLD_CONFDIR" || :
    else
      uci -q delete 'dhcp.@dnsmasq[0].confdir' || :
    fi
    rm -f /etc/dnsmasq.d/mtci-routing.conf
    /etc/init.d/dnsmasq restart >/dev/null 2>&1 || :
  fi
}
trap cleanup EXIT

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
printf 'nameserver 192.168.1.1\n' > /etc/netns/"$CLIENT"/resolv.conf

ip link add "$OUT" type veth peer name mtci-server0
ip link set mtci-server0 netns "$SERVER"
ip link set "$OUT" up
ip addr add 192.0.2.1/30 dev "$OUT"
ip -n "$SERVER" link set lo up
ip -n "$SERVER" link set mtci-server0 up
ip -n "$SERVER" addr add 192.0.2.2/30 dev mtci-server0
ip -n "$SERVER" addr add "$DEST"/32 dev mtci-server0
ip -n "$SERVER" route add default via 192.0.2.1

# The temporary egress needs explicit fw4 LAN forwarding permission.
# This does not add a route or mark: policy selection is still mt-c's job.
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
nft list chain inet fw4 forward_lan | grep -q mtci_smoke ||
  die "fw4 LAN forwarding rule missing"

# The DNS proxy intercepts the client's port-53 request. Local dnsmasq
# returns a deterministic A record, and never forwards AAAA or other
# example.com types to public upstream resolvers (dnsmasq 2.86+).
if OLD_CONFDIR=$(uci -q get 'dhcp.@dnsmasq[0].confdir'); then
  OLD_CONFDIR_PRESENT=1
fi
DNSMASQ_TOUCHED=1
mkdir -p /etc/dnsmasq.d
printf 'address=/example.com/%s\nlocal=/example.com/\n' "$DEST" \
  > /etc/dnsmasq.d/mtci-routing.conf
uci set 'dhcp.@dnsmasq[0].confdir=/etc/dnsmasq.d'
/etc/init.d/dnsmasq restart

# Run the HTTP server in foreground under a tracked PID for reliable cleanup.
mkdir -p /tmp/mtci-www
printf 'route-ok\n' > /tmp/mtci-www/index.html
ip netns exec "$SERVER" uhttpd -f -p "$DEST":18080 -h /tmp/mtci-www \
  > /tmp/mtci-uhttpd.log 2>&1 &
SERVER_PID=$!
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null ||
  { cat /tmp/mtci-uhttpd.log; die "test server exited"; }
ip netns exec "$SERVER" wget -T 5 -q -O /tmp/mtci-local-body \
  "http://$DEST:18080/" || die "test server not reachable within its namespace"
grep -q 'route-ok' /tmp/mtci-local-body ||
  die "test server returned unexpected content"

# A main-table blackhole makes the negative control deterministic and
# makes the positive result depend on mt-c's marked policy route taking
# precedence over main. No fake mark, extra route or ipset entry is added.
ip -4 route add blackhole "$DEST"/32
if ip netns exec "$CLIENT" wget -T 3 -q -O /tmp/mtci-before \
  "http://$DEST:18080/"; then
  die "destination reachable before the mt-c rule was created"
fi
log "PASS: destination blocked before adding domain group"

# Host creates the group through the real API and provides its returned ID.
touch /tmp/mtci-ready
for i in $(seq 1 60); do
  [ -s /tmp/mtci-group-id ] && break
  sleep 1
done
[ -s /tmp/mtci-group-id ] || die "no group ID from host API"
GROUP_ID=$(cat /tmp/mtci-group-id)
printf '%s\n' "$GROUP_ID" | grep -Eq '^[0-9a-f]{8}$' ||
  die "invalid group ID from API"
SET_NAME=mt_"$GROUP_ID"_4
CHAIN_NAME=MT_"$GROUP_ID"

ip netns exec "$CLIENT" nslookup example.com 192.168.1.1 \
  > /tmp/mtci-dns.out 2>&1 ||
  { cat /tmp/mtci-dns.out; die "client DNS failed"; }
grep -q "$DEST" /tmp/mtci-dns.out ||
  { cat /tmp/mtci-dns.out; die "unexpected DNS answer"; }
ipset test "$SET_NAME" "$DEST" ||
  die "example.com IP missing from its own group's ipset"

# Match the exact group chain and mark, rather than unrelated mt_* sets or
# any other fwmark policy rule present in OpenWrt.
MANGLE_RULES=$(iptables -t mangle -S "$CHAIN_NAME") ||
  die "group-specific mangle chain missing"
printf '%s\n' "$MANGLE_RULES" | grep -F -- "--match-set $SET_NAME dst" |
  grep -q -- '-j MARK' || die "group-specific packet mark rule missing"
MARK=$(printf '%s\n' "$MANGLE_RULES" | awk '/-j MARK / {
  for (i = 1; i <= NF; i++)
    if ($i == "--set-mark" || $i == "--set-xmark") {
      split($(i+1), value, "/"); print value[1]; exit
    }
}')
[ -n "$MARK" ] || die "group mark not found in mangle chain"
MARK_DEC=$(printf '%u' "$MARK")
MARK_HEX=$(printf '0x%x' "$MARK")
ip -4 rule show | grep -F -q "fwmark $MARK_HEX" ||
  die "group-specific policy rule missing"
ip -4 route show table "$MARK_DEC" | grep -q "default dev $OUT" ||
  die "group policy table does not use selected egress"

# A real domain HTTP request from a separate LAN namespace must override
# the main-table blackhole through mt-c's PREROUTING fwmark policy.
if ! ip netns exec "$CLIENT" wget -T 8 -q -O /tmp/mtci-body \
  "http://example.com:18080/"; then
  log "forwarding diagnostics:"
  ip -4 rule show >&2 || :
  ip -4 route show table "$MARK_DEC" >&2 || :
  ip -4 route show table main >&2 || :
  ip -s link show dev "$OUT" >&2 || :
  ip -n "$SERVER" -s link show dev mtci-server0 >&2 || :
  nft -a list chain inet fw4 forward_lan >&2 || :
  iptables -t mangle -nvL PREROUTING >&2 || :
  iptables -t filter -nvL FORWARD >&2 || :
  cat /tmp/mtci-uhttpd.log >&2 || :
  die "client HTTP packet did not reach selected egress"
fi
grep -q 'route-ok' /tmp/mtci-body ||
  die "unexpected HTTP response"
log "PASS: client DNS, group ipset, fwmark and HTTP egress via $OUT"

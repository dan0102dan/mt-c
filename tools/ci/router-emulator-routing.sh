#!/bin/sh
# Real LAN -> mt-c DNS/ipset -> marked policy route -> selected egress.
set -eu

CLIENT=mtci-client
SERVER=mtci-server
DEST=198.51.100.77
DEST6=2001:db8:77::77
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
  ip -6 route del blackhole "$DEST6"/128 from 2001:db8:1::/64 2>/dev/null || :
  ip -6 route del 2001:db8:77::/64 via 2001:db8:2::2 dev "$OUT" 2>/dev/null || :
  ip -6 addr del 2001:db8:1::1/64 dev "$LAN" 2>/dev/null || :
  ip netns del "$CLIENT" 2>/dev/null || :
  ip netns del "$SERVER" 2>/dev/null || :
  ip link del mtci-lan 2>/dev/null || :
  ip link del "$OUT" 2>/dev/null || :
  rm -rf /etc/netns/"$CLIENT"
  uci -q delete firewall.mtci_smoke 2>/dev/null || :
  uci -q delete firewall.mtci_forward 2>/dev/null || :
  uci -q delete firewall.mtci_ndp 2>/dev/null || :
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
ip -6 addr add 2001:db8:1::1/64 dev "$LAN" nodad
ip -n "$CLIENT" -6 addr add 2001:db8:1::250/64 dev mtci-client0 nodad
ip -n "$CLIENT" -6 route add default via 2001:db8:1::1
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
ip -6 addr add 2001:db8:2::1/64 dev "$OUT" nodad
ip -n "$SERVER" -6 addr add 2001:db8:2::2/64 dev mtci-server0 nodad
ip -n "$SERVER" -6 addr add "$DEST6"/128 dev mtci-server0 nodad
ip -n "$SERVER" -6 route add default via 2001:db8:2::1
# Keep the server subnet reachable for IPv6 neighbor discovery/source
# validation. The source-specific blackhole below still blocks LAN clients.
ip -6 route add 2001:db8:77::/64 via 2001:db8:2::2 dev "$OUT"

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
# Like a real IPv6 WAN zone, the isolated server zone must allow neighbor
# discovery even though other input to the router is rejected.
uci -q set firewall.mtci_ndp=rule
uci -q set firewall.mtci_ndp.src=mtci_smoke
uci -q set firewall.mtci_ndp.family=ipv6
uci -q set firewall.mtci_ndp.proto=icmp
uci -q add_list firewall.mtci_ndp.icmp_type=neighbour-solicitation
uci -q add_list firewall.mtci_ndp.icmp_type=neighbour-advertisement
uci -q set firewall.mtci_ndp.target=ACCEPT
uci commit firewall
/etc/init.d/firewall reload
nft list chain inet fw4 forward_lan | grep -q mtci_smoke ||
  die "fw4 LAN forwarding rule missing"

# The DNS proxy intercepts the client's port-53 request. Local dnsmasq
# returns a deterministic A record, and never forwards AAAA or other
# mtci.test types to public upstream resolvers (dnsmasq 2.86+).
if OLD_CONFDIR=$(uci -q get 'dhcp.@dnsmasq[0].confdir'); then
  OLD_CONFDIR_PRESENT=1
fi
DNSMASQ_TOUCHED=1
mkdir -p /etc/dnsmasq.d
printf 'address=/mtci.test/%s\nlocal=/mtci.test/\n' "$DEST" \
  > /etc/dnsmasq.d/mtci-routing.conf
uci set 'dhcp.@dnsmasq[0].confdir=/etc/dnsmasq.d'
/etc/init.d/dnsmasq restart

# Run the HTTP server in foreground under a tracked PID for reliable cleanup.
mkdir -p /tmp/mtci-www
printf 'route-ok\n' > /tmp/mtci-www/index.html
ip netns exec "$SERVER" uhttpd -f -p "$DEST":18080 -p "[$DEST6]:18080" -h /tmp/mtci-www \
  > /tmp/mtci-uhttpd.log 2>&1 &
SERVER_PID=$!
sleep 1
kill -0 "$SERVER_PID" 2>/dev/null ||
  { cat /tmp/mtci-uhttpd.log; die "test server exited"; }
ip netns exec "$SERVER" wget -T 5 -q -O /tmp/mtci-local-body \
  "http://$DEST:18080/" || die "test server not reachable within its namespace"
grep -q 'route-ok' /tmp/mtci-local-body ||
  die "test server returned unexpected content"

# Main-table blackholes make success depend on mt-c's policy routing,
# rather than the router's connected route or a permissive firewall.
ip -4 route add blackhole "$DEST"/32
ip -6 route add blackhole "$DEST6"/128 from 2001:db8:1::/64
API=http://127.0.0.1:8080/api/v1/groups

client_http() {
  ip netns exec "$CLIENT" curl -"$FAMILY" --noproxy '*' \
    --silent --show-error --fail --connect-timeout 2 --max-time 5 \
    -o /tmp/mtci-body "$1"
}
expect_blocked() {
  if client_http "$URL"; then
    die "$TYPE: destination reachable $1"
  fi
  log "PASS: $TYPE destination blocked $1"
}
resolve() {
  ip netns exec "$CLIENT" nslookup "$1" 192.168.1.1 \
    > /tmp/mtci-dns.out 2>&1 ||
    { cat /tmp/mtci-dns.out; die "$TYPE: client DNS failed for $1"; }
  grep -Fq "$DEST" /tmp/mtci-dns.out ||
    { cat /tmp/mtci-dns.out; die "$TYPE: unexpected DNS answer for $1"; }
}

# Run each type alone. Shared destination addresses must not let an earlier
# successful rule mask a broken matcher, subnet sync or IPv6 route.
while IFS='|' read -r TYPE GROUP_ID RULE QUERY MISS FAMILY; do
  SET_NAME=mt_"$GROUP_ID"_"$FAMILY"
  CHAIN_NAME=MT_"$GROUP_ID"
  if [ "$FAMILY" = 6 ]; then
    TARGET=$DEST6
    URL="http://[$DEST6]:18080/"
    IPTABLES=ip6tables
    # Prove the IPv6 server is listening before relying on a negative control.
    ip netns exec "$SERVER" curl -6 --noproxy '*' --silent --show-error \
      --fail --max-time 5 "http://[$DEST6]:18080/" | grep -q '^route-ok$' ||
      die "$TYPE: IPv6 test server is not listening"
  else
    TARGET=$DEST
    URL="http://$DEST:18080/"
    IPTABLES=iptables
  fi
  expect_blocked 'before group creation'

  cat > /tmp/mtci-group-request.json <<EOF
{"id":"$GROUP_ID","name":"CI $TYPE policy","interface":"$OUT","enable":true,"rules":[{"name":"CI $TYPE","type":"$TYPE","rule":"$RULE","enable":true}]}
EOF
  curl --silent --show-error --fail --connect-timeout 5 --max-time 35 \
    --dump-header /tmp/mtci-post-headers \
    --output /tmp/mtci-group-response.json \
    --header 'Content-Type: application/json' \
    --data-binary @/tmp/mtci-group-request.json "$API" ||
    { cat /tmp/mtci-group-response.json; die "$TYPE: group creation failed"; }
  grep -Eq '^HTTP/[0-9.]+ 200([[:space:]]|$)' /tmp/mtci-post-headers &&
    grep -q "$GROUP_ID" /tmp/mtci-group-response.json ||
    { cat /tmp/mtci-group-response.json; die "$TYPE: unexpected group response"; }

  if [ "$QUERY" != '-' ]; then
    # A nearby nonmatching domain resolves to the SAME IP. It must not add
    # that address to this group's set or make the destination reachable.
    resolve "$MISS"
    if ipset test "$SET_NAME" "$TARGET"; then
      die "$TYPE: nonmatching domain populated the group ipset"
    fi
    expect_blocked 'after nonmatching DNS request'
    resolve "$QUERY"
    URL="http://$QUERY:18080/"
  fi
  # subnet/subnet6 must populate their ipsets without any DNS request.
  for attempt in $(seq 1 15); do
    ipset test "$SET_NAME" "$TARGET" 2>/dev/null && break
    sleep 1
  done
  ipset test "$SET_NAME" "$TARGET" || die "$TYPE: target missing from group ipset"

  MANGLE_RULES=$($IPTABLES -t mangle -S "$CHAIN_NAME") ||
    die "$TYPE: group-specific mangle chain missing"
  printf '%s\n' "$MANGLE_RULES" | grep -F -- "--match-set $SET_NAME dst" |
    grep -q -- '-j MARK' || die "$TYPE: group-specific packet mark rule missing"
  MARK=$(printf '%s\n' "$MANGLE_RULES" | awk '/-j MARK / {
    for (i = 1; i <= NF; i++)
      if ($i == "--set-mark" || $i == "--set-xmark") {
        split($(i+1), value, "/"); print value[1]; exit
      }
  }')
  [ -n "$MARK" ] || die "$TYPE: group mark not found"
  MARK_DEC=$(printf '%u' "$MARK")
  MARK_HEX=$(printf '0x%x' "$MARK")
  ip -"$FAMILY" rule show | grep -Fq "fwmark $MARK_HEX" ||
    die "$TYPE: group-specific policy rule missing"
  ROUTES=$(ip -"$FAMILY" route show table "$MARK_DEC")
  printf '%s\n' "$ROUTES" | grep '^default ' |
    grep -Eq "(^|[[:space:]])dev $OUT([[:space:]]|$)" ||
    { printf '%s\n' "$ROUTES"; die "$TYPE: group policy table does not use selected egress"; }

  if ! client_http "$URL"; then
    log "forwarding diagnostics for $TYPE:"
    ip -"$FAMILY" rule show >&2 || :
    ip -"$FAMILY" route show table "$MARK_DEC" >&2 || :
    ip -"$FAMILY" route show table main >&2 || :
    ip -"$FAMILY" neigh show >&2 || :
    ip -n "$SERVER" -"$FAMILY" route show >&2 || :
    ip -s link show dev "$OUT" >&2 || :
    $IPTABLES -t mangle -nvL PREROUTING >&2 || :
    $IPTABLES -t filter -nvL FORWARD >&2 || :
    $IPTABLES -t mangle -nvL "$CHAIN_NAME" >&2 || :
    $IPTABLES -t filter -nvL "$CHAIN_NAME" >&2 || :
    $IPTABLES -t nat -nvL "$CHAIN_NAME" >&2 || :
    nft list chain inet fw4 forward_lan >&2 || :
    cat /proc/sys/net/ipv6/conf/all/forwarding >&2 || :
    cat /tmp/mtci-uhttpd.log >&2 || :
    die "$TYPE: client HTTP packet did not reach selected egress"
  fi
  grep -qx 'route-ok' /tmp/mtci-body || die "$TYPE: unexpected HTTP response"
  log "PASS: $TYPE ipset, fwmark and IPv$FAMILY client HTTP egress via $OUT"

  curl --silent --show-error --fail --connect-timeout 5 --max-time 35 \
    -X DELETE "$API/$GROUP_ID" > /tmp/mtci-delete-response.json ||
    die "$TYPE: group deletion failed"
  if ipset list "$SET_NAME" >/dev/null 2>&1; then
    die "$TYPE: group deletion left its ipset behind"
  fi
  expect_blocked 'after group deletion'
done <<'CASES'
domain|c1c0ff01|exact.mtci.test|exact.mtci.test|child.exact.mtci.test|4
namespace|c1c0ff02|ns.mtci.test|child.ns.mtci.test|notns.mtci.test|4
wildcard|c1c0ff03|wild*.mtci.test|wildcard.mtci.test|other.mtci.test|4
regex|c1c0ff04|^regex[0-9]+[.]mtci[.]test$|regex42.mtci.test|regexxx.mtci.test|4
subnet|c1c0ff05|198.51.100.0/24|-|-|4
subnet6|c1c0ff06|2001:db8:77::/64|-|-|6
CASES
log "PASS: all six rule types forwarded real client traffic and stopped after deletion"

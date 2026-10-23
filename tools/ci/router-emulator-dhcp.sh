#!/bin/sh
# Exercise actual DHCP -> system host hints -> mt-c journal metadata.
# This runs only inside the disposable OpenWrt guest, never on the CI host.
set -eu
NS=mtci-dhcp
VETH=mtci-dhcp-lan
PEER=mtci-dhcp0
MAC=02:00:00:00:28:31
API=http://127.0.0.1:8080/api/v1/system/dns
DIR=/tmp/mtci-dhcp
DHCP_PID=
SSE_PID=
CAPTURE_BEFORE=
STREAM=0
DOMAIN=$(uci -q get 'dhcp.@dnsmasq[0].domain' || :)
HINT_NAME=
mkdir -p "$DIR"
log() { echo "mt-c DHCP smoke: $*"; }
die() { log "FAIL: $*" >&2; exit 1; }
api() { curl --silent --show-error --fail --connect-timeout 3 --max-time 5 "$@"; }
stop_pid() { kill "$1" 2>/dev/null || :; wait "$1" 2>/dev/null || :; }
cleanup() {
  result=$?
  if [ "$result" -ne 0 ]; then
    for file in "$DIR"/*.log "$DIR"/*.json "$DIR"/*.sse; do
      [ -f "$file" ] || continue
      echo "--- $file ---"
      case "$file" in
        */hints.json) cat "$file" || : ;;
        *) tail -n 50 "$file" || : ;;
      esac
    done
    logread | tail -n 60 || :
  fi
  [ -z "$SSE_PID" ] || stop_pid "$SSE_PID"
  [ -z "$DHCP_PID" ] || stop_pid "$DHCP_PID"
  if [ -n "$CAPTURE_BEFORE" ]; then
    api -X PUT -H 'Content-Type: application/json' \
      -d "{\"enabled\":$CAPTURE_BEFORE}" "$API/capture" >/dev/null || :
  fi
  ip netns del "$NS" 2>/dev/null || :
  ip link del "$VETH" 2>/dev/null || :
  return "$result"
}
trap cleanup EXIT

# Do not install the inventory provider here: missing APK dependencies must fail.
command -v jsonfilter >/dev/null || die "OpenWrt jsonfilter missing"
ubus -t 5 call luci-rpc getHostHints '{}' > "$DIR/hints.json" ||
  die "APK did not provide a working luci-rpc.getHostHints"
DAEMON_PID=$(pidof magitrickled)
[ -n "$DAEMON_PID" ] || die "mt-c is not running"
CAPTURE_BEFORE=$(api "$API/capture" | jsonfilter -e '@.enabled')
case "$CAPTURE_BEFORE" in true|false) ;; *) die "invalid capture status";; esac
api -X PUT -H 'Content-Type: application/json' -d '{"enabled":true}' \
  "$API/capture" > "$DIR/capture.json"
[ "$(jsonfilter -i "$DIR/capture.json" -e '@.enabled')" = true ] ||
  die "could not enable DNS capture"

# Keep a real journal viewer connected: inventory discovery is lazy and
# clients_begin/chunk/end are what the WebUI actually consumes.
start_stream() {
  [ -z "$SSE_PID" ] || stop_pid "$SSE_PID"
  STREAM=$((STREAM + 1))
  SSE_FILE="$DIR/viewer-$STREAM.sse"
  curl --silent --show-error --fail --no-buffer --connect-timeout 3 \
    --max-time 240 "$API/stream" > "$SSE_FILE" 2> "$DIR/viewer-$STREAM.log" &
  SSE_PID=$!
  for attempt in $(seq 1 15); do
    kill -0 "$SSE_PID" 2>/dev/null || die "journal SSE disconnected"
    grep -q '^event: snapshot_end' "$SSE_FILE" && return
    sleep 1
  done
  die "journal SSE did not complete its initial snapshot"
}
# Only expose the last complete metadata transaction, not a partial transfer
# or a stale matching chunk from before a rename. A new begin discards staging.
latest_clients() {
  awk '
    /^event: / { event=substr($0,8); next }
    /^data: / { data=substr($0,7); next }
    /^$/ {
      if (event=="clients_begin") { pending=""; active=1 }
      else if (event=="clients_chunk" && active) { pending=pending data "\n" }
      else if (event=="clients_end" && active) { complete=pending; active=0 }
      event=""; data=""
    }
    END { printf "%s", complete }
  ' "$SSE_FILE"
}
wait_label() {
  expected=$1
  for attempt in $(seq 1 40); do
    kill -0 "$SSE_PID" 2>/dev/null || die "journal SSE disconnected"
    names=$(latest_clients | while IFS= read -r chunk; do
      jsonfilter -s "$chunk" -e "@.clients[@.ip=\"$LEASE_IP\"].name" || :
    done)
    [ "$names" = "$expected" ] && return
    sleep 1
  done
  die "mt-c did not publish $LEASE_IP -> $expected in a complete SSE snapshot"
}
start_stream

ip netns add "$NS"
ip link add "$VETH" type veth peer name "$PEER"
ip link set "$PEER" netns "$NS"
ip link set "$VETH" master br-lan
ip link set "$VETH" up
ip -n "$NS" link set lo up
ip -n "$NS" link set "$PEER" address "$MAC"
ip -n "$NS" link set "$PEER" up
# The DHCP hook applies only the received lease to the client's namespace.
# Never call netifd's router-wide DHCP hook or overwrite the router's resolver.
cat > "$DIR/lease-hook.sh" <<'EOF'
#!/bin/sh
set -eu
case "$1" in
  deconfig) ip -4 addr flush dev "$interface" ;;
  bound|renew)
    ip -4 addr flush dev "$interface"
    ip -4 addr add "$ip/$subnet" dev "$interface"
    set -- $router
    ip -4 route replace default via "$1" dev "$interface"
    set -- $dns
    printf '%s\n' "$1" > /tmp/mtci-dhcp/dns
    printf '%s\n' "$ip" > /tmp/mtci-dhcp/ip.new
    mv /tmp/mtci-dhcp/ip.new /tmp/mtci-dhcp/ip
    ;;
esac
EOF
chmod 700 "$DIR/lease-hook.sh"
acquire() {
  hostname=$1
  [ -z "$DHCP_PID" ] || stop_pid "$DHCP_PID"
  DHCP_PID=
  rm -f "$DIR/ip" "$DIR/dns"
  set -- -f -n -R -t 5 -T 2 -i "$PEER" -s "$DIR/lease-hook.sh" -x "hostname:$hostname"
  [ -z "${LEASE_IP:-}" ] || set -- "$@" -r "$LEASE_IP"
  ip netns exec "$NS" udhcpc "$@" > "$DIR/dhcp-$hostname.log" 2>&1 &
  DHCP_PID=$!
  for attempt in $(seq 1 20); do
    kill -0 "$DHCP_PID" 2>/dev/null || die "DHCP client exited without a lease"
    [ -s "$DIR/ip" ] && break
    sleep 1
  done
  [ -s "$DIR/ip" ] || die "DHCP server did not grant a lease"
  new_ip=$(cat "$DIR/ip")
  [ -z "${LEASE_IP:-}" ] || [ "$new_ip" = "$LEASE_IP" ] ||
    die "renewal changed the address; cannot test stale names on the same IP"
  LEASE_IP=$new_ip
  for attempt in $(seq 1 15); do
    ubus -t 5 call luci-rpc getHostHints '{}' > "$DIR/hints.json" ||
      die "system host-hint provider failed"
    hint=$(jsonfilter -i "$DIR/hints.json" -e "@[\"$MAC\"].name" || :)
    # LuCI may return the short DHCP hostname or its local DNS-qualified
    # name. Validate identity first, then require mt-c to preserve exactly
    # the name supplied by the system; never accept a stale name on rename.
    if { [ "$hint" = "$hostname" ] || [ "$hint" = "$hostname${DOMAIN:+.$DOMAIN}" ]; } &&
        jsonfilter -i "$DIR/hints.json" -e "@[\"$MAC\"].ipaddrs[*]" | grep -Fxq "$LEASE_IP"; then
      HINT_NAME=$hint
      log "PASS: actual DHCP lease and system hint: $MAC / $LEASE_IP / $HINT_NAME"
      return
    fi
    sleep 1
  done
  die "system host hints did not reflect the real DHCP lease for $hostname"
}

acquire mtci-dhcp-first
wait_label "$HINT_NAME"
QUERY=mtci-dhcp-first${DOMAIN:+.$DOMAIN}
ip netns exec "$NS" nslookup "$QUERY" "$(cat "$DIR/dns")" > "$DIR/dns.log" 2>&1 ||
  die "DHCP client's real DNS request failed"
grep -Fq "$LEASE_IP" "$DIR/dns.log" || die "DNS did not return the DHCP address"
api "$API/queries?limit=128" > "$DIR/queries.json"
QUERY_ID=$(jsonfilter -i "$DIR/queries.json" \
  -e "@.queries[@.client=\"$LEASE_IP\" && @.domain=\"$QUERY\" && @.qtype=1].id" | head -n 1)
case "$QUERY_ID" in ''|*[!0-9]*) die "client DNS request missing from mt-c journal";; esac
[ "$(jsonfilter -i "$DIR/queries.json" -e "@.queries[@.id=$QUERY_ID].client_name")" = "$HINT_NAME" ] ||
  die "journal row has an IP but no correct DHCP device name"
log "PASS: real client DNS request has its DHCP name in the mt-c journal"

# Renew with a new hostname but the same MAC/IP and keep the SAME SSE viewer.
# Do not edit leases, inject ubus notifications or restart mt-c to force a refresh.
acquire mtci-dhcp-renamed
wait_label "$HINT_NAME"
api "$API/queries?limit=128" > "$DIR/queries.json"
[ "$(jsonfilter -i "$DIR/queries.json" -e "@.queries[@.id=$QUERY_ID].client_name")" = "$HINT_NAME" ] ||
  die "historical DNS row retained the old name after DHCP rename"
[ "$(pidof magitrickled)" = "$DAEMON_PID" ] || die "mt-c restarted during DHCP discovery"
log "PASS: DHCP rename updates live SSE and the existing journal row without restart"

# Closing the last viewer releases discovery; reopening must discover again.
start_stream
wait_label "$HINT_NAME"
log "PASS: device name rediscovered after closing and reopening the journal"

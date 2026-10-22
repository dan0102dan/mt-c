# DNS journal: automatic updates and device labels

The DNS toolbar has no Refresh action and no manual GET path. DNS records still arrive on the existing SSE connection. Pause freezes the view by closing that subscription; Resume reconnects and receives the current snapshot. Retry is shown only as error recovery and reconnects SSE, not polling.

## Addresses and names

`::ffff:192.168.1.42` is an IPv4-mapped IPv6 representation. JSON responses and the UI now present it as `192.168.1.42`. Actual IPv6 addresses are retained. The compact ring's binary address representation is unchanged.

API rows may have an additional optional `client_name` string. The table, grouped view and device picker display that label first, with the address as secondary text. Filter values remain normalized addresses; two different devices named "Phone" are not merged. Missing, invalid or ambiguous labels fall back to the address, not a fabricated alias.

Names describe the router's **current** inventory for an address, not a historical assertion that the same physical device owned that address at the time of every retained request. DHCP reassignment cannot be reconstructed from an IP-only journal.

## Providers

- **Keenetic `_kn`:** read-only `GET http://127.0.0.1:79/rci/show/ip/hotspot`. Prefer the Client Lists `name`, then `hostname`. Explicitly inactive hosts are ignored; no MAC-to-name guessing, PTR queries, credentials in URLs, shell commands or router writes are involved. Both array inventory and single-host forms are accepted. In addition to explicit inventory IPs, IPv6 neighbors are joined to inventory names by an exact observed MAC match (see below).
- **OpenWrt:** use native libubus to request `luci-rpc.getDHCPLeases` from the packaged `rpcd-mod-luci` provider. It supplies current dnsmasq/odhcpd IPv4 and IPv6 leases using the router's configured paths. Subscribe to dnsmasq/odhcpd object notifications, DHCP configuration changes and provider lifecycle events. No direct lease-file reader, inotify, subprocess or private ubus protocol implementation is used. Missing/ambiguous hostnames fall back to IP.
- **Other builds:** no provider is instantiated and no router discovery request is made. The Keenetic provider source is compiled only in `_kn` builds.

## Resource and lifecycle constraints

One shared worker fetches metadata off the DNS/event-loop thread, initially when a viewer subscribes. Both platforms use source events and a five-minute full reconciliation while a viewer is connected (see below). OpenWrt owns a native ubus connection on that worker; Keenetic uses the RCI worker and a separate read-only netlink event socket. There is no per-query lookup request and no frontend polling. RCI connection/total timeouts are 500/2000 ms; redirects and environment proxies are disabled. Inventory input is capped at 1 MiB, at most 2048 entries and 127 UTF-8 bytes per name. The cache grows with actual named devices; names are not copied into the ring's stored records.

Only a changed inventory wakes viewers. The existing bounded snapshot protocol then refreshes labels for **already displayed** queries on the same connection. A label change during snapshot transmission restarts staging, avoiding mixed snapshots or duplicated queries. Unchanged inventories do not retransmit history. Failed discovery clears cached labels instead of indefinitely attaching a stale name to a reassigned address. Removing the final viewer releases the inventory; HTTP streams are torn down before the service.

Inventory reconciliation discovers router-side changes; it does not control DNS query delivery. DNS events remain event-driven. No feature here changes routes, policies, config export or persistence.

## Verification

Repository tests added:

- `tests/unit/test_dns_clients.c`: mapped IPv4/true IPv6, UTF-8 labels, alias preference, hostname fallback, inactive/ambiguous hosts and malformed/oversized responses.
- `tests/unit/test_dns_client_service.c`: discovery runs off-loop, concurrent refreshes coalesce, rename/failure/recovery notifications, cache release and generic build isolation.
- Frontend `tests/unit/dns-client-labels.test.ts`: address normalization, optional names, address-keyed filtering, atomic name replacement/removal and event size bounds.
- Frontend `tests/e2e/dns-client-names.spec.ts`: real open mock SSE responses, no Refresh/manual GET, identical aliases on different addresses, renaming/fallback while filtered, desktop/mobile viewports.

Backend verification uses full host builds and unit suites for generic, Keenetic and OpenWrt variants. OpenWrt integration tests use official libubus/libubox and an isolated real ubusd, including sanitizer runs. Target SDK builds remain in CI; host results do not establish physical Keenetic/OpenWrt firmware behavior. Frontend code is unchanged by the provider work.

## References

Keenetic distinguishes the user-defined `name` from the DHCP `hostname`:
https://support.keenetic.ru/eaeu/ultra/kn-1811/ru/45307-accessing-a-network-device-by-its-hostname.html

IPv4-mapped IPv6 format: RFC 4291, section 2.5.5.2:
https://www.rfc-editor.org/rfc/rfc4291.html#section-2.5.5.2

## Independent metadata stream

Client inventories now travel as bounded `clients_begin/chunk/end` events. A
rename or inventory failure atomically updates the client-name map without
serializing the DNS history again. DNS rows carry canonical IP identity, not a
copy of the name in every event. Labels describe the current inventory and
never prove which device owned that IP at the recorded time.

## Keenetic IPv6 neighbor enrichment

A client can send DNS from `fe80::4a17:e738:40c9:6a7` while hotspot inventory
only lists `192.168.5.125`. Comparing those IP strings cannot find its name.
The Keenetic provider now takes a read-only `RTM_GETNEIGH` IPv6 snapshot and
joins `IPv6 -> MAC -> hotspot name/hostname`. The regression fixture uses the
reported Lenovo/Why device, including its link-local and ULA addresses.
An IPv4 lease is not required when the inventory supplies a valid MAC and name.

The same shared metadata worker reads this snapshot, not the DNS loop or a
per-request subprocess. A separate read-only `RTMGRP_NEIGH` subscription on the
event loop invalidates this snapshot after IPv6 neighbor additions, changes or
deletions. Events are coalesced into at most one worker refresh per second; an
event racing an inflight refresh remains pending. Notifications are hints, not
trusted partial updates: each refresh reads a complete bounded dump. There is no probing, reverse DNS, EUI-64 decoding,
routing change or frontend/API format change. MACs are
used transiently for the join and are not stored in the DNS ring or sent to the
browser. The frontend receives ordinary IP/name entries in its existing
independent metadata stream; it keeps the original IPv6 address as identity.

`STALE`, `DELAY`, `PROBE`, `REACHABLE`, `PERMANENT` and resolved `NOARP` entries
with a unicast Ethernet MAC can supply a binding. Failed/incomplete/proxy
entries, missing or non-Ethernet MACs, multicast and unspecified IPs are not
used. Duplicate observations collapse. Conflicting MACs or interface indices
for the same IP suppress enrichment even if the router gives them the same
name: current DNS rows do not carry an ingress interface, so a link-local scope
cannot safely be guessed. Conflicting inventory names retain the existing
IP-only fallback rather than picking an arbitrary host.

The dump has a 500 ms total deadline, checks cancellation at most every 50 ms
while waiting, and is bounded to 1 MiB of wire data / 2048 neighbor entries.
Only a complete, non-interrupted kernel dump is published. Failure or overflow
drops enrichment for that refresh, preserving explicit hotspot IP labels;
it never reuses a stale IPv6 snapshot. Exceeding the existing 2048 client-label
budget similarly falls back to the explicit inventory instead of a partial
join. This platform code is compiled only for Keenetic `_kn` builds.

Hotspot RCI is cached in the worker. A new IPv6 on a known MAC reuses its name
without an HTTP request. A previously unseen unknown MAC requests fresh RCI,
with a five-second minimum interval and a deferred retry when rate limited.
MACs already checked against RCI do not repeatedly trigger HTTP requests merely
because they remain unnamed. The cached inventory and these observations are
bounded and never sent to the browser. Every five minutes a full reconciliation
refreshes both RCI and neighbors, discovers renames, and recovers from missed
notifications. Socket overflow/truncation invalidates the snapshot; an unusable
watch is closed and retried at reconciliation. If subscription is unavailable,
the same five-minute reconciliation remains as fallback.

The watch is opened before initial discovery and closed with the last viewer.
The worker releases its cached RCI on unsubscribe; destruction joins the worker
before freeing cache/watch resources. Reopening while a fetch is inflight
requests fresh discovery instead of publishing that previous viewer's result.

`tests/unit/test_dns_neighbors.c` covers the reported device, multiple IPv6
addresses per MAC, hostname fallback, IPv6-only inventory, inactive/unknown
clients, bad MACs, neighbor states, scope and name conflicts, bounded fallback,
native-endian/unaligned and truncated netlink messages, interrupted/error dumps,
non-Ethernet neighbors and cancellation. Run it via `make test ENTWARE_KN=1`
with a separate `BUILD` directory; it also runs in the existing Keenetic CI and
sanitizer variant. A real-router check must still confirm that the installed
firmware exposes the client's MAC/name through hotspot RCI.


Event/cache regressions are in `test_dns_client_events.c` (burst coalescing,
events during an inflight dump, watch cleanup) and `test_dns_hotspot_cache.c`
(known-MAC reuse, unknown-MAC rate limiting/deduplication, reconciliation,
dump/RCI failures and cancellation).


## Native OpenWrt inventory and events

The OpenWrt package depends on `libubus`, `libubox` and `rpcd-mod-luci`.
`luci-rpc.getDHCPLeases` supplies the initial and reconciled inventory from the
platform's DHCP sources, including configured lease paths and odhcpd IPv6.
This API is present in OpenWrt 24.10; unlike a proposed newer dnsmasq
`get_leases` API, it does not require a recent dnsmasq upgrade. The backend
parses native blobmsg data with libubox and applies its existing IP/name
validation, conflict suppression and 2048-label/1 MiB limits. DHCPv6 addresses
with `/128` are accepted; delegated prefixes are not client identities.

A single metadata worker owns all libubus calls and buffers. It subscribes
before requesting inventory, discovers every `dnsmasq`/`dnsmasq.*` instance and
the `dhcp` object, and follows their removal/recreation via native subscriber
callbacks. DHCP object notifications and `config.change` for `dhcp` invalidate
the snapshot. Events are coalesced for one second, including events arriving
during a query. Other notifications, including DNS connmark events, are ignored.

The broker connection is re-established with bounded exponential backoff after
loss; old object IDs and subscriptions are discarded and rebuilt. Failure
publishes an empty label map instead of keeping stale names. A missing inventory
provider is retried and its arrival triggers discovery. Inventory RPCs have a
two-second response deadline and check shutdown at most every 50 ms while
waiting. Native local broker registration/lookup uses libubus itself.

Lease expiry schedules the next snapshot even when no notification is emitted.
Five-minute reconciliation recovers missed notifications and updates names;
not every supported odhcpd version emits IPv6 lease notifications, so immediate
DHCPv6 change delivery is not promised. Removing the final viewer closes the
worker's ubus connection and releases the map. Viewer generations prevent an
old query from publishing into a reopened journal. Generic/Entware builds have
no ubus dependency or OpenWrt discovery path.

`test_dns_openwrt.c` covers blobmsg IPv4/IPv6 parsing, expiry, permanent leases,
conflicting names, malformed/oversized replies and prefix delegation. Its
integration fixture runs an isolated **real ubusd** plus a separate native
provider process, exercising bursts, configuration changes, provider recreation,
broker restart/resubscription, expiry, RPC timeout/cancellation and unsubscribe/resubscribe. Set
`MT_TEST_UBUSD` to the broker binary and run under root or fakeroot for the
broker's isolated ACL ownership checks. CI builds the official OpenWrt 24.10.0
libubox/ubus revisions for these host tests; firmware builds use each target
SDK's own libraries. This is not a claim of physical-router verification.

Sources:
- [OpenWrt 24.10 luci-rpc implementation](https://github.com/openwrt/luci/blob/openwrt-24.10/libs/rpcd-mod-luci/src/luci.c)
- [Native dnsmasq notifications](https://github.com/imp/dnsmasq/blob/master/src/ubus.c)
- [Native ubus subscriber API](https://github.com/openwrt/ubus/blob/afa57cce0aff82f4a7a0e509d4387ebc23dd3be7/libubus-sub.c)

# DNS journal: automatic updates and device labels

The DNS toolbar has no Refresh action and no manual GET path. DNS records still arrive on the existing SSE connection. Pause freezes the view by closing that subscription; Resume reconnects and receives the current snapshot. Retry is shown only as error recovery and reconnects SSE, not polling.

## Addresses and names

`::ffff:192.168.1.42` is an IPv4-mapped IPv6 representation. JSON responses and the UI now present it as `192.168.1.42`. Actual IPv6 addresses are retained. The compact ring's binary address representation is unchanged.

API rows may have an additional optional `client_name` string. The table, grouped view and device picker display that label first, with the address as secondary text. Filter values remain normalized addresses; two different devices named "Phone" are not merged. Missing, invalid or ambiguous labels fall back to the address, not a fabricated alias.

Names describe the router's **current** inventory for an address, not a historical assertion that the same physical device owned that address at the time of every retained request. DHCP reassignment cannot be reconstructed from an IP-only journal.

## Providers

- **Keenetic `_kn`:** read-only `GET http://127.0.0.1:79/rci/show/ip/hotspot`. Prefer the Client Lists `name`, then `hostname`. Explicitly inactive hosts are ignored; no MAC-to-name guessing, PTR queries, credentials in URLs, shell commands or router writes are involved. Both array inventory and single-host forms are accepted. Only addresses explicitly present in the host inventory are associated with a name.
- **OpenWrt:** read the standard dnsmasq `/tmp/dhcp.leases` file, using non-expired named leases (expiry `0` is permanent). This provides DHCP hostnames, not a promise to reconstruct every LuCI alias. Custom lease paths and odhcpd IPv6 leases are not read in this implementation; unavailable names fall back to IP.
- **Other builds:** no provider is instantiated and no router discovery request is made. The Keenetic provider source is compiled only in `_kn` builds.

## Resource and lifecycle constraints

One shared worker fetches metadata off the DNS/event-loop thread, initially when a viewer subscribes and then every 30 seconds while a viewer is connected. There is no per-query lookup request and no frontend polling. RCI connection/total timeouts are 500/2000 ms; redirects and environment proxies are disabled. Inventory input is capped at 1 MiB, at most 2048 entries and 127 UTF-8 bytes per name. The cache grows with actual named devices; names are not copied into the ring's stored records.

Only a changed inventory wakes viewers. The existing bounded snapshot protocol then refreshes labels for **already displayed** queries on the same connection. A label change during snapshot transmission restarts staging, avoiding mixed snapshots or duplicated queries. Unchanged inventories do not retransmit history. Failed discovery clears cached labels instead of indefinitely attaching a stale name to a reassigned address. Removing the final viewer releases the inventory; HTTP streams are torn down before the service.

The 30-second period is for discovering changes in the router's device inventory, **not** for DNS query delivery. DNS events remain event-driven. No feature here changes routes, policies, config export or persistence.

## Verification

Repository tests added:

- `tests/unit/test_dns_clients.c`: mapped IPv4/true IPv6, UTF-8 labels, alias preference, hostname fallback, inactive/ambiguous hosts, malformed/oversized responses, lease expiry and unnamed leases.
- `tests/unit/test_dns_client_service.c`: discovery runs off-loop, concurrent refreshes coalesce, rename/failure/recovery notifications, cache release and generic build isolation.
- Frontend `tests/unit/dns-client-labels.test.ts`: address normalization, optional names, address-keyed filtering, atomic name replacement/removal and event size bounds.
- Frontend `tests/e2e/dns-client-names.spec.ts`: real open mock SSE responses, no Refresh/manual GET, identical aliases on different addresses, renaming/fallback while filtered, desktop/mobile viewports.

These tests must also be run in the full target checkout. Local isolated verification used the actual changed modules, the installed cJSON/curl libraries, a test-only loop adapter and a Node runner for the pure TypeScript tests; it is not a full firmware/Svelte build or a test on physical Keenetic/OpenWrt hardware.

## References

Keenetic distinguishes the user-defined `name` from the DHCP `hostname`:
https://support.keenetic.ru/eaeu/ultra/kn-1811/ru/45307-accessing-a-network-device-by-its-hostname.html

IPv4-mapped IPv6 format: RFC 4291, section 2.5.5.2:
https://www.rfc-editor.org/rfc/rfc4291.html#section-2.5.5.2

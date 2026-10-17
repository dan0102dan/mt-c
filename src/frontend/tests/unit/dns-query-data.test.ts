import assert from "node:assert/strict";

import {
  dnsQueryType,
  filterDnsQueries,
  groupDnsQueries,
  type DnsQuery,
} from "../../src/modules/settings/dns-query-data";

const rows: DnsQuery[] = [
  { id: 3, time_ms: 300, client: "192.168.1.2", domain: "EXAMPLE.com", qtype: 1, protocol: "udp" },
  { id: 2, time_ms: 200, client: "192.168.1.3", domain: "example.com", qtype: 28, protocol: "tcp" },
  { id: 1, time_ms: 100, client: "192.168.1.2", domain: "another.org", qtype: 65, protocol: "udp" },
];

Deno.test("DNS journal filters clients and domain search together", () => {
  assert.deepEqual(filterDnsQueries(rows, "EXAMPLE", "192.168.1.2").map((row) => row.id), [3]);
  assert.deepEqual(filterDnsQueries(rows, " example ", "").map((row) => row.id), [3, 2]);
  assert.equal(filterDnsQueries(rows, "", "").length, 3);
});

Deno.test("grouping counts all matching queries and distinct clients", () => {
  const grouped = groupDnsQueries(rows);
  assert.equal(grouped.length, 2);
  assert.equal(grouped[0].count, 2);
  assert.deepEqual(grouped[0].clients, ["192.168.1.2", "192.168.1.3"]);
  assert.equal(grouped[0].lastTime, 300);
  assert.equal(groupDnsQueries(filterDnsQueries(rows, "example", "192.168.1.2"))[0].count, 1);
});

Deno.test("all unknown DNS record types keep their numeric label", () => {
  assert.equal(dnsQueryType(28), "AAAA");
  assert.equal(dnsQueryType(65), "HTTPS");
  assert.equal(dnsQueryType(65535), "TYPE65535");
});

import assert from "node:assert/strict";

import { groupDnsBursts, type DnsQuery } from "../../src/modules/settings/dns/data.ts";

const row = (
  id: number,
  qtype: number,
  time_ms: number,
  extra: Partial<DnsQuery> = {},
): DnsQuery => ({
  id,
  qtype,
  time_ms,
  client: "192.0.2.1",
  domain: "example.org",
  protocol: "udp",
  ...extra,
});
Deno.test("nearby A/AAAA/HTTPS display together without altering raw queries", () => {
  const input = [row(3, 28, 1100), row(2, 65, 1010), row(1, 1, 1000)];
  const before = structuredClone(input),
    groups = groupDnsBursts(input);
  assert.equal(groups.length, 1);
  assert.equal(groups[0].id, 1);
  assert.deepEqual(
    groups[0].queries.map((q) => q.qtype),
    [1, 65, 28],
  );
  assert.deepEqual(input, before);
  assert.deepEqual(
    groups[0].queries.map((q) => q.time_ms),
    [1000, 1010, 1100],
  );
});
Deno.test("different devices, transports and ordinary record types remain separate", () => {
  const groups = groupDnsBursts([
    row(1, 1, 1000),
    row(2, 65, 1010, { client: "192.0.2.2" }),
    row(3, 28, 1020, { protocol: "tcp" }),
    row(4, 15, 1030),
    row(5, 65, 1040, { domain: "other.org" }),
  ]);
  assert.equal(groups.length, 5);
});
Deno.test("fixed one-second window does not slide and duplicate types are not swallowed", () => {
  assert.equal(groupDnsBursts([row(1, 1, 1000), row(2, 65, 1900), row(3, 28, 2100)]).length, 2);
  assert.equal(groupDnsBursts([row(1, 1, 1000), row(2, 65, 2000)]).length, 1);
  assert.equal(groupDnsBursts([row(1, 1, 1000), row(2, 65, 2001)]).length, 2);
  assert.equal(groupDnsBursts([row(1, 1, 1000), row(2, 1, 1001)]).length, 2);
  assert.equal(groupDnsBursts([row(1, 1, 1000), row(2, 65, 999)]).length, 2);
});
Deno.test("first row is immediately visible and its anchor survives new sibling types", () => {
  const first = groupDnsBursts([row(1, 1, 1000)]);
  const updated = groupDnsBursts([row(2, 65, 1005), row(1, 1, 1000)]);
  assert.equal(first.length, 1);
  assert.equal(updated[0].id, first[0].id);
  assert.equal(updated[0].queries.length, 2);
});

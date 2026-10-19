import assert from "node:assert/strict";
import {
  dnsClientNames,
  isDnsClientName,
  normalizeDnsClientAddress,
} from "../../src/modules/settings/dns-client-labels";
import { filterDnsQueries } from "../../src/modules/settings/dns-query-data";
import {
  applyDnsEvent,
  createSnapshotReceiver,
  MAX_EVENT_SIZE,
} from "../../src/modules/settings/dns-query-stream";

const row = (id = 1, name?: string, client = "::ffff:192.168.1.42") => ({
  id, time_ms: 123456, client, client_name: name, domain: "example.org", qtype: 1,
  protocol: "udp" as const,
});
const event = (type: string, value: unknown) => ({ event: type, data: JSON.stringify(value) });

Deno.test("normalize only actual IPv4-mapped addresses, in all supported representations", () => {
  for (const ip of ["::ffff:192.168.1.42", "::FFFF:c0a8:12a", "0:0:0:0:0:ffff:c0a8:012a"])
    assert.equal(normalizeDnsClientAddress(ip), "192.168.1.42");
  for (const ip of ["192.168.1.42", "::192.168.1.42", "2001:db8::ffff:12", "::ffff:999.1.2.3", "::ffff:192.168.01.42", "::ffff:1:2:3", "x::ffff:c0a8:12a"])
    assert.equal(normalizeDnsClientAddress(ip), ip);
});

Deno.test("client aliases remain optional, UTF-8 bounded and are not filter identities", () => {
  assert(isDnsClientName("Телефон 📱"));
  assert(!isDnsClientName("\nInjected"));
  assert(!isDnsClientName("я".repeat(64)));
  assert(!isDnsClientName(42));
  const state = applyDnsEvent({ queries: [], capacity: 1000 }, event("snapshot", {
    queries: [row(2, "Same name"), row(1, "Same name", "192.168.1.43")], capacity: 1000,
  }));
  assert.equal(state.queries[0].client, "192.168.1.42");
  assert.equal(dnsClientNames(state.queries).size, 2);
  assert.equal(filterDnsQueries(state.queries, "", "192.168.1.42").length, 1);
  assert.throws(() => applyDnsEvent(state, event("query", { ...row(3), client_name: 42 })));
  const badLabel = applyDnsEvent(state, event("query", row(3, "bad\nname")));
  assert.equal(badLabel.queries[0].client_name, undefined);
  assert.equal(badLabel.queries[0].client, "192.168.1.42");
});

Deno.test("alias refresh replaces existing rows atomically and removes missing names", () => {
  const receive = createSnapshotReceiver();
  let state = applyDnsEvent({ queries: [], capacity: 1000 }, event("snapshot", {
    queries: [row(1, "Old name")], capacity: 1000,
  }));
  assert.equal(receive(event("snapshot_begin", { count: 1, capacity: 1000, through: 1 })), null);
  assert.equal(receive(event("snapshot_chunk", { offset: 0, queries: [row(1, "New name")] })), null);
  assert.equal(dnsClientNames(state.queries).get("192.168.1.42"), "Old name");
  const complete = receive(event("snapshot_end", { through: 1 }));
  assert(complete);
  state = applyDnsEvent(state, complete);
  assert.equal(state.queries.length, 1);
  assert.equal(dnsClientNames(state.queries).get("192.168.1.42"), "New name");
  state = applyDnsEvent(state, event("snapshot", { queries: [row()], capacity: 1000 }));
  assert.equal(dnsClientNames(state.queries).size, 0);
  assert.equal(state.queries[0].client, "192.168.1.42");
});

Deno.test("newest rows control labels; old aliases never reappear after a name is removed", () => {
  assert.equal(dnsClientNames([row(2), row(1, "Stale name")]).size, 0);
  assert.equal(dnsClientNames([row(1, "Stale name"), row(2)]).size, 0);
  assert.equal(dnsClientNames([row(2, "New name"), row(1, "Stale name")]).get("192.168.1.42"), "New name");
});

Deno.test("bounded snapshot batches still fit with longest escaped domain and name", () => {
  const queries = Array.from({ length: 32 }, (_, index) => ({
    ...row(index + 1, "\\".repeat(127), "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"),
    domain: "\\255".repeat(250), qtype: 65535,
  }));
  const payload = JSON.stringify({ offset: 0, queries });
  assert(new TextEncoder().encode(`event: snapshot_chunk\ndata: ${payload}\n\n`).length < MAX_EVENT_SIZE);
  const receive = createSnapshotReceiver();
  receive(event("snapshot_begin", { count: 32, capacity: 1000, through: 32 }));
  receive(event("snapshot_chunk", { offset: 0, queries }));
  assert(receive(event("snapshot_end", { through: 32 })));
});

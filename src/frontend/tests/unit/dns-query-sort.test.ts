import assert from "node:assert/strict";

import {
  groupDnsBursts,
  groupDnsQueries,
  nextDnsTimeSort,
  sortDnsByCount,
  sortDnsByTime,
  type DnsQuery,
  type DnsTimeSort,
} from "../../src/modules/settings/dns/data";

const query = (id: number, time_ms: number, domain = `host${id}.example`): DnsQuery => ({
  id,
  time_ms,
  domain,
  client: "192.168.1.10",
  qtype: 1,
  protocol: "udp",
});
const time = (row: DnsQuery) => row.time_ms;

Deno.test("time sort alternates newest and oldest without an unsorted state", () => {
  let direction: DnsTimeSort = "desc";
  const states: DnsTimeSort[] = [direction];
  for (let i = 0; i < 4; i++) {
    direction = nextDnsTimeSort(direction);
    states.push(direction);
  }
  assert.deepEqual(states, ["desc", "asc", "desc", "asc", "desc"]);
});

Deno.test(
  "time sorting uses numeric timestamps, keeps ties stable and never mutates history",
  () => {
    // IDs deliberately disagree with time (for example after router clock correction).
    const rows = Object.freeze([query(4, 900), query(3, 10000), query(2, 10000), query(1, 500)]);
    const before = [...rows];
    assert.deepEqual(
      sortDnsByTime(rows, "desc", time).map((row) => row.id),
      [3, 2, 4, 1],
    );
    assert.deepEqual(
      sortDnsByTime(rows, "asc", time).map((row) => row.id),
      [1, 4, 3, 2],
    );
    assert.deepEqual(rows, before);
    assert.deepEqual(sortDnsByTime([], "desc", time), []);
  },
);

Deno.test("time sort covers the full retained view before pagination and incoming events", () => {
  const rows = Array.from({ length: 65 }, (_, index) => query(65 - index, (65 - index) * 1000));
  const ascending = sortDnsByTime(rows, "asc", time);
  assert.equal(ascending[0].id, 1);
  assert.equal(ascending.slice(30, 60)[0].id, 31);
  assert.equal(ascending.slice(60)[0].id, 61);
  const updated = [query(66, 500), ...rows];
  assert.equal(sortDnsByTime(updated, "asc", time)[0].id, 66);
  assert.equal(sortDnsByTime(updated, "desc", time)[0].id, 65);
  assert.equal(rows[0].id, 65);
});

Deno.test("domain groups sort by last request, not by the number of queries", () => {
  const groups = groupDnsQueries([
    query(4, 9000, "new.example"),
    query(3, 3000, "busy.example"),
    query(2, 2000, "busy.example"),
    query(1, 1000, "busy.example"),
  ]);
  const lastTime = (row: (typeof groups)[number]) => row.lastTime;
  assert.equal(groups[0].domain, "busy.example");
  assert.equal(sortDnsByTime(groups, "desc", lastTime)[0].domain, "new.example");
  assert.equal(sortDnsByTime(groups, "asc", lastTime)[0].domain, "busy.example");
  assert.equal(groups[0].count, 3);
});

Deno.test("sorting display bursts keeps their anchor and complete raw membership", () => {
  const rows: DnsQuery[] = [
    query(4, 6000, "later.example"),
    { ...query(3, 1500, "related.example"), qtype: 65 },
    { ...query(2, 1200, "related.example"), qtype: 28 },
    query(1, 1000, "related.example"),
  ];
  const bursts = groupDnsBursts(rows);
  const before = structuredClone(bursts);
  assert.equal(bursts.length, 2);
  for (const direction of ["desc", "asc"] as const) {
    const sorted = sortDnsByTime(bursts, direction, time);
    const combined = sorted.find((row) => row.domain === "related.example")!;
    assert.equal(combined.id, 1);
    assert.equal(combined.time_ms, 1000);
    assert.deepEqual(
      combined.queries.map((row) => row.id),
      [1, 2, 3],
    );
  }
  assert.deepEqual(bursts, before);
  assert.deepEqual(
    rows.map((row) => row.id),
    [4, 3, 2, 1],
  );
});

Deno.test("count sorting orders full groups, breaks ties by time and preserves history", () => {
  const groups = groupDnsQueries([
    query(6, 6000, "recent.example"),
    query(5, 5000, "busy.example"),
    query(4, 4000, "busy.example"),
    query(3, 3000, "busy.example"),
    query(2, 2000, "old.example"),
    query(1, 1000, "old.example"),
  ]);
  const before = structuredClone(groups);
  assert.deepEqual(
    sortDnsByCount(groups, "desc").map((row) => row.domain),
    ["busy.example", "old.example", "recent.example"],
  );
  assert.deepEqual(
    sortDnsByCount(groups, "asc").map((row) => row.domain),
    ["recent.example", "old.example", "busy.example"],
  );
  const ties = groups.map((row) => ({ ...row, count: 1 }));
  for (const direction of ["asc", "desc"] as const)
    assert.deepEqual(
      sortDnsByCount(ties, direction).map((row) => row.domain),
      ["recent.example", "busy.example", "old.example"],
    );
  assert.deepEqual(groups, before);
  assert.deepEqual(sortDnsByCount([], "desc"), []);
});

import { strict as assert } from "node:assert";

import {
  boundedRuleListLines,
  buildImportedRules,
  parseRuleList,
  ruleListPreview,
} from "../../src/modules/groups/rule-list-parser";

Deno.test(
  "rule list parsing retains separators, detection order, comments and invalid preview lines",
  () => {
    const parsed = parseRuleList(
      " # comment\nexample.test,192.168.1.1\n2001:db8::/32\n/regex/\n[\n",
      "auto",
    );
    assert.deepEqual(parsed.entries, [
      { rule: "example.test", type: "namespace" },
      { rule: "192.168.1.1", type: "subnet" },
      { rule: "2001:db8::/32", type: "subnet6" },
      { rule: "/regex/", type: "regex" },
    ]);
    assert.deepEqual(parsed.stats, { namespace: 1, subnet: 1, subnet6: 1, regex: 1, INVALID: 1 });
    assert.equal(parsed.lines[0].type, null);
    assert.equal(parsed.lines[5].type, "INVALID");
    assert.equal(parsed.lines[6].type, null);
    assert.equal(parsed.totalLines, 7);
  },
);

Deno.test(
  "explicit types validate separately and deduplicate by trimmed type and pattern in source order",
  () => {
    const text = " example.test\nexample.test\n*.example.test\n[";
    const namespaces = parseRuleList(text, "namespace");
    assert.deepEqual(namespaces.entries, [{ rule: "example.test", type: "namespace" }]);
    assert.deepEqual(namespaces.stats, { namespace: 2, INVALID: 2 });
    const wildcards = parseRuleList(text, "wildcard");
    assert.deepEqual(wildcards.entries, [
      { rule: "example.test", type: "wildcard" },
      { rule: "*.example.test", type: "wildcard" },
    ]);
    assert.deepEqual(wildcards.stats, { wildcard: 3, INVALID: 1 });
  },
);

Deno.test(
  "100k rule previews stay bounded while complete imported rules remain unique and ordered",
  () => {
    const text = Array.from({ length: 100000 }, (_, index) => `item${index}.example.test`).join(
      "\n",
    );
    const parsed = parseRuleList(text, "auto");
    const preview = ruleListPreview(parsed);
    assert.equal(preview.lines.length, 500);
    assert.equal(preview.totalLines, 100000);
    assert.deepEqual(preview.stats, { namespace: 100000 });
    assert.equal(Object.hasOwn(preview, "entries"), false);
    let id = 0x10000000;
    const rules = buildImportedRules(parsed, () => (id++).toString(16));
    assert.equal(rules.length, 100000);
    assert.equal(new Set(rules.map((rule) => rule.id)).size, 100000);
    assert.equal(rules[0].rule, "item0.example.test");
    assert.equal(rules.at(-1)!.rule, "item99999.example.test");
    assert.equal(
      rules.every((rule) => rule.enable && rule.name === "" && rule.type === "namespace"),
      true,
    );
    assert.equal(Object.hasOwn(parsed.entries[0], "id"), false);
  },
);

Deno.test("imported rule IDs retry collisions before exposing a duplicate identity", () => {
  const ids = ["11223344", "11223344", "22334455"];
  const rules = buildImportedRules(parseRuleList("first.test\nsecond.test", "auto"), () =>
    ids.shift()!,
  );
  assert.deepEqual(
    rules.map((rule) => rule.id),
    ["11223344", "22334455"],
  );
});

Deno.test("editor line counting stops at its visible height limit and ignores empty tokens", () => {
  assert.equal(boundedRuleListLines("", 20), 1);
  assert.equal(boundedRuleListLines(" \n, \n first.test,second.test\n", 20), 2);
  assert.equal(
    boundedRuleListLines(Array.from({ length: 100000 }, () => "example.test").join("\n"), 20),
    20,
  );
});

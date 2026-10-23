import { strict as assert } from "node:assert";

import { VALIDATOP_MAP } from "../../src/utils/rule-validators";
import { subscriptionRulePattern } from "../../src/utils/subscription-pattern";

Deno.test("keywords map to the existing wildcard type", () => {
  assert.equal(subscriptionRulePattern("DOMAIN-KEYWORD,openai", "wildcard"), "*openai*");
  assert.equal(subscriptionRulePattern("DOMAIN-KEYWORD,labs.google", "wildcard"), "*labs.google*");
  assert.equal(subscriptionRulePattern("DOMAIN-KEYWORD,a*b?c", "wildcard"), String.raw`*a\*b\?c*`);
  assert.equal(subscriptionRulePattern("DOMAIN-KEYWORD,openai", "namespace"), "openai");
  assert.equal(subscriptionRulePattern("DOMAIN-WILDCARD,*.example.com", "wildcard"), "*.example.com");
  assert.equal(VALIDATOP_MAP.keyword, undefined);
  assert.equal(VALIDATOP_MAP["domain-wildcard"], undefined);
});

Deno.test("typed source remains stable and independent of rule presentation", () => {
  const examples = [
    ["DOMAIN,example.com", "example.com", "domain"],
    ["DOMAIN-SUFFIX,example.com", "example.com", "namespace"],
    ["DOMAIN-KEYWORD,labs.google", "*labs.google*", "wildcard"],
    ["DOMAIN-WILDCARD,*.example.com", "*.example.com", "wildcard"],
    ["DOMAIN-REGEX,^a{1,3}\\.example$", "^a{1,3}\\.example$", "regex"],
    ["IP-CIDR,192.0.2.0/24", "192.0.2.0/24", "subnet"],
    ["IP-CIDR6,2001:db8::/32", "2001:db8::/32", "subnet6"],
  ] as const;
  for (const [source, expected, type] of examples) {
    assert.equal(subscriptionRulePattern(source, type), expected);
    assert.equal(subscriptionRulePattern(source, "domain"), source.slice(source.indexOf(",") + 1));
  }
  for (const source of ["example.com", "*.example.com", "^a,b$", "UNKNOWN,value", ""]) {
    assert.equal(subscriptionRulePattern(source), source);
  }
});

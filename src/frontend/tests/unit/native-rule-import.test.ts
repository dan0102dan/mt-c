import { strict as assert } from "node:assert";

import {
  isValidDomainWildcard,
  isValidKeyword,
  VALIDATOP_MAP,
} from "../../src/utils/rule-validators";
import { subscriptionRulePattern } from "../../src/utils/subscription-pattern";

Deno.test("native keyword accepts literal punctuation but not empty or oversized input", () => {
  for (const pattern of ["openai", "labs.google", "a*b?c", "[literal]", "OPENAI"]) {
    assert(isValidKeyword(pattern));
  }
  for (const pattern of ["", "has space", "line\nbreak", "\u0000", "кириллица", "x".repeat(4097)]) {
    assert(!isValidKeyword(pattern));
  }
});

Deno.test("domain wildcard validates the native star/question dialect", () => {
  for (const pattern of ["*.example.com", "api?.example.com", "**", "_service.*"]) {
    assert(isValidDomainWildcard(pattern));
  }
  for (const pattern of ["", "[ab].example.com", "a\\.b", "a b", "(a|b)"]) {
    assert(!isValidDomainWildcard(pattern));
  }
  assert.equal(VALIDATOP_MAP.keyword, isValidKeyword);
  assert.equal(VALIDATOP_MAP["domain-wildcard"], isValidDomainWildcard);
});

Deno.test("subscription display strips only a canonical source prefix", () => {
  const examples = [
    ["DOMAIN,example.com", "example.com", "domain"],
    ["DOMAIN-SUFFIX,example.com", "example.com", "namespace"],
    ["DOMAIN-KEYWORD,labs.google", "labs.google", "keyword"],
    ["DOMAIN-WILDCARD,*.example.com", "*.example.com", "domain-wildcard"],
    ["DOMAIN-REGEX,^a{1,3}\\.example$", "^a{1,3}\\.example$", "regex"],
    ["IP-CIDR,192.0.2.0/24", "192.0.2.0/24", "subnet"],
    ["IP-CIDR6,2001:db8::/32", "2001:db8::/32", "subnet6"],
  ];
  for (const [source, expected, type] of examples) {
    const rule = { rule: source, type, enable: true };
    const before = { ...rule };
    assert.equal(subscriptionRulePattern(rule.rule), expected);
    assert(VALIDATOP_MAP[type](subscriptionRulePattern(rule.rule)));
    assert.deepEqual(rule, before);
    // The source identity is independent of an edited effective type.
    rule.type = "keyword";
    assert.equal(subscriptionRulePattern(rule.rule), expected);
    assert.equal(rule.rule, source);
  }
  for (const source of ["example.com", "*.example.com", "^a,b$", "UNKNOWN,value", ""]) {
    assert.equal(subscriptionRulePattern(source), source);
  }
});

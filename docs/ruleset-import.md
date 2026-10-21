# Native keywords and text rule-set subscriptions

## Rule types

`keyword` searches for a **literal ASCII case-insensitive substring**. For example,
`openai` matches `api.OPENAI.com`; `labs.google` does not match `labsXgoogle`.
Dots, stars and question marks in a keyword are literal. Empty keywords do not
match. There is no PCRE2 compilation or matching for this type.

`domain-wildcard` is a separate native, ASCII case-insensitive glob: `*` matches
zero or more bytes (including dots), `?` exactly one byte, and `.` is literal.
`*.example.com` matches `a.b.example.com`, but not `example.com`. The supported
import dialect is only `*`/`?`, not bracket classes or regular expressions.

The existing `wildcard` retains its go-wildcard compatibility behavior, including
its non-literal dot, case sensitivity and trailing-question-mark quirk. Existing
`domain`, `namespace`, `regex`, `subnet` and `subnet6` rules are unchanged.
Neither new type is silently converted into a regex. Only `regex` uses PCRE2.

## Supported subscription sources

Use a **full text rule-set**, for example `meta/ai.list` in the `ruleset` branch of
QuixoticHeart/rule-set, not its `meta/domain/ai.list` (which omits keyword rules).
This is a shared text-rule subset, not support for entire proxy configurations.

| Source record | Effective mt-c type |
| --- | --- |
| `DOMAIN,example.com` / `HOST,example.com,policy` | `domain` |
| `DOMAIN-SUFFIX,example.com` / `HOST-SUFFIX,...` | `namespace` |
| `DOMAIN-KEYWORD,openai` / `HOST-KEYWORD,...` | `keyword` |
| `DOMAIN-WILDCARD,*.example.com` | `domain-wildcard` |
| `DOMAIN-REGEX,^a[0-9]{1,3}\.example\.com$` | `regex` |
| `IP-CIDR,192.0.2.0/24,no-resolve` | `subnet` |
| `IP-CIDR6,2001:db8::/32,no-resolve` / `IP6-CIDR,...` | `subnet6` |

The IP family is validated and selected from the address, including IPv6 written
using `IP-CIDR`. Optional policy labels are ignored: the subscription's mt-c
interface/profile and priority select the route. `no-resolve` is accepted only as
the final IP modifier; these records already become static destination IP rules.
Source-IP (`src`) rules are rejected, not imported as destination rules.

Record names are case-insensitive. Non-regex values are folded to ASCII lowercase;
exact-domain and namespace rules retain the existing mt-c matching semantics
(case-sensitive matching of the normalized value). Native keyword/glob rules are
case-insensitive. IDNs must be supplied as ASCII A-labels.

Typed rule-sets use one record per physical line, accept UTF-8 BOM and CRLF, and
skip empty lines and whole-line `#`, `;`, `//` comments. Regex records retain the
**entire remainder after the first comma**, including quantifiers and character
classes. Use two-field `DOMAIN-REGEX,expression`; inline policy suffixes on regex
records and inline comments are not interpreted.

`PROCESS-*`, `GEOIP`, `GEOSITE`, `IP-ASN`, logical rules and unsupported modifiers
are not approximated. A recognized typed source is parsed strictly: an unsupported
or malformed record rejects the entire refresh, logs its line/reason, and leaves
the previous subscription rules intact. Mixing bare domains into a typed rule-set
is rejected. YAML/JSON, binary MRS/SRS, nested rule providers and complete client
configs are outside this importer. A successful empty source retains the existing
empty-subscription behavior.

Plain legacy lists still use their existing comma/newline tokenizer and inference
order; no automatic reinterpretation of existing wildcard/regex patterns occurs.
The existing 8 MiB download, 100,000-rule and 4096-byte rule limits remain in effect.

## Persistence and identity decision

The downloaded rule's `rule` field retains a canonical `TYPE,value` source record.
It is the stable identity, not just the extracted payload. Consequently
`DOMAIN,example.com` and `DOMAIN-KEYWORD,example.com` do not collapse together.
Aliases, whitespace, non-regex case and policy-label changes normalize to the same
identity, preserving IDs and user overrides across refreshes.

Only runtime projection and WebUI display/validation extract the payload. HTTP
updates and exported overrides retain the canonical identity, even when the user
changes the effective type. Cache reload can recover the original type before
applying authoritative user overrides; removing an override cannot resurrectitate
from an effective cache. No new YAML/JSON fields or config-version changes are
required. New `keyword`/`domain-wildcard` rules require a supporting mt-c version;
older versions cannot be assumed to understand them. Existing configurations
remain readable without migration.

This is an additive extension to the legacy contracts in
`c-rewrite/compatibility-contract.md` sections 6 and 8 and the regex decision D-07.
The frozen legacy differential corpus is not changed. Coverage is in
`test_native_rule_import.c` and `native-rule-import.test.ts`. Matching remains
linear over keyword/glob rules; no measured throughput or device-speedup claim
is made. A shared multi-pattern index should be justified by router benchmarks,
not introduced speculatively.

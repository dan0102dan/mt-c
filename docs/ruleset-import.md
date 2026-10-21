# Text rule-set import using existing mt-c rule types

## Design

Importing rules from Meta / Surge / Loon / Shadowrocket / Stash text lists
does **not** create new configurable rule types. mt-c retains the existing
`domain`, `namespace`, `wildcard`, `regex`, `subnet`, and `subnet6`.

| External rule | Existing mt-c type | Meaning |
| --- | --- | --- |
| `DOMAIN,example.com` | `domain` | Exact domain |
| `DOMAIN-SUFFIX,example.com` | `namespace` | Domain and subdomains |
| `DOMAIN-KEYWORD,openai` | `wildcard` | Like `*openai*` |
| `DOMAIN-WILDCARD,*.example.com` | `wildcard` | Source wildcard |
| `DOMAIN-REGEX,expression` | `regex` | PCRE2 |
| `IP-CIDR,192.0.2.0/24` | `subnet` | Existing IP routing |
| `IP-CIDR6,2001:db8::/32` | `subnet6` | Existing IPv6 routing |

`HOST`, `HOST-SUFFIX`, `HOST-KEYWORD` and `IP6-CIDR` are accepted aliases.

Keyword and source wildcard records map to the **existing `wildcard`** rule
type, rather than adding keyword/domain-wildcard types. `DOMAIN-KEYWORD,openai`
is converted to a wildcard equivalent to `*openai*` at runtime. Since mt-c's
legacy Go-compatible wildcard implementation interprets `.` as any byte and
compares case-sensitively, imported source records use a small, **runtime-only**
`@mtc:` pattern marker selecting literal-dot, ASCII case-insensitive glob
matching. The importer escapes any keyword `*`, `?`, or backslash so they
stay literal. This marker is never persisted or exposed as a configurable
rule type. Original user-entered wildcard rules still use the unchanged
Go-compatible implementation.

Neither imported keyword nor source wildcard invokes PCRE2. Only explicit
`DOMAIN-REGEX` rules use the regex engine.

## Source identity and user overrides

The subscription cache retains a canonical `TYPE,value` record as each
downloaded rule's stable **source identity**. This distinguishes identical
values from different external types and preserves IDs and user overrides
across refreshes and restart. At runtime, a recognized source prefix is
removed or converted according to the current *existing* rule type.
If an imported keyword's type is manually overridden to `namespace`, its
runtime value is `openai`, not `*openai*`. The WebUI displays the imported
keyword as `*openai*` when its effective type is `wildcard`.

This changes neither the persisted schema nor the public six-type enum;
older versions still cannot be assumed to understand typed canonical source
lines in an exported subscription cache.

## Supported text source and validation

Use the complete text rule-set, such as `meta/ai.list` in the
QuixoticHeart/rule-set `ruleset` branch. The split `meta/domain/ai.list`
omits keyword rules.

Typed sources are parsed one physical line at a time. BOM and CRLF work;
empty lines and whole-line `#`, `;`, `//` comments are skipped.
`DOMAIN-REGEX` expressions retain all text after the first comma,
including quantifier commas. Optional upstream policy labels are ignored:
the interface/profile/priority from the mt-c subscription take precedence.
`no-resolve` is accepted only as a final IP modifier; `src` is rejected.

Unknown/unsupported types, malformed records, unsupported flags and mixing
plain domains with typed records cause an atomic refresh failure. Previously
loaded subscription rules are retained. This imports a common **text rule-set
subset**, not full client configs, YAML/JSON, MRS/SRS or nested rule providers.
`GEOIP`, `GEOSITE`, process rules, ASN rules and logical rules are not
approximated.

Legacy plain-domain subscriptions retain their original comma/newline
tokenization and type inference, and old wildcard rules retain their matching
semantics. Original size limits remain (8 MiB fetch, 100k rules, 4096 bytes
per record).

## Tests

C regression tests cover the typed parser, literal-dot keywords, case folding,
literal keyword metacharacters, imported glob semantics, legacy behavior,
cache restore, override persistence and runtime projection. A dedicated
executable intercepts PCRE2 and asserts zero compile/match calls for typed
keyword and source wildcard rules. Frontend tests assert the absence of new
configurable rule types. Router throughput improvement is not claimed without
benchmark measurements.

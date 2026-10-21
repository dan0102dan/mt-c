# Mihomo text rule-name coverage

Audited on 2026-10-21 against the official
[routing rules documentation](https://wiki.metacubex.one/en/config/rules/) and
[`rules/parser.go`](https://github.com/MetaCubeX/mihomo/blob/Meta/rules/parser.go),
Git blob `474d01ef42a6870fcc349e4bf8f8b5836406e1d4`.

The parser has 37 accepted rule **names**, including the `IP-CIDR6` alias.
mt-c recognizes all 37 when detecting a typed text subscription. Recognition
is not equivalent to executing all Mihomo routing semantics: seven names map
to existing mt-c rules, and the other 30 reject the entire refresh with
`MT_ERR_INVAL`. The previous subscription remains intact. No unsupported rule
is silently skipped or downgraded into a bare domain or ordinary CIDR.

## Imported using existing types

| Mihomo name | mt-c type |
| --- | --- |
| `DOMAIN` | `domain` |
| `DOMAIN-SUFFIX` | `namespace` |
| `DOMAIN-KEYWORD` | `wildcard` |
| `DOMAIN-WILDCARD` | `wildcard` |
| `DOMAIN-REGEX` | `regex` |
| `IP-CIDR` | `subnet` or `subnet6`, selected from the address family |
| `IP-CIDR6` | `subnet` or `subnet6`, selected from the address family |

These are text import mappings, not support for full Mihomo configurations,
policy/action execution, or every upstream regular-expression dialect.
Existing conversion, validation, wildcard matching and PCRE2 behavior are
unchanged by this coverage fix. See [ruleset-import.md](ruleset-import.md).

## Recognized, rejected rather than misinterpreted

| Category | Exact names |
| --- | --- |
| External data / geographical and ASN matching | `GEOSITE`, `GEOIP`, `SRC-GEOIP`, `IP-ASN`, `SRC-IP-ASN` |
| IP suffixes and source addresses | `IP-SUFFIX`, `SRC-IP-CIDR`, `SRC-IP-SUFFIX` |
| Ports | `SRC-PORT`, `DST-PORT`, `IN-PORT` |
| Inbound / rematch metadata | `IN-TYPE`, `IN-USER`, `IN-NAME`, `REMATCH-NAME` |
| Processes | `PROCESS-NAME`, `PROCESS-PATH`, `PROCESS-NAME-REGEX`, `PROCESS-PATH-REGEX`, `PROCESS-NAME-WILDCARD`, `PROCESS-PATH-WILDCARD` |
| Other connection metadata | `NETWORK`, `UID`, `DSCP` |
| Providers / logic / catch-all | `RULE-SET`, `SUB-RULE`, `AND`, `OR`, `NOT`, `MATCH` |

For example, `IP-SUFFIX,8.8.8.8/24` must never turn into the destination
subnet `8.8.8.0/24`. It is a known typed record with semantics that mt-c
cannot express using its current six types, so import fails atomically.
Implementing these remaining semantics would require separate routing work;
this patch does not pretend to implement them by recognizing their names.

## Compatibility and future additions

Names are compared case-insensitively and by exact token, not by a broad
prefix or uppercase/hyphen heuristic. `MY-SITE,example.com`,
`IN-HOUSE,example.com` and `IP-SUFFIX-HOST,example.com` remain legacy lists.

Existing aliases from other text dialects (`HOST`, `HOST-SUFFIX`,
`HOST-KEYWORD`, `IP6-CIDR`) still import. Previously reserved `FINAL` and
`INCLUDE` still reject; they are not counted as Mihomo rule names.

An unknown name inside an already identified typed source is rejected.
Auto-detection cannot unambiguously distinguish a future, unlisted rule name
from a valid legacy single-label hostname. Therefore no claim is made about
all future upstream names. When Mihomo adds a new parser case, update both
the production recognition table and the independent test inventory.

## Regression coverage

`src/backend-c/tests/unit/test_mihomo_rule_types.c` contains an independent
inventory of all 37 names. It covers uppercase/lowercase/mixed-case names,
whitespace, BOM/CRLF/comments, unsupported-only sources (no supported record
to trigger strict mode), records at the start/middle/end of a refresh,
retention of previous IDs/types/enable flags, legacy prefix lookalikes, and
other-dialect aliases. The existing unit-test Makefile discovers it normally;
no CI configuration or DNS-path work is added.

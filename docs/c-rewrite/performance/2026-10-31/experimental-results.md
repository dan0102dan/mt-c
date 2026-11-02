# Initial candidate experiments retained for comparison

The initial production candidate was `4fefbaf`; its timer reuse and unchanged
response forwarding applied to both UDP and TCP. These experiments exposed a
native TCP capacity p99 regression, including adverse directions after replacing
only the timer or proxy object. The final implementation therefore narrows those
two optimizations to UDP and is validated/measured in separate `final-*` folders.
No original raw result or executed harness has been overwritten.

Initial reports and summaries:

- `experimental-host-results.md` and `experimental-host-summary.json`.
- `experimental-emulator-results.md`, `experimental-emulator-correctness-summary.json`
  and `experimental-emulator-performance-summary.json`.

The earlier native runner probed readiness over UDP even before TCP scenarios;
the final runner uses the scenario protocol for both upstream and proxy probes.
Both variants within each experiment always use the same readiness method.
The exact earlier runner snapshots remain in their original output directories.

Final baseline LAN/DHCP smoke may reuse the completed `apk-validation/` and
`ipk-validation-final/` evidence only after checking baseline executable and
runtime/fixture identity. Final candidate smoke and both variants' detailed
DNS/kernel checks run again; any reuse is explicit in the final metadata.

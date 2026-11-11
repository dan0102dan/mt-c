import assert from "node:assert/strict";

import type { GeneralSettings } from "../../src/modules/settings/general/config.ts";
import { requiresDnsRestart } from "../../src/modules/settings/general/save-impact.ts";

function fixture(): GeneralSettings {
  return {
    httpWeb: { enabled: true, auth: { enabled: false }, host: { address: "0.0.0.0", port: 8080 } },
    dnsProxy: {
      host: { address: "::", port: 3553 },
      upstream: { address: "127.0.0.1", port: 53 },
      disableRemap53: false,
      disableFakePTR: false,
      disableDropAAAA: false,
      maxIdleConns: 10,
      maxConcurrent: 100,
      timeoutMs: 5000,
    },
    netfilter: { disableIPv4: false, disableIPv6: false, ipset: { additionalTTLSeconds: 3600 } },
    link: ["br0"],
    showAllInterfaces: false,
    logLevel: "info",
  };
}

// Keep all 20 editable fields aligned with the native preview classification
// test. The browser warns locally; the native PUT still enforces permission.
const cases: { name: string; change: (settings: GeneralSettings) => void; restart: boolean }[] = [
  { name: "httpWeb.enabled", change: (s) => (s.httpWeb.enabled = false), restart: false },
  { name: "httpWeb.auth.enabled", change: (s) => (s.httpWeb.auth.enabled = true), restart: false },
  { name: "httpWeb.host.address", change: (s) => (s.httpWeb.host.address = "127.0.0.1"), restart: true },
  { name: "httpWeb.host.port", change: (s) => (s.httpWeb.host.port = 8081), restart: true },
  { name: "dnsProxy.host.address", change: (s) => (s.dnsProxy.host.address = "0.0.0.0"), restart: true },
  { name: "dnsProxy.host.port", change: (s) => (s.dnsProxy.host.port = 3554), restart: true },
  { name: "dnsProxy.upstream.address", change: (s) => (s.dnsProxy.upstream.address = "8.8.8.8"), restart: true },
  { name: "dnsProxy.upstream.port", change: (s) => (s.dnsProxy.upstream.port = 5353), restart: true },
  { name: "dnsProxy.disableRemap53", change: (s) => (s.dnsProxy.disableRemap53 = true), restart: true },
  { name: "dnsProxy.disableFakePTR", change: (s) => (s.dnsProxy.disableFakePTR = true), restart: false },
  { name: "dnsProxy.disableDropAAAA", change: (s) => (s.dnsProxy.disableDropAAAA = true), restart: false },
  { name: "dnsProxy.maxIdleConns", change: (s) => (s.dnsProxy.maxIdleConns = 20), restart: true },
  { name: "dnsProxy.maxConcurrent", change: (s) => (s.dnsProxy.maxConcurrent = 50), restart: true },
  { name: "dnsProxy.timeoutMs", change: (s) => (s.dnsProxy.timeoutMs = 6000), restart: true },
  { name: "netfilter.disableIPv4", change: (s) => (s.netfilter.disableIPv4 = true), restart: true },
  { name: "netfilter.disableIPv6", change: (s) => (s.netfilter.disableIPv6 = true), restart: true },
  { name: "netfilter.ipset.additionalTTLSeconds", change: (s) => (s.netfilter.ipset.additionalTTLSeconds = 42), restart: false },
  { name: "link", change: (s) => (s.link = ["br-lan"]), restart: true },
  { name: "showAllInterfaces", change: (s) => (s.showAllInterfaces = true), restart: false },
  { name: "logLevel", change: (s) => (s.logLevel = "warn"), restart: false },
];

for (const { name, change, restart } of cases) {
  Deno.test(`immediate settings warning classifies ${name}`, () => {
    const current = fixture();
    const next = structuredClone(current);
    change(next);
    assert.equal(requiresDnsRestart(current, next), restart);
    assert.deepEqual(current, fixture());
  });
}

Deno.test("unchanged and reverted settings do not require a DNS warning", () => {
  const current = fixture();
  const next = structuredClone(current);
  assert.equal(requiresDnsRestart(current, next), false);
  next.dnsProxy.timeoutMs = 7000;
  assert.equal(requiresDnsRestart(current, next), true);
  next.dnsProxy.timeoutMs = current.dnsProxy.timeoutMs;
  next.logLevel = "debug";
  next.httpWeb.enabled = false;
  assert.equal(requiresDnsRestart(current, next), false);
});

Deno.test("mixed WebUI and DNS changes still require a DNS warning", () => {
  const current = fixture();
  const next = structuredClone(current);
  next.httpWeb.enabled = false;
  next.dnsProxy.timeoutMs = 7000;
  assert.equal(requiresDnsRestart(current, next), true);
});

Deno.test("enabling WebUI alone does not require a DNS warning", () => {
  const current = fixture();
  current.httpWeb.enabled = false;
  const next = structuredClone(current);
  next.httpWeb.enabled = true;
  assert.equal(requiresDnsRestart(current, next), false);
});

Deno.test("warning comparison ignores object-key order but preserves interface order", () => {
  const current = fixture();
  current.link = ["br0", "br-lan"];
  const next = structuredClone(current);
  next.httpWeb.host = { port: current.httpWeb.host.port, address: current.httpWeb.host.address };
  assert.equal(requiresDnsRestart(current, next), false);
  next.link.reverse();
  assert.equal(requiresDnsRestart(current, next), true);
});

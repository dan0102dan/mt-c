import type { MockGeneralSettings } from "../../dev/settings-mock.ts";
import type { ServerConfig } from "../../src/modules/config-transfer/adapter.ts";

export function generalSettings(): MockGeneralSettings {
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

export function generalServerConfig(): ServerConfig {
  const settings = generalSettings();
  const { timeoutMs, ...dns } = settings.dnsProxy;
  return {
    configVersion: "0.8.5",
    app: {
      httpWeb: { ...settings.httpWeb, skin: "retained-skin" },
      dnsProxy: { ...dns, timeout: `${timeoutMs}ms` },
      netfilter: {
        disableIPv4: false,
        disableIPv6: false,
        iptables: { chainPrefix: "KEEP_" },
        ipset: { tablePrefix: "keep_", additionalTTL: "1h0m0s" },
        startMarkTableIndex: 1298220905,
      },
      keeneticPolicy: { enabled: true, policyId: "Policy1", mode: "include" },
      link: settings.link,
      showAllInterfaces: false,
      logLevel: "info",
    },
    groups: [
      {
        id: "aabbccdd",
        name: "Routing group",
        interface: "tun0",
        enable: true,
        priority: 300,
        color: "#ffffff",
        rules: [
          { id: "deadbeef", name: "Example", type: "domain", rule: "example.com", enable: true },
        ],
      },
    ],
    profiles: [
      { id: "11223344", name: "Primary VPN", interfaces: ["tun0"], on_unavailable: "blackhole" },
    ],
    subscriptions: [
      {
        id: "bbccddee",
        name: "Domain subscription",
        interface: "tun0",
        enable: true,
        priority: 100,
        url: "https://example.com/domains.txt",
        interval: 86400,
        last_update: 123,
        rules: [{ id: "ddccbbaa", type: "domain", rule: "cached.example", enable: true }],
      },
    ],
  };
}

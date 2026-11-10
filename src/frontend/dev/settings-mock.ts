import { isIP } from "node:net";
import type { Hono } from "hono";

import type { ServerConfig } from "../src/modules/config-transfer/adapter.ts";
import { fingerprint } from "../src/modules/config-transfer/model";

const BASE = "/api/v1/system/settings";
const UINT32_MAX = 4294967295;
type Host = { address: string; port: number };
export type MockGeneralSettings = {
  httpWeb: { enabled: boolean; auth: { enabled: boolean }; host: Host };
  dnsProxy: {
    host: Host;
    upstream: Host;
    disableRemap53: boolean;
    disableFakePTR: boolean;
    disableDropAAAA: boolean;
    maxIdleConns: number;
    maxConcurrent: number;
    timeoutMs: number;
  };
  netfilter: {
    disableIPv4: boolean;
    disableIPv6: boolean;
    ipset: { additionalTTLSeconds: number };
  };
  link: string[];
  showAllInterfaces: boolean;
  logLevel: string;
};
type Operation = {
  id: string;
  state: "applying" | "succeeded" | "failed" | "rollback_failed";
  target_revision: string;
  error?: string;
};

class SettingsError extends Error {
  constructor(
    public field: string,
    message = "Invalid general setting",
  ) {
    super(message);
  }
}

function record(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function fields(value: unknown, names: readonly string[], path = ""): Record<string, unknown> {
  const object = record(value);
  const extra = Object.keys(object).find((key) => !names.includes(key));
  const missing = names.find((key) => !Object.hasOwn(object, key));
  if (extra || missing) throw new SettingsError([path, extra ?? missing].filter(Boolean).join("."));
  return object;
}

function boolean(value: unknown, field: string): boolean {
  if (typeof value !== "boolean") throw new SettingsError(field);
  return value;
}

function integer(value: unknown, field: string, minimum: number, maximum = UINT32_MAX): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < minimum || value > maximum)
    throw new SettingsError(field);
  return value;
}

function host(value: unknown, field: string, upstream = false): Host {
  const input = fields(value, ["address", "port"], field);
  const address = input.address;
  const literal = typeof address === "string" ? address.replace(/^\[([^\]]+)\]$/, "$1") : "";
  const hostname =
    upstream &&
    typeof address === "string" &&
    address.length < 256 &&
    /^[A-Za-z0-9_.-]+$/.test(address);
  if (typeof address !== "string" || (!isIP(literal) && !hostname))
    throw new SettingsError(`${field}.address`);
  return { address, port: integer(input.port, `${field}.port`, 1, 65535) };
}

function readSettings(value: unknown): MockGeneralSettings {
  const settings = fields(value, [
    "httpWeb",
    "dnsProxy",
    "netfilter",
    "link",
    "showAllInterfaces",
    "logLevel",
  ]);
  const http = fields(settings.httpWeb, ["enabled", "auth", "host"], "httpWeb");
  const auth = fields(http.auth, ["enabled"], "httpWeb.auth");
  const dns = fields(
    settings.dnsProxy,
    [
      "host",
      "upstream",
      "disableRemap53",
      "disableFakePTR",
      "disableDropAAAA",
      "maxIdleConns",
      "maxConcurrent",
      "timeoutMs",
    ],
    "dnsProxy",
  );
  const netfilter = fields(
    settings.netfilter,
    ["disableIPv4", "disableIPv6", "ipset"],
    "netfilter",
  );
  const ipset = fields(netfilter.ipset, ["additionalTTLSeconds"], "netfilter.ipset");
  if (
    !Array.isArray(settings.link) ||
    settings.link.some(
      (name) =>
        typeof name !== "string" ||
        !name ||
        new TextEncoder().encode(name).length >= 16 ||
        /[\s/\x00-\x1f\x7f]/.test(name),
    ) ||
    new Set(settings.link).size !== settings.link.length
  )
    throw new SettingsError("link");
  if (
    typeof settings.logLevel !== "string" ||
    !["trace", "debug", "info", "warn", "error", "fatal", "panic", "nolevel", "disabled"].includes(
      settings.logLevel,
    )
  )
    throw new SettingsError("logLevel");
  return {
    httpWeb: {
      enabled: boolean(http.enabled, "httpWeb.enabled"),
      auth: { enabled: boolean(auth.enabled, "httpWeb.auth.enabled") },
      host: host(http.host, "httpWeb.host"),
    },
    dnsProxy: {
      host: host(dns.host, "dnsProxy.host"),
      upstream: host(dns.upstream, "dnsProxy.upstream", true),
      disableRemap53: boolean(dns.disableRemap53, "dnsProxy.disableRemap53"),
      disableFakePTR: boolean(dns.disableFakePTR, "dnsProxy.disableFakePTR"),
      disableDropAAAA: boolean(dns.disableDropAAAA, "dnsProxy.disableDropAAAA"),
      maxIdleConns: integer(dns.maxIdleConns, "dnsProxy.maxIdleConns", 0),
      maxConcurrent: integer(dns.maxConcurrent, "dnsProxy.maxConcurrent", 1),
      timeoutMs: integer(dns.timeoutMs, "dnsProxy.timeoutMs", 1),
    },
    netfilter: {
      disableIPv4: boolean(netfilter.disableIPv4, "netfilter.disableIPv4"),
      disableIPv6: boolean(netfilter.disableIPv6, "netfilter.disableIPv6"),
      ipset: {
        additionalTTLSeconds: integer(
          ipset.additionalTTLSeconds,
          "netfilter.ipset.additionalTTLSeconds",
          0,
        ),
      },
    },
    link: [...settings.link] as string[],
    showAllInterfaces: boolean(settings.showAllInterfaces, "showAllInterfaces"),
    logLevel: settings.logLevel,
  };
}

function duration(value: unknown, unitNanoseconds: number): number {
  if (typeof value === "number") return value / unitNanoseconds;
  if (typeof value !== "string") return NaN;
  const units: Record<string, number> = {
    ns: 1,
    us: 1000,
    µs: 1000,
    μs: 1000,
    ms: 1e6,
    s: 1e9,
    m: 6e10,
    h: 36e11,
  };
  let result = 0;
  let end = 0;
  for (const part of value.matchAll(/(\d+(?:\.\d+)?)(ns|us|µs|μs|ms|s|m|h)/g)) {
    if (part.index !== end) return NaN;
    result += Number(part[1]) * units[part[2]];
    end += part[0].length;
  }
  return end === value.length && end > 0 ? result / unitNanoseconds : NaN;
}

function project(config: ServerConfig): MockGeneralSettings {
  const app = config.app;
  const http = record(app.httpWeb);
  const dns = record(app.dnsProxy);
  const netfilter = record(app.netfilter);
  return readSettings({
    httpWeb: { enabled: http.enabled, auth: http.auth, host: http.host },
    dnsProxy: {
      host: dns.host,
      upstream: dns.upstream,
      disableRemap53: dns.disableRemap53,
      disableFakePTR: dns.disableFakePTR,
      disableDropAAAA: dns.disableDropAAAA,
      maxIdleConns: dns.maxIdleConns,
      maxConcurrent: dns.maxConcurrent,
      timeoutMs: duration(dns.timeout, 1e6),
    },
    netfilter: {
      disableIPv4: netfilter.disableIPv4,
      disableIPv6: netfilter.disableIPv6,
      ipset: { additionalTTLSeconds: duration(record(netfilter.ipset).additionalTTL, 1e9) },
    },
    link: app.link,
    showAllInterfaces: app.showAllInterfaces,
    logLevel: app.logLevel,
  });
}

function overlay(config: ServerConfig, settings: MockGeneralSettings): ServerConfig {
  const current = structuredClone(config);
  const http = record(current.app.httpWeb);
  const dns = record(current.app.dnsProxy);
  const netfilter = record(current.app.netfilter);
  const { timeoutMs, ...dnsFields } = settings.dnsProxy;
  current.app = {
    ...current.app,
    httpWeb: { ...http, ...settings.httpWeb },
    dnsProxy: { ...dns, ...dnsFields, timeout: `${timeoutMs}ms` },
    netfilter: {
      ...netfilter,
      disableIPv4: settings.netfilter.disableIPv4,
      disableIPv6: settings.netfilter.disableIPv6,
      ipset: {
        ...record(netfilter.ipset),
        additionalTTL: `${settings.netfilter.ipset.additionalTTLSeconds}s`,
      },
    },
    link: [...settings.link],
    showAllInterfaces: settings.showAllInterfaces,
    logLevel: settings.logLevel,
  } as ServerConfig["app"];
  return current;
}

async function revision(settings: MockGeneralSettings): Promise<string> {
  const digest = await crypto.subtle.digest(
    "SHA-256",
    new TextEncoder().encode(fingerprint(settings)),
  );
  return Array.from(new Uint8Array(digest), (byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Match the native runtime's restart boundary; the browser asks the API. */
export function mockRequiresDnsRestart(
  current: MockGeneralSettings,
  next: MockGeneralSettings,
): boolean {
  const runtime = (settings: MockGeneralSettings) => ({
    httpWeb: { enabled: settings.httpWeb.enabled, host: settings.httpWeb.host },
    dnsProxy: {
      host: settings.dnsProxy.host,
      upstream: settings.dnsProxy.upstream,
      disableRemap53: settings.dnsProxy.disableRemap53,
      maxIdleConns: settings.dnsProxy.maxIdleConns,
      maxConcurrent: settings.dnsProxy.maxConcurrent,
      timeoutMs: settings.dnsProxy.timeoutMs,
    },
    netfilter: {
      disableIPv4: settings.netfilter.disableIPv4,
      disableIPv6: settings.netfilter.disableIPv6,
    },
    link: settings.link,
  });
  return fingerprint(runtime(current)) !== fingerprint(runtime(next));
}

async function readRequest(request: Request, allowRestartFlag = false) {
  if (
    !/^application\/json(?:\s*;|\s*$)/i.test(request.headers.get("Content-Type") ?? "") ||
    request.headers.get("Sec-Fetch-Site") === "cross-site"
  )
    throw new SettingsError("", "Expected a same-origin JSON request");
  let input: unknown;
  try {
    input = await request.json();
  } catch {
    throw new SettingsError("", "Invalid JSON");
  }
  const hasRestartFlag = allowRestartFlag && Object.hasOwn(record(input), "allow_dns_restart");
  const body = fields(input, [
    "settings",
    "revision",
    ...(hasRestartFlag ? ["allow_dns_restart"] : []),
  ]);
  if (typeof body.revision !== "string") throw new SettingsError("revision");
  return {
    settings: readSettings(body.settings),
    revision: body.revision,
    allowDnsRestart: hasRestartFlag ? boolean(body.allow_dns_restart, "allow_dns_restart") : true,
  };
}

/** In-memory lifecycle simulation. The next status GET applies the queued
 * projection to the latest model; real listeners, DNS and disk are unchanged. */
export function registerMockSettingsRoutes(
  app: Hono,
  source: {
    get: () => ServerConfig;
    apply: (config: unknown) => void;
    runtimeActive?: () => boolean;
  },
) {
  let operation: Operation | null = null;
  let pending:
    | {
        settings: MockGeneralSettings;
        baseline: string;
        allowDnsRestart: boolean;
        dnsRestartRequired: boolean;
      }
    | undefined;
  let accepting = false;
  const runtimeActive = () => source.runtimeActive?.() ?? true;
  const requiresRestart = (current: MockGeneralSettings, next: MockGeneralSettings) =>
    !runtimeActive() || mockRequiresDnsRestart(current, next);

  function completePending() {
    if (!pending || !operation) return;
    const request = pending;
    pending = undefined;
    try {
      const current = source.get();
      if (fingerprint(project(current)) !== request.baseline)
        throw new Error("General settings changed before application");
      if (
        (!request.allowDnsRestart || !request.dnsRestartRequired) &&
        requiresRestart(project(current), request.settings)
      )
        throw new Error("A DNS restart became necessary before application; save again to confirm");
      source.apply(overlay(current, request.settings));
      operation = { ...operation, state: "succeeded" };
    } catch (error) {
      operation = {
        ...operation,
        state: "failed",
        error: error instanceof Error ? error.message : "Could not apply settings",
      };
    }
  }

  app.get(BASE, async (c) => {
    c.header("Cache-Control", "no-store");
    completePending();
    const settings = project(source.get());
    const currentOperation = operation && { ...operation };
    return c.json({ settings, revision: await revision(settings), operation: currentOperation });
  });

  app.post(`${BASE}/preview`, async (c) => {
    c.header("Cache-Control", "no-store");
    if (pending || accepting)
      return c.json({ code: "SETTINGS_BUSY", error: "Settings are already being applied" }, 409);
    try {
      const input = await readRequest(c.req.raw);
      const current = project(source.get());
      const baseline = fingerprint(current);
      if (
        input.revision !== (await revision(current)) ||
        fingerprint(project(source.get())) !== baseline
      )
        return c.json(
          { code: "SETTINGS_CONFLICT", error: "General settings changed; reload before saving" },
          409,
        );
      return c.json({ dns_restart_required: requiresRestart(current, input.settings) });
    } catch (error) {
      if (error instanceof SettingsError)
        return c.json(
          {
            code: "SETTINGS_INVALID",
            error: error.message,
            ...(error.field ? { field: error.field } : {}),
          },
          400,
        );
      throw error;
    }
  });

  app.put(BASE, async (c) => {
    c.header("Cache-Control", "no-store");
    if (pending || accepting)
      return c.json({ code: "SETTINGS_BUSY", error: "Settings are already being applied" }, 409);
    accepting = true;
    try {
      const input = await readRequest(c.req.raw, true);
      const settings = input.settings;
      const current = project(source.get());
      const baseline = fingerprint(current);
      if (input.revision !== (await revision(current)))
        return c.json(
          { code: "SETTINGS_CONFLICT", error: "General settings changed; reload before saving" },
          409,
        );
      const target_revision = await revision(settings);
      if (fingerprint(project(source.get())) !== baseline)
        return c.json(
          { code: "SETTINGS_CONFLICT", error: "General settings changed; reload before saving" },
          409,
        );
      const dnsRestartRequired = requiresRestart(current, settings);
      if (!input.allowDnsRestart && dnsRestartRequired)
        return c.json(
          {
            code: "SETTINGS_RESTART_REQUIRED",
            error: "A DNS restart must be confirmed before saving",
          },
          409,
        );
      const unchanged = fingerprint(settings) === baseline && runtimeActive();
      operation = {
        id: crypto.randomUUID().replaceAll("-", ""),
        state: unchanged ? "succeeded" : "applying",
        target_revision,
      };
      if (!unchanged)
        pending = {
          settings,
          baseline,
          allowDnsRestart: input.allowDnsRestart,
          dnsRestartRequired,
        };
      return c.json(
        {
          operation,
          target_revision,
          next_web: {
            enabled: settings.httpWeb.enabled,
            address: settings.httpWeb.host.address,
            port: settings.httpWeb.host.port,
            auth_enabled: settings.httpWeb.auth.enabled,
          },
        },
        unchanged ? 200 : 202,
      );
    } catch (error) {
      if (error instanceof SettingsError)
        return c.json(
          {
            code: "SETTINGS_INVALID",
            error: error.message,
            ...(error.field ? { field: error.field } : {}),
          },
          400,
        );
      throw error;
    } finally {
      accepting = false;
    }
  });
}

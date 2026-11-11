import {
  array,
  boolean,
  check,
  integer,
  maxValue,
  minValue,
  nonEmpty,
  nullable,
  number,
  object,
  optional,
  parse,
  picklist,
  pipe,
  safeParse,
  strictObject,
  string,
  type InferOutput,
} from "valibot";

import { HttpError } from "../../../utils/http-error";

export const UINT32_MAX = 4294967295;
export const logLevels = [
  "trace",
  "debug",
  "info",
  "warn",
  "error",
  "fatal",
  "panic",
  "nolevel",
  "disabled",
] as const;

function listenAddress(value: string): boolean {
  if (!value || value !== value.trim() || /[\s/\\@?#]/.test(value)) return false;
  const address = value.startsWith("[") && value.endsWith("]") ? value.slice(1, -1) : value;
  if (address.includes(":")) {
    try {
      return new URL(`http://[${address}]/`).hostname.startsWith("[");
    } catch {
      return false;
    }
  }
  const parts = address.split(".");
  return (
    parts.length === 4 &&
    parts.every((part) => /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255)
  );
}

function upstreamAddress(value: string): boolean {
  return (
    listenAddress(value) || (value.length > 0 && value.length < 256 && /^[a-z\d_.-]+$/i.test(value))
  );
}

function unsigned(
  min: number,
  max = UINT32_MAX,
  message = min === 0
    ? "Enter a whole number from 0 to 4294967295."
    : "Enter a whole number from 1 to 4294967295.",
) {
  return pipe(number(message), integer(message), minValue(min, message), maxValue(max, message));
}
const port = unsigned(1, 65535, "Enter a valid port from 1 to 65535.");
const localAddress = pipe(string(), check(listenAddress, "Enter an IPv4 or IPv6 address."));
const dnsAddress = pipe(string(), check(upstreamAddress, "Enter an IP address or hostname."));
const interfaceNames = pipe(
  array(
    pipe(
      string(),
      check(
        (value) =>
          Boolean(value) &&
          new TextEncoder().encode(value).length < 16 &&
          !/[\s/\x00-\x1f\x7f]/.test(value),
        "Use interface names of up to 15 bytes, without spaces or slashes.",
      ),
    ),
  ),
  check((names) => new Set(names).size === names.length, "Interface names must not repeat."),
);

/** This is the editable projection, never the complete app configuration. */
export const generalSettingsSchema = strictObject({
  httpWeb: strictObject({
    enabled: boolean(),
    auth: strictObject({ enabled: boolean() }),
    host: strictObject({ address: localAddress, port }),
  }),
  dnsProxy: strictObject({
    host: strictObject({ address: localAddress, port }),
    upstream: strictObject({ address: dnsAddress, port }),
    disableRemap53: boolean(),
    disableFakePTR: boolean(),
    disableDropAAAA: boolean(),
    maxIdleConns: unsigned(0),
    maxConcurrent: unsigned(1),
    timeoutMs: unsigned(1),
  }),
  netfilter: strictObject({
    disableIPv4: boolean(),
    disableIPv6: boolean(),
    ipset: strictObject({ additionalTTLSeconds: unsigned(0) }),
  }),
  link: interfaceNames,
  showAllInterfaces: boolean(),
  logLevel: picklist(logLevels, "Select a log level."),
});
export type GeneralSettings = InferOutput<typeof generalSettingsSchema>;

const revision = pipe(string(), nonEmpty());
const operationSchema = object({
  id: pipe(string(), nonEmpty()),
  state: picklist(["applying", "succeeded", "failed", "rollback_failed"]),
  target_revision: optional(revision),
  error: optional(string()),
});
const webTargetSchema = object({
  enabled: boolean(),
  address: localAddress,
  port,
  auth_enabled: boolean(),
});
const snapshotSchema = object({
  settings: generalSettingsSchema,
  revision,
  operation: nullable(operationSchema),
});
const previewSchema = object({ dns_restart_required: boolean() });
const acknowledgementSchema = object({
  operation: operationSchema,
  target_revision: revision,
  next_web: webTargetSchema,
});
export type SettingsSnapshot = InferOutput<typeof snapshotSchema>;
export type SettingsPreview = InferOutput<typeof previewSchema>;
export type SettingsOperation = InferOutput<typeof operationSchema>;
export type SettingsAcknowledgement = InferOutput<typeof acknowledgementSchema>;
export type WebTarget = InferOutput<typeof webTargetSchema>;
export type SettingsError = { message: string; code?: string; field?: string };

export function readSettingsSnapshot(value: unknown): SettingsSnapshot {
  try {
    return parse(snapshotSchema, value);
  } catch {
    throw new Error("Invalid general settings response.");
  }
}

export function readSettingsPreview(value: unknown): SettingsPreview {
  try {
    return parse(previewSchema, value);
  } catch {
    throw new Error("Invalid settings preview response.");
  }
}

export function readSettingsAcknowledgement(value: unknown): SettingsAcknowledgement {
  try {
    const result = parse(acknowledgementSchema, value);
    if (!["applying", "succeeded"].includes(result.operation.state)) throw new Error();
    return result;
  } catch {
    throw new Error("The server did not confirm the settings request.");
  }
}

export function readSettingsError(error: unknown): SettingsError {
  if (error instanceof HttpError && error.data && typeof error.data === "object") {
    const data = error.data as Record<string, unknown>;
    return {
      message: typeof data.error === "string" ? data.error : error.message,
      ...(typeof data.code === "string" ? { code: data.code } : {}),
      ...(typeof data.field === "string" ? { field: data.field } : {}),
    };
  }
  return { message: error instanceof Error ? error.message : "Request failed" };
}

export type GeneralDraft = {
  upstreamAddress: string;
  upstreamPort: string;
  dnsAddress: string;
  dnsPort: string;
  interceptDns: boolean;
  blockPtr: boolean;
  filterAaaa: boolean;
  timeoutMs: string;
  maxConcurrent: string;
  maxIdleConns: string;
  links: string;
  ipv4: boolean;
  ipv6: boolean;
  additionalTTLSeconds: string;
  showAllInterfaces: boolean;
  webEnabled: boolean;
  authEnabled: boolean;
  webAddress: string;
  webPort: string;
  logLevel: string;
};
export type DraftField = keyof GeneralDraft;
export type TextField = {
  [K in DraftField]: GeneralDraft[K] extends string ? K : never;
}[DraftField];
export type SwitchField = Exclude<DraftField, TextField>;

export function emptyDraft(): GeneralDraft {
  return {
    upstreamAddress: "",
    upstreamPort: "",
    dnsAddress: "",
    dnsPort: "",
    interceptDns: false,
    blockPtr: false,
    filterAaaa: false,
    timeoutMs: "",
    maxConcurrent: "",
    maxIdleConns: "",
    links: "",
    ipv4: false,
    ipv6: false,
    additionalTTLSeconds: "",
    showAllInterfaces: false,
    webEnabled: false,
    authEnabled: false,
    webAddress: "",
    webPort: "",
    logLevel: "",
  };
}

/** Linux interface names may contain commas or backslashes. Escape them in
 * the compact list editor so a save of another setting cannot split a name. */
export function formatInterfaceNames(names: readonly string[]): string {
  return names.map((name) => name.replaceAll("\\", "\\\\").replaceAll(",", "\\,")).join(", ");
}

export function parseInterfaceNames(value: string): string[] {
  const names: string[] = [];
  let current = "";
  for (let index = 0; index < value.length; index++) {
    const char = value[index];
    if (
      char === "\\" &&
      index + 1 < value.length &&
      (value[index + 1] === "," || value[index + 1] === "\\")
    ) {
      current += value[++index];
    } else if (char === "," || /\s/.test(char)) {
      if (current) names.push(current);
      current = "";
    } else {
      current += char;
    }
  }
  if (current) names.push(current);
  return names;
}

export function settingsToDraft(settings: GeneralSettings): GeneralDraft {
  return {
    upstreamAddress: settings.dnsProxy.upstream.address,
    upstreamPort: String(settings.dnsProxy.upstream.port),
    dnsAddress: settings.dnsProxy.host.address,
    dnsPort: String(settings.dnsProxy.host.port),
    interceptDns: !settings.dnsProxy.disableRemap53,
    blockPtr: !settings.dnsProxy.disableFakePTR,
    filterAaaa: !settings.dnsProxy.disableDropAAAA,
    timeoutMs: String(settings.dnsProxy.timeoutMs),
    maxConcurrent: String(settings.dnsProxy.maxConcurrent),
    maxIdleConns: String(settings.dnsProxy.maxIdleConns),
    links: formatInterfaceNames(settings.link),
    ipv4: !settings.netfilter.disableIPv4,
    ipv6: !settings.netfilter.disableIPv6,
    additionalTTLSeconds: String(settings.netfilter.ipset.additionalTTLSeconds),
    showAllInterfaces: settings.showAllInterfaces,
    webEnabled: settings.httpWeb.enabled,
    authEnabled: settings.httpWeb.auth.enabled,
    webAddress: settings.httpWeb.host.address,
    webPort: String(settings.httpWeb.host.port),
    logLevel: settings.logLevel,
  };
}

const fieldPaths: Record<string, DraftField> = {
  "httpWeb.enabled": "webEnabled",
  "httpWeb.auth.enabled": "authEnabled",
  "httpWeb.host.address": "webAddress",
  "httpWeb.host.port": "webPort",
  "dnsProxy.host.address": "dnsAddress",
  "dnsProxy.host.port": "dnsPort",
  "dnsProxy.upstream.address": "upstreamAddress",
  "dnsProxy.upstream.port": "upstreamPort",
  "dnsProxy.disableRemap53": "interceptDns",
  "dnsProxy.disableFakePTR": "blockPtr",
  "dnsProxy.disableDropAAAA": "filterAaaa",
  "dnsProxy.timeoutMs": "timeoutMs",
  "dnsProxy.maxConcurrent": "maxConcurrent",
  "dnsProxy.maxIdleConns": "maxIdleConns",
  "netfilter.disableIPv4": "ipv4",
  "netfilter.disableIPv6": "ipv6",
  "netfilter.ipset.additionalTTLSeconds": "additionalTTLSeconds",
  link: "links",
  showAllInterfaces: "showAllInterfaces",
  logLevel: "logLevel",
};
export const fieldForPath = (path: string): DraftField | undefined =>
  fieldPaths[path.replace(/^settings\./, "")];

export function buildGeneralSettings(draft: GeneralDraft): {
  settings?: GeneralSettings;
  errors: Partial<Record<DraftField, string>>;
} {
  const numeric = (value: string) => (/^\d+$/.test(value.trim()) ? Number(value) : Number.NaN);
  const result = safeParse(generalSettingsSchema, {
    httpWeb: {
      enabled: draft.webEnabled,
      auth: { enabled: draft.authEnabled },
      host: { address: draft.webAddress.trim(), port: numeric(draft.webPort) },
    },
    dnsProxy: {
      host: { address: draft.dnsAddress.trim(), port: numeric(draft.dnsPort) },
      upstream: { address: draft.upstreamAddress.trim(), port: numeric(draft.upstreamPort) },
      disableRemap53: !draft.interceptDns,
      disableFakePTR: !draft.blockPtr,
      disableDropAAAA: !draft.filterAaaa,
      timeoutMs: numeric(draft.timeoutMs),
      maxConcurrent: numeric(draft.maxConcurrent),
      maxIdleConns: numeric(draft.maxIdleConns),
    },
    netfilter: {
      disableIPv4: !draft.ipv4,
      disableIPv6: !draft.ipv6,
      ipset: { additionalTTLSeconds: numeric(draft.additionalTTLSeconds) },
    },
    link: parseInterfaceNames(draft.links),
    showAllInterfaces: draft.showAllInterfaces,
    logLevel: draft.logLevel,
  });
  if (result.success) return { settings: result.output, errors: {} };
  const errors: Partial<Record<DraftField, string>> = {};
  for (const issue of result.issues) {
    const path = issue.path?.map((part) => String(part.key)).join(".") ?? "";
    const key = fieldForPath(path.startsWith("link.") ? "link" : path);
    if (key && !errors[key]) errors[key] = issue.message;
  }
  return { errors };
}

export const sameSettings = (a: GeneralSettings, b: GeneralSettings) =>
  JSON.stringify(settingsToDraft(a)) === JSON.stringify(settingsToDraft(b));

export function webTarget(settings: GeneralSettings): WebTarget {
  return {
    enabled: settings.httpWeb.enabled,
    ...settings.httpWeb.host,
    auth_enabled: settings.httpWeb.auth.enabled,
  };
}
export function changesWebAccess(a: GeneralSettings, b: GeneralSettings): boolean {
  return JSON.stringify(webTarget(a)) !== JSON.stringify(webTarget(b));
}

/** Only a user-clicked link is produced; credentials are never copied to it. */
export function webTargetUrl(target: WebTarget, currentUrl: string): string | undefined {
  if (!target.enabled) return;
  const url = new URL(currentUrl);
  const address = target.address.replace(/^\[|\]$/g, "");
  const wildcard = address === "0.0.0.0" || address === "::";
  if (!wildcard) url.hostname = address.includes(":") ? `[${address}]` : address;
  url.port = String(target.port);
  url.pathname = "/";
  url.search = "";
  url.hash = "";
  url.username = "";
  url.password = "";
  return url.href;
}

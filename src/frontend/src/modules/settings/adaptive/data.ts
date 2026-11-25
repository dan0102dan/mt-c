import {
  array,
  boolean,
  integer,
  maxValue,
  minValue,
  nullable,
  number,
  object,
  optional,
  parse,
  picklist,
  pipe,
  string,
  type InferOutput,
} from "valibot";

const bounded = (min: number, max: number) =>
  pipe(number(), integer(), minValue(min), maxValue(max));

export const AdaptiveSettingsSchema = object({
  enabled: boolean(),
  tcpAllPorts: optional(boolean(), false),
  quicEnabled: optional(boolean(), false),
  interface: string(),
  ingressInterfaces: array(string()),
  connectTimeoutMs: bounded(100, 60000),
  tlsTimeoutMs: bounded(100, 60000),
  idleTimeoutMs: bounded(1000, 3600000),
  ttlSeconds: bounded(30, 86400),
  maxConnections: bounded(1, 1024),
  maxEntries: bounded(1, 65536),
});
export type AdaptiveSettings = InferOutput<typeof AdaptiveSettingsSchema>;

export const adaptiveDefaults: AdaptiveSettings = {
  enabled: false,
  tcpAllPorts: false,
  quicEnabled: false,
  interface: "",
  ingressInterfaces: [],
  connectTimeoutMs: 2000,
  tlsTimeoutMs: 3000,
  idleTimeoutMs: 300000,
  ttlSeconds: 600,
  maxConnections: 128,
  maxEntries: 2048,
};

const counter = bounded(0, Number.MAX_SAFE_INTEGER);
const AdaptiveStatusSchema = object({
  settings: AdaptiveSettingsSchema,
  active: boolean(),
  error: nullable(string()),
  interfaces: array(object({ id: string(), name: optional(string()) })),
  entries: array(
    object({
      address: string(),
      family: picklist([4, 6]),
      protocol: optional(picklist(["tcp", "udp"]), "tcp"),
      port: bounded(1, 65535),
      alternate: boolean(),
      expiresInSeconds: counter,
    }),
  ),
  stats: object({
    active: counter,
    accepted: counter,
    directSuccess: counter,
    alternateSuccess: counter,
    failures: counter,
    overLimit: counter,
  }),
});
export type AdaptiveStatus = InferOutput<typeof AdaptiveStatusSchema>;
export type AdaptiveDraft = Omit<
  AdaptiveSettings,
  "connectTimeoutMs" | "tlsTimeoutMs" | "ttlSeconds"
> & {
  connectTimeoutMs: string;
  tlsTimeoutMs: string;
  ttlSeconds: string;
};
export type AdaptiveField =
  | "interface"
  | "ingressInterfaces"
  | "connectTimeoutMs"
  | "tlsTimeoutMs"
  | "ttlSeconds";

export function validInterface(id: string): boolean {
  return (
    /^[a-zA-Z0-9_.:-]{1,15}$/.test(id) && !id.startsWith("-") && id !== "lo" && id !== "blackhole"
  );
}

function validScope(settings: AdaptiveSettings): boolean {
  const ingress = settings.ingressInterfaces;
  return (
    (!settings.interface || validInterface(settings.interface)) &&
    ingress.length <= 16 &&
    ingress.every(validInterface) &&
    new Set(ingress).size === ingress.length &&
    (!settings.interface || !ingress.includes(settings.interface)) &&
    (!settings.enabled || Boolean(settings.interface && ingress.length))
  );
}

export function readAdaptiveSettings(value: unknown): AdaptiveSettings {
  const settings = parse(AdaptiveSettingsSchema, value);
  if (!validScope(settings)) throw new Error("Invalid adaptive routing settings");
  return settings;
}

export function readAdaptiveStatus(value: unknown): AdaptiveStatus {
  const status = parse(AdaptiveStatusSchema, value);
  if (
    !validScope(status.settings) ||
    status.interfaces.some((item) => !validInterface(item.id)) ||
    new Set(status.interfaces.map((item) => item.id)).size !== status.interfaces.length ||
    status.entries.some(
      (entry) =>
        !entry.address ||
        /[\s\x00-\x1f\x7f]/.test(entry.address) ||
        (entry.protocol === "udp" && entry.port !== 443),
    ) ||
    new Set(
      status.entries.map(
        (entry) => `${entry.protocol}/${entry.family}/${entry.address}/${entry.port}`,
      ),
    ).size !== status.entries.length
  )
    throw new Error("Invalid adaptive routing response");
  return status;
}

export function adaptiveDraft(settings: AdaptiveSettings): AdaptiveDraft {
  return {
    ...settings,
    ingressInterfaces: [...settings.ingressInterfaces],
    connectTimeoutMs: String(settings.connectTimeoutMs),
    tlsTimeoutMs: String(settings.tlsTimeoutMs),
    ttlSeconds: String(settings.ttlSeconds),
  };
}

export function buildAdaptiveSettings(draft: AdaptiveDraft): {
  settings?: AdaptiveSettings;
  errors: Partial<Record<AdaptiveField, string>>;
} {
  const errors: Partial<Record<AdaptiveField, string>> = {};
  if ((draft.enabled && !draft.interface) || (draft.interface && !validInterface(draft.interface)))
    errors.interface = "Choose an alternate interface.";
  if (
    (draft.enabled && !draft.ingressInterfaces.length) ||
    draft.ingressInterfaces.length > 16 ||
    !draft.ingressInterfaces.every(validInterface) ||
    new Set(draft.ingressInterfaces).size !== draft.ingressInterfaces.length
  )
    errors.ingressInterfaces = "Choose between 1 and 16 client LAN interfaces.";
  if (draft.interface && draft.ingressInterfaces.includes(draft.interface))
    errors.ingressInterfaces = "The alternate interface cannot also be a client LAN interface.";

  const numeric = (
    key: "connectTimeoutMs" | "tlsTimeoutMs" | "ttlSeconds",
    min: number,
    max: number,
    message: string,
  ) => {
    const value = /^\d+$/.test(draft[key]) ? Number(draft[key]) : NaN;
    if (!Number.isSafeInteger(value) || value < min || value > max) errors[key] = message;
    return value;
  };
  const connectTimeoutMs = numeric(
    "connectTimeoutMs",
    100,
    60000,
    "Enter a connection timeout from 100 to 60000 ms.",
  );
  const tlsTimeoutMs = numeric(
    "tlsTimeoutMs",
    100,
    60000,
    "Enter a TLS timeout from 100 to 60000 ms.",
  );
  const ttlSeconds = numeric(
    "ttlSeconds",
    30,
    86400,
    "Enter a learning lifetime from 30 to 86400 seconds.",
  );
  if (Object.keys(errors).length) return { errors };
  return {
    settings: readAdaptiveSettings({
      ...draft,
      ingressInterfaces: [...draft.ingressInterfaces],
      connectTimeoutMs,
      tlsTimeoutMs,
      ttlSeconds,
    }),
    errors,
  };
}

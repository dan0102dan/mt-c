/** YAML field shapes from backend-c/src/config/yaml_save.c and yaml_load.c.
 * Keep YAML names here (not HTTP names such as lastUpdate). Missing/null fields
 * retain the loader's overlay semantics; semantic constraints remain server-side. */
export type YamlField =
  | "text"
  | "boolean"
  | "unsigned"
  | "priority"
  | "duration"
  | { fields: Record<string, YamlField> }
  | { items: YamlField };

const text: YamlField = "text";
const boolean: YamlField = "boolean";
const unsigned: YamlField = "unsigned";
const host: YamlField = { fields: { address: text, port: unsigned } };
const rule: YamlField = {
  fields: { id: text, name: text, type: text, rule: text, enable: boolean },
};
const routing = {
  id: text,
  name: text,
  interface: text,
  profile: text,
  enable: boolean,
  priority: "priority" as const,
};

export const yamlConfigSchema: YamlField = {
  fields: {
    configVersion: text,
    app: {
      fields: {
        httpWeb: {
          fields: { enabled: boolean, auth: { fields: { enabled: boolean } }, host, skin: text },
        },
        dnsProxy: {
          fields: {
            host,
            upstream: host,
            disableRemap53: boolean,
            disableFakePTR: boolean,
            disableDropAAAA: boolean,
            maxIdleConns: unsigned,
            maxConcurrent: unsigned,
            timeout: "duration",
          },
        },
        netfilter: {
          fields: {
            iptables: { fields: { chainPrefix: text } },
            ipset: { fields: { tablePrefix: text, additionalTTL: "duration" } },
            disableIPv4: boolean,
            disableIPv6: boolean,
            startMarkTableIndex: unsigned,
          },
        },
        keeneticPolicy: { fields: { enabled: boolean, policyId: text, mode: text } },
        link: { items: text },
        showAllInterfaces: boolean,
        logLevel: text,
      },
    },
    groups: { items: { fields: { ...routing, color: text, rules: { items: rule } } } },
    subscriptions: {
      items: {
        fields: {
          ...routing,
          url: text,
          interval: unsigned,
          last_update: unsigned,
          rules: { items: rule },
          overrides: { items: { fields: { type: text, rule: text, enable: boolean } } },
        },
      },
    },
    profiles: {
      items: {
        fields: { id: text, name: text, interfaces: { items: text }, on_unavailable: text },
      },
    },
  },
};

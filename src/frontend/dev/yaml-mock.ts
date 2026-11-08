import type { Hono } from "hono";
import { LineCounter, parseDocument, stringify } from "yaml";

import type { ServerConfig } from "../src/modules/config-transfer/adapter.ts";
import { fingerprint } from "../src/modules/config-transfer/model.ts";

const BASE = "/api/v1/system/config/yaml";

function startupSettings(config: ServerConfig) {
  const settings = structuredClone(config.app);
  delete settings.keeneticPolicy;
  delete settings.showAllInterfaces;
  const netfilter = settings.netfilter;
  if (netfilter && typeof netfilter === "object" && !Array.isArray(netfilter)) {
    const ipset = netfilter.ipset;
    if (ipset && typeof ipset === "object" && !Array.isArray(ipset)) delete ipset.additionalTTL;
  }
  return fingerprint(settings);
}

// In-memory simulation only: raw text survives until another mock route changes
// the model. No file/runtime divergence is possible here; the C API owns that case.
export function registerMockYamlRoutes(
  app: Hono,
  source: { get: () => ServerConfig; apply: (config: unknown) => void },
) {
  const startup = startupSettings(source.get());
  let modelRevision = "";
  let revision = 0;
  let yaml = "";

  function snapshot() {
    const config = source.get();
    const current = fingerprint(config);
    if (current !== modelRevision) {
      yaml = stringify(config);
      modelRevision = current;
      revision++;
    }
    return {
      yaml,
      revision: String(revision),
      restart_required: startupSettings(config) !== startup,
      out_of_sync: false,
    };
  }

  app.get(BASE, (c) => c.json(snapshot()));
  app.put(BASE, async (c) => {
    let input: unknown;
    try {
      input = await c.req.json();
    } catch {
      return c.json({ error: "Invalid JSON", code: "INVALID_CONFIG" }, 400);
    }
    if (!input || typeof input !== "object" || Array.isArray(input))
      return c.json({ error: "YAML and revision are required", code: "INVALID_CONFIG" }, 400);
    const body = input as Record<string, unknown>;
    if (
      typeof body.yaml !== "string" ||
      typeof body.revision !== "string" ||
      (body.allow_out_of_sync !== undefined && typeof body.allow_out_of_sync !== "boolean")
    )
      return c.json({ error: "YAML and revision are required", code: "INVALID_CONFIG" }, 400);
    if (body.revision !== snapshot().revision)
      return c.json(
        { error: "Configuration changed; reload before saving", code: "CONFIG_CONFLICT" },
        409,
      );

    const lineCounter = new LineCounter();
    const document = parseDocument(body.yaml, {
      lineCounter,
      prettyErrors: false,
      stringKeys: true,
    });
    const syntaxError = document.errors[0] ?? document.warnings[0];
    if (syntaxError) {
      const position = lineCounter.linePos(syntaxError.pos[0]);
      return c.json(
        {
          error: syntaxError.message,
          code: "INVALID_CONFIG",
          line: position.line,
          column: position.col,
        },
        400,
      );
    }
    try {
      source.apply(document.toJS());
    } catch (error) {
      return c.json(
        {
          error: error instanceof Error ? error.message : "Invalid configuration",
          code: "INVALID_CONFIG",
        },
        400,
      );
    }
    yaml = body.yaml;
    modelRevision = fingerprint(source.get());
    revision++;
    return c.json(snapshot());
  });
}

import type { Hono } from "hono";

import type {
  Policy,
  PolicySettings,
  Status,
} from "../src/modules/settings/keenetic/state.svelte.ts";

const BASE = "/api/v1/keenetic";
const POLICIES: Policy[] = [
  { id: "Policy0", name: "Default", available: true },
  { id: "Policy1", name: "VPN", available: true },
  { id: "Policy2", name: "Guest", available: true },
  { id: "Policy3", name: "Offline VPN", available: false },
];

// In-memory dev simulation: no RCI calls or routing changes. Restart resets settings.
export function registerMockPolicyRoutes(
  app: Hono,
  source?: {
    get: () => PolicySettings;
    set: (settings: PolicySettings) => void;
  },
) {
  let settings: PolicySettings = { enabled: false, policy_id: "", mode: "include" };
  function status(): Status {
    const current = source?.get() ?? settings;
    const selected =
      POLICIES.find((p) => p.id === current.policy_id) ??
      (current.policy_id === "mt" ? POLICIES.find((p) => p.name === "mt") : undefined);
    return {
      settings: current,
      runtime: !current.enabled
        ? { state: "disabled" }
        : selected?.available
          ? { state: "ready" }
          : { state: "unavailable", reason: selected ? "POLICY_UNAVAILABLE" : "POLICY_NOT_FOUND" },
    };
  }

  app.get(`${BASE}/policies`, (c) => c.json({ policies: POLICIES }));
  app.get(`${BASE}/policy`, (c) => c.json(status()));
  app.put(`${BASE}/policy`, async (c) => {
    let input: unknown;
    try {
      input = await c.req.json();
    } catch {
      return c.json({ error: "Invalid JSON" }, 400);
    }
    if (!input || typeof input !== "object" || Array.isArray(input))
      return c.json({ error: "Invalid policy settings" }, 400);
    const body = input as Record<string, unknown>;
    if (
      Object.keys(body).length !== 3 ||
      typeof body.enabled !== "boolean" ||
      typeof body.policy_id !== "string" ||
      new TextEncoder().encode(body.policy_id).length >= 128 ||
      /[\x00-\x1f\x7f]/.test(body.policy_id) ||
      (body.mode !== "include" && body.mode !== "exclude") ||
      (body.enabled && !body.policy_id)
    ) {
      return c.json(
        { error: "Expected enabled (boolean), policy_id (string) and mode (include or exclude)" },
        400,
      );
    }
    settings = { enabled: body.enabled, policy_id: body.policy_id, mode: body.mode };
    source?.set(settings);
    return c.json(status());
  });
}

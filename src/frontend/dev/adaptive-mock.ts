import type { Hono } from "hono";

import {
  adaptiveDefaults,
  readAdaptiveSettings,
  type AdaptiveSettings,
} from "../src/modules/settings/adaptive/data.ts";

/** UI development only: no network interception or reachability checks. */
export function registerMockAdaptiveRoutes(app: Hono) {
  let settings: AdaptiveSettings = structuredClone(adaptiveDefaults);
  const snapshot = () => ({
    settings: structuredClone(settings),
    active: settings.enabled,
    error: null,
    interfaces: [
      { id: "br-lan", name: "Local network" },
      { id: "eth1" },
      { id: "wg0", name: "WireGuard Interface" },
      { id: "nwg0" },
    ],
    entries: [],
    stats: {
      active: 0,
      accepted: 0,
      directSuccess: 0,
      alternateSuccess: 0,
      failures: 0,
      overLimit: 0,
    },
  });
  app.get("/api/v1/adaptive", (c) => c.json(snapshot()));
  app.put("/api/v1/adaptive", async (c) => {
    try {
      const value: unknown = await c.req.json();
      if (!value || typeof value !== "object" || !("settings" in value))
        throw new Error("Missing settings");
      settings = readAdaptiveSettings(value.settings);
      return c.json(snapshot());
    } catch {
      return c.json({ code: "ADAPTIVE_INVALID", error: "Invalid adaptive routing settings" }, 400);
    }
  });
  app.delete("/api/v1/adaptive/cache", (c) => c.json(snapshot()));
}

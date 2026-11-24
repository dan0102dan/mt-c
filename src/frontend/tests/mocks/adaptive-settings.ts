import { adaptiveDefaults, type AdaptiveStatus } from "../../src/modules/settings/adaptive/data";

export function adaptiveStatus(): AdaptiveStatus {
  return {
    settings: { ...adaptiveDefaults, ingressInterfaces: [] },
    active: false,
    error: null,
    interfaces: [
      { id: "br-lan", name: "Local network" },
      { id: "br0", name: "Guest network" },
      { id: "wg0", name: "WireGuard" },
      { id: "wan0", name: "Provider" },
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
  };
}

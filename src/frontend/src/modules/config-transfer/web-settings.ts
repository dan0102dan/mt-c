import type { Settings } from "./model";

/** Only settings owned by WebUI editors belong in portable configuration.
 * Daemon-only fields from older exports must not enter an import or download. */
export function webSettings(settings: Settings): Settings {
  const policy = settings.keeneticPolicy;
  if (policy === undefined) return {};
  if (policy === null || typeof policy !== "object" || Array.isArray(policy))
    return { keeneticPolicy: policy };
  const fields = Object.fromEntries(
    ["enabled", "policyId", "mode"]
      .filter((field) => Object.hasOwn(policy, field))
      .map((field) => [field, policy[field]]),
  );
  return { keeneticPolicy: fields };
}

/** Apply portable settings over the device configuration, retaining internal fields. */
export function applyWebSettings(current: Settings, incoming: Settings): Settings {
  const editable = webSettings(incoming);
  const previous = current.keeneticPolicy;
  const next = editable.keeneticPolicy;
  if (
    previous &&
    typeof previous === "object" &&
    !Array.isArray(previous) &&
    next &&
    typeof next === "object" &&
    !Array.isArray(next)
  ) {
    editable.keeneticPolicy = { ...previous, ...next };
  }
  return { ...current, ...editable };
}

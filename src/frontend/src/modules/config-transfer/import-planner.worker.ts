import { ImportPlannerSession, type PlannerRequest, type PlannerResponse } from "./import-planner";

let session: ImportPlannerSession | undefined;
let pending: Extract<PlannerRequest, { type: "plan" }> | undefined;
let timer: ReturnType<typeof setTimeout> | undefined;
const reply = (response: PlannerResponse) => self.postMessage(response);

self.onmessage = (event: MessageEvent<PlannerRequest>) => {
  const request = event.data;
  if (request.type === "init") {
    session = new ImportPlannerSession(request.current, request.imported);
    return;
  }
  if (request.type === "plan") {
    // A quick sequence of checkbox changes needs only its newest preview.
    pending = request;
    if (timer !== undefined) return;
    timer = setTimeout(() => {
      timer = undefined;
      const latest = pending;
      pending = undefined;
      if (!latest) return;
      try {
        if (!session) throw new Error("Import planner is not initialized");
        reply({ type: "preview", version: latest.version, preview: session.preview(latest) });
      } catch (error) {
        reply({
          type: "error",
          version: latest.version,
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }, 0);
    return;
  }
  try {
    if (!session) throw new Error("Import planner is not initialized");
    reply({ type: "apply", version: request.version, plan: session.apply(request.version) });
  } catch (error) {
    reply({
      type: "error",
      version: request.version,
      message: error instanceof Error ? error.message : String(error),
    });
  }
};

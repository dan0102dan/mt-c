import { requireAuthentication, token } from "../../data/auth.svelte";

import { API_BASE, fetcher } from "../../utils/fetcher";
import { subscribeApplicationLogs, type LogConnectionState } from "./connection";
import { LogHistory, parseLogCapture, type LogCapture } from "./history";

/** Application lifetime, not tab lifetime. No file, localStorage or IndexedDB. */
function createLogsStore() {
  let history = new LogHistory();
  let revision = $state(0);
  let capture = $state.raw<LogCapture | null>(null);
  let connection = $state<LogConnectionState>("idle");
  let changing = $state(false);
  let requested = false;
  let authorized = false;
  let credential = "";
  let generation = 0;
  let mutation = 0;
  let stop: (() => void) | undefined;
  let publishTimer: ReturnType<typeof setTimeout> | undefined;

  const publish = () => {
    clearTimeout(publishTimer);
    publishTimer = undefined;
    capture = history.capture;
    revision++;
  };
  const schedule = () => {
    if (!publishTimer) publishTimer = setTimeout(publish, 50);
  };
  const disconnect = () => {
    generation++;
    stop?.();
    stop = undefined;
  };
  const connect = () => {
    disconnect();
    if (!requested || !authorized) return;
    const attempt = generation;
    const requestToken = credential;
    stop = subscribeApplicationLogs({
      url: `${API_BASE}/logs/stream`,
      token: requestToken,
      cursor: () => history.cursor(),
      receiver: () => history.receiver(),
      onChange: schedule,
      onState: (state) => {
        if (attempt !== generation) return;
        connection = state;
        if (state === "reconnecting") publish();
      },
      onUnauthorized: () => {
        if (attempt === generation && (token.current ?? "") === requestToken)
          requireAuthentication();
      },
    });
  };
  return {
    get entries() {
      void revision;
      return history.entries;
    },
    get count() {
      void revision;
      return history.count;
    },
    get revision() {
      return revision;
    },
    get capture() {
      return capture;
    },
    get connection() {
      return connection;
    },
    get changing() {
      return changing;
    },
    authorize(allowed: boolean, nextCredential: string) {
      if (allowed === authorized && nextCredential === credential) return;
      const clear = authorized && (!allowed || nextCredential !== credential);
      disconnect();
      authorized = allowed;
      credential = nextCredential;
      if (clear) {
        mutation++;
        history = new LogHistory();
        changing = false;
        publish();
      }
      if (!allowed) {
        connection = "idle";
        return;
      }
      connect();
    },
    open() {
      if (requested) return;
      requested = true;
      connect();
    },
    clear() {
      history.clear();
      publish();
    },
    async setRecording(enabled: boolean) {
      if (!authorized || changing) return;
      changing = true;
      const identity = credential;
      const operation = ++mutation;
      try {
        const result = await fetcher<unknown>("/logs/capture", {
          method: "PUT",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ enabled }),
          timeoutMs: 10000,
        });
        parseLogCapture(result);
      } finally {
        // A late response belonging to a former login must not unlock or
        // reconnect a newer account's recording operation.
        if (operation === mutation) {
          changing = false;
          // A lost PUT response may have applied; reconcile by reading state.
          if (authorized && identity === credential) connect();
        }
      }
    },
    destroy() {
      mutation++;
      disconnect();
      clearTimeout(publishTimer);
      publishTimer = undefined;
      history = new LogHistory();
      requested = authorized = false;
      credential = "";
      connection = "idle";
      publish();
    },
  };
}
export const applicationLogs = createLogsStore();

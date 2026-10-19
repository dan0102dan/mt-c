import type { GroupsStore } from "../groups/groups.svelte";
import type { SubscriptionsStore } from "../subscriptions/subscriptions.svelte";

export const CONFIG_TRANSFER_CONTEXT = Symbol("config-transfer");

export type PolicyDraftSource = {
  get: () => { enabled: boolean; policy_id: string; mode: "include" | "exclude" } | undefined;
  dirty: () => boolean;
  saving: () => boolean;
  refresh: () => Promise<void>;
};

export type ConfigTransferControls = {
  registerGroups: (store: GroupsStore) => () => void;
  registerSubscriptions: (store: SubscriptionsStore) => () => void;
  registerPolicy: (source: PolicyDraftSource) => () => void;
  exportConfig: () => Promise<void>;
  importConfig: (event: Event) => Promise<void>;
};

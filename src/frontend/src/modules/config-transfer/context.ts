import type { GroupsStore } from "../groups/groups.svelte";
import type { SubscriptionsStore } from "../subscriptions/subscriptions.svelte";

import type { Profile } from "../../types";
import type { TransferPolicy } from "./adapter";

export type ImportSaveSection = "groups" | "subscriptions" | "settings";

export const CONFIG_TRANSFER_CONTEXT = Symbol("config-transfer");

export type PolicyDraftSource = {
  get: () => { enabled: boolean; policy_id: string; mode: "include" | "exclude" } | undefined;
  dirty: () => boolean;
  saving: () => boolean;
  valid: () => boolean;
  stage: (baseline: TransferPolicy, draft: TransferPolicy) => void;
  accept: (baseline: TransferPolicy, draft: TransferPolicy) => void;
  refresh: () => Promise<void>;
};

export type ConfigTransferControls = {
  registerGroups: (store: GroupsStore) => () => void;
  registerSubscriptions: (store: SubscriptionsStore) => () => void;
  registerPolicy: (source: PolicyDraftSource) => () => void;
  connectionChangeBlocked: () => boolean;
  pending: () => boolean;
  busy: () => boolean;
  canSave: (section: ImportSaveSection) => boolean;
  save: (section: ImportSaveSection) => Promise<void>;
  routeProfiles: () => readonly Profile[];
  exportConfig: () => Promise<void>;
  importConfig: (event: Event) => Promise<void>;
};

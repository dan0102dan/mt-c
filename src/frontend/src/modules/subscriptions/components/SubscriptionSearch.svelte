<script lang="ts">
  import { getContext } from "svelte";

  import SearchField from "../../../components/ui/SearchField.svelte";
  import { t } from "../../../data/locale.svelte";
  import { SUBSCRIPTIONS_STORE_CONTEXT, type SubscriptionsStore } from "../subscriptions.svelte";

  const store = getContext<SubscriptionsStore>(SUBSCRIPTIONS_STORE_CONTEXT);
  if (!store) throw new Error("SubscriptionsStore context is missing");
</script>

<div class="subscription-controls-search">
  <SearchField
    expandable
    bind:value={store.searchValue}
    label={t("Search subscriptions and rules...")}
  />
</div>

<style>
  .subscription-controls-search {
    display: flex;
    align-items: center;
    flex: 0 1 auto;
    transition: flex-grow 0.3s ease;
    min-width: 0;
  }
  @media (max-width: 570px) {
    .subscription-controls-search {
      flex: 1 1 auto;
    }
  }
</style>

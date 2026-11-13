<script lang="ts">
  import { getContext } from "svelte";

  import SearchField from "../../../components/ui/SearchField.svelte";
  import { t } from "../../../data/locale.svelte";
  import { GROUPS_STORE_CONTEXT, type GroupsStore } from "../groups.svelte";

  const store = getContext<GroupsStore>(GROUPS_STORE_CONTEXT);
  if (!store) throw new Error("GroupsStore context is missing");
</script>

<div class="group-controls-search">
  <SearchField expandable bind:value={store.searchValue} label={t("Search groups and rules...")} />
</div>

<style>
  .group-controls-search {
    display: flex;
    align-items: center;
    flex: 0 1 auto;
    transition: flex-grow 0.3s ease;
    min-width: 0;
  }
  @media (max-width: 570px) {
    .group-controls-search {
      flex: 1 1 auto;
    }
  }
</style>

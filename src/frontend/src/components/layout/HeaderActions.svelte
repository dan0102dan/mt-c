<script lang="ts">
  import { authState, token } from "../../data/auth.svelte";
  import { t } from "../../data/locale.svelte";
  import Button from "../ui/Button.svelte";
  import Tooltip from "../ui/Tooltip.svelte";

  import { LogOut } from "../ui/icons";
</script>

{#if authState.enabled}
  <div class="logout">
    <Tooltip value={t("Logout")}>
      <Button small aria-label={t("Logout")} onclick={() => token.reset()}>
        <LogOut size={20} aria-hidden="true" />
      </Button>
    </Tooltip>
  </div>
{/if}

<style>
  .logout {
    display: flex;
    align-items: center;
    flex-shrink: 0;
  }
  .logout :global(button) {
    width: 35px;
    height: 35px;
  }
</style>

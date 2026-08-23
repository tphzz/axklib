<script lang="ts">
    interface Props {
        enabled: boolean;
        open: boolean;
        activeWorkspaceId: string | null;
    }

    let { enabled, open = $bindable(), activeWorkspaceId }: Props = $props();
    const workspaceManagerModule = import('./WorkspaceManager.svelte');
</script>

{#if enabled}
    {#await workspaceManagerModule then workspaceManager}
        {@const WorkspaceManager = workspaceManager.default}
        <WorkspaceManager {open} {activeWorkspaceId} onclose={() => (open = false)} />
    {/await}
{/if}

<script lang="ts">
    import { invoke } from '@tauri-apps/api/core';
    import Icon from '../../lib/components/Icon.svelte';
    let error = $state('');
    let busy = $state(false);
    async function open(): Promise<void> {
        if (busy) return;
        busy = true;
        error = '';
        try {
            await invoke('open_diagnostic_logs');
        } catch (reason) {
            error = `Could not open Logs: ${String(reason)}`;
        } finally {
            busy = false;
        }
    }
</script>

{#if error}<span role="alert" title={error}>{error}</span>{/if}
<button type="button" class="logs-button" title="Open application logs" disabled={busy} onclick={open}>
    <Icon name="list" size={12} />Logs...
</button>

<style>
    .logs-button {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        padding: 0 4px;
        height: 18px;
        border: 0;
        background: transparent;
        color: var(--color-text);
        font-size: 10px;
        flex-shrink: 0;
    }
    .logs-button:hover {
        color: var(--color-text-strong);
    }
    span {
        max-width: 300px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        color: var(--color-danger);
    }
</style>

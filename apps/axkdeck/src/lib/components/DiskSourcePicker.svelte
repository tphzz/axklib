<script lang="ts">
    import { onDestroy } from 'svelte';
    import { browserUploadSource, type ClientUploadSource } from '../clientUploadSource';
    import { userFacingMessage } from '../userFacingMessage';
    import Icon from './Icon.svelte';
    let {
        disabled = false,
        onadd,
        onerror,
    }: {
        disabled?: boolean;
        onadd: (files: ClientUploadSource[]) => void;
        onerror: (message: string) => void;
    } = $props();
    const native = '__TAURI_INTERNALS__' in window;
    let picking = $state(false);
    let disposed = false;
    onDestroy(() => {
        disposed = true;
    });
    async function chooseNative() {
        if (disabled || picking) return;
        picking = true;
        try {
            const { selectNativeDiskSources } = await import('../nativeDiskSources');
            if (disposed || disabled) return;
            const files = await selectNativeDiskSources();
            if (files.length && !disabled && !disposed) onadd(files);
        } catch (error) {
            if (!disposed) onerror(userFacingMessage(error));
        } finally {
            picking = false;
        }
    }
</script>

{#if native}
    <button class="secondary-button" disabled={disabled || picking} onclick={chooseNative}>
        <Icon name="file-plus" size={14} />Add disks
    </button>
{:else}
    <label class="secondary-button add-disks" aria-disabled={disabled}>
        <Icon name="file-plus" size={14} />Add disks
        <input
            type="file"
            accept=".img,.ima"
            multiple
            {disabled}
            onchange={(event) => {
                const files = Array.from(event.currentTarget.files ?? []).map(browserUploadSource);
                event.currentTarget.value = '';
                if (files.length) onadd(files);
            }}
        />
    </label>
{/if}

<style>
    .add-disks {
        position: relative;
        overflow: hidden;
    }
    input {
        position: absolute;
        inset: 0;
        opacity: 0;
        cursor: pointer;
        width: 100%;
    }
    .add-disks:focus-within {
        outline: 1px solid var(--state-focus);
        outline-offset: 2px;
    }
</style>

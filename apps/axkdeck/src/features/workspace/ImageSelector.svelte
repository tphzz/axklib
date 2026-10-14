<script lang="ts">
    import { tick } from 'svelte';
    import type { OpenedImage } from '../../lib/transport';
    import { sourceLabels } from './sourceLabels';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    let {
        sources,
        selected,
        disabled,
        deviceLabel,
        onselect,
        onerror,
    }: {
        sources: OpenedImage['sources'];
        selected: string | null;
        disabled: boolean;
        deviceLabel?: string;
        onselect: (id: string) => Promise<void>;
        onerror: (message: string) => void;
    } = $props();
    let pending = $state(false);
    const options = $derived(sourceLabels(sources, deviceLabel));
    async function select(control: HTMLSelectElement) {
        pending = true;
        try {
            await onselect(control.value);
        } catch (error) {
            onerror(userFacingMessage(error));
        } finally {
            await tick();
            control.value = selected ?? '';
            pending = false;
        }
    }
</script>

{#if options.length}
    <select
        class="image-select dialog-field-control"
        aria-label="Image"
        value={selected ?? ''}
        title={options.find((item) => item.id === selected)?.path}
        disabled={disabled || pending}
        onchange={(event) => void select(event.currentTarget)}
    >
        {#each options as option (option.id)}<option value={option.id} title={option.path}>{option.label}</option
            >{/each}
    </select>
{/if}

<style>
    .image-select {
        width: 320px;
        max-width: 100%;
        min-width: 0;
        height: 25px;
        font-size: 11px;
    }
</style>

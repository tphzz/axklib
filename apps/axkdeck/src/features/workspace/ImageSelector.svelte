<script lang="ts">
    import SelectControl from '../../lib/components/SelectControl.svelte';
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
    async function select(id: string) {
        pending = true;
        try {
            await onselect(id);
        } catch (error) {
            onerror(userFacingMessage(error));
        } finally {
            pending = false;
        }
    }
</script>

{#if options.length}
    <div class="image-select">
        <SelectControl
            label="Image"
            value={selected ?? ''}
            options={options.map((option) => ({ value: option.id, label: option.label, title: option.path }))}
            size="header"
            title={options.find((item) => item.id === selected)?.path}
            disabled={disabled || pending}
            onchange={(id) => void select(id)}
        />
    </div>
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

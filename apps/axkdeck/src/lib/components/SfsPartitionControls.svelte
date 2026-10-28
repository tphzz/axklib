<script lang="ts">
    import type { HardDiskCreationProfile } from '../transport';
    let {
        options,
        value,
        disabled = false,
        onchange,
    }: {
        options: HardDiskCreationProfile['partitionOptions'];
        value: number;
        disabled?: boolean;
        onchange: (value: number) => void;
    } = $props();
    const selected = $derived(options.find((option) => option.partitionCount === value));
</script>

<fieldset {disabled}>
    <legend>Partitions</legend>
    <div class="partitions dialog-segmented-control">
        {#each [1, 2, 3, 4, 5, 6, 7, 8] as count}
            {@const allowed = options.some((option) => option.partitionCount === count)}
            <button
                type="button"
                aria-label={`${count} ${count === 1 ? 'partition' : 'partitions'}`}
                aria-pressed={allowed && count === value}
                disabled={!allowed || disabled}
                title={!allowed ? 'This partition count is not supported for the selected capacity.' : undefined}
                onclick={() => onchange(count)}>{count}</button
            >
        {/each}
    </div>
</fieldset>
{#if selected}<p class="summary">
        {value}
        {value === 1 ? 'partition' : 'partitions'} · {(
            selected.partitionSizeBytes /
            1024 ** (selected.partitionSizeBytes >= 1024 ** 3 ? 3 : 2)
        ).toFixed(2)}
        {selected.partitionSizeBytes >= 1024 ** 3 ? 'GiB' : 'MiB'} each
    </p>{/if}

<style>
    fieldset {
        min-width: 0;
        margin: 0;
        padding: 0;
        border: 0;
    }
    legend {
        margin-bottom: 4px;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }
    .partitions {
        grid-template-columns: repeat(8, minmax(0, 1fr));
        grid-auto-flow: row;
    }
    .summary {
        margin: 0;
        color: var(--color-text-muted);
        font-size: var(--dialog-metadata-font-size);
    }
</style>

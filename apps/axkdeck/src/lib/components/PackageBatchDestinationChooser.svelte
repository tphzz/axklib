<script lang="ts">
    import type {
        ImportDestinationMode,
        ImportPartitionOption,
        ImportVolumeOption,
    } from '../../features/import/packageDestinations';
    import type { PackageBatchDestinationStrategy } from '../../features/import/packageBatchTypes';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';

    interface Props {
        strategy: PackageBatchDestinationStrategy;
        mode: ImportDestinationMode;
        partitionIndex: number | null;
        volumeName: string;
        partitions: ImportPartitionOption[];
        volumes: ImportVolumeOption[];
        separateAvailable: boolean;
        disabled: boolean;
        onstrategy: (strategy: PackageBatchDestinationStrategy) => void;
        onmode: (mode: ImportDestinationMode) => void;
        onvolume: (partitionIndex: number | null, volumeName: string) => void;
        onpartition: (partitionIndex: number) => void;
        onname: (name: string) => void;
    }

    let {
        strategy,
        mode,
        partitionIndex,
        volumeName,
        partitions,
        volumes,
        separateAvailable,
        disabled,
        onstrategy,
        onmode,
        onvolume,
        onpartition,
        onname,
    }: Props = $props();
</script>

<section class="batch-destination" aria-label="Batch destination">
    <div class="strategy-heading">
        <strong>Placement</strong>
        <div class="strategy-control" role="group" aria-label="Package placement">
            <button
                type="button"
                class:active={strategy === 'shared'}
                aria-pressed={strategy === 'shared'}
                {disabled}
                onclick={() => onstrategy('shared')}>One volume</button
            >
            <button
                type="button"
                class:active={strategy === 'separate'}
                aria-pressed={strategy === 'separate'}
                disabled={disabled || !separateAvailable}
                title={separateAvailable
                    ? 'Create one volume for each package'
                    : 'Separate volumes require only Volume packages'}
                onclick={() => onstrategy('separate')}>Separate volumes</button
            >
        </div>
    </div>

    {#if strategy === 'shared'}
        <ImportDestinationChooser
            {mode}
            {partitionIndex}
            {volumeName}
            {partitions}
            {volumes}
            {disabled}
            {onmode}
            {onvolume}
            {onpartition}
            {onname}
        />
    {:else}
        <label class="partition-field">
            <span>Partition</span>
            <select
                class="dialog-field-control"
                value={partitionIndex ?? ''}
                disabled={disabled || partitions.length === 0}
                onchange={(event) => onpartition(Number(event.currentTarget.value))}
            >
                {#each partitions as option (option.partitionIndex)}
                    <option value={option.partitionIndex}>{option.name}</option>
                {/each}
            </select>
        </label>
        <small>Each selected package creates a volume using its placement hint and editable name below.</small>
    {/if}
</section>

<style>
    .batch-destination {
        display: grid;
        gap: 10px;
        margin-bottom: 10px;
        padding: 10px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: rgb(255 255 255 / 2%);
    }

    .strategy-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
    }

    .strategy-control {
        display: flex;
    }

    .strategy-control button {
        min-height: 32px;
        padding: 0 12px;
        border: 1px solid var(--color-border);
        color: var(--color-text-muted);
        background: var(--color-bg-deep);
    }

    .strategy-control button:first-child {
        border-radius: 5px 0 0 5px;
    }

    .strategy-control button:last-child {
        margin-left: -1px;
        border-radius: 0 5px 5px 0;
    }

    .strategy-control button.active {
        color: #fff;
        background: var(--color-accent-strong);
    }

    .partition-field {
        display: grid;
        grid-template-columns: minmax(100px, 0.35fr) minmax(180px, 1fr);
        align-items: center;
        gap: 10px;
    }

    .batch-destination > small {
        color: var(--color-text-muted);
    }

    @media (max-width: 620px) {
        .strategy-heading {
            align-items: flex-start;
            flex-direction: column;
        }

        .partition-field {
            grid-template-columns: 1fr;
        }
    }
</style>

<script lang="ts">
    import type { ImportCapacityReview } from '../../features/import/importCapacityReview.svelte';
    import type {
        ImportDestinationMode,
        ImportPartitionOption,
        ImportVolumeOption,
    } from '../../features/import/packageDestinations';
    import type { AudioImportGrouping, AudioImportOptions } from '../transport';
    import AudioImportTargetSettings from './AudioImportTargetSettings.svelte';
    import ImportCapacityControl from './ImportCapacityControl.svelte';
    import ImportDestinationChooser from './ImportDestinationChooser.svelte';

    let {
        mode = $bindable(),
        sampleBankName = $bindable(),
        sampleFormat = $bindable(),
        error,
        disabled,
        hasFiles,
        capacity,
        destinationMode,
        partitionIndex,
        volumeName,
        partitions,
        volumes,
        onmode,
        onvolume,
        onpartition,
        onname,
    }: {
        mode: AudioImportGrouping['kind'];
        sampleBankName: string;
        sampleFormat: AudioImportOptions['sampleFormat'];
        error: string;
        disabled: boolean;
        hasFiles: boolean;
        capacity: ImportCapacityReview;
        destinationMode: ImportDestinationMode;
        partitionIndex: number | null;
        volumeName: string;
        partitions: ImportPartitionOption[];
        volumes: ImportVolumeOption[];
        onmode: (mode: ImportDestinationMode) => void;
        onvolume: (partitionIndex: number | null, volumeName: string) => void;
        onpartition: (partitionIndex: number) => void;
        onname: (name: string) => void;
    } = $props();
</script>

<div class="audio-import-controls">
    <ImportDestinationChooser
        compact
        mode={destinationMode}
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
    <AudioImportTargetSettings
        compact
        showImportSettings={hasFiles}
        bind:mode
        bind:sampleBankName
        bind:sampleFormat
        {error}
        {disabled}
    >
        <ImportCapacityControl review={capacity} {disabled} compact showSummary={false} />
    </AudioImportTargetSettings>
</div>

<style>
    .audio-import-controls {
        --audio-import-mode-width: 280px;
        display: flex;
        flex: none;
        flex-wrap: wrap;
        align-items: flex-start;
        gap: 4px;
        padding: 6px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
    }
    @container audio-import (min-width: 1252px) {
        .audio-import-controls {
            --audio-import-mode-width: 220px;
        }
    }
</style>

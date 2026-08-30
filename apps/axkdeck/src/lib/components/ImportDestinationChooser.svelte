<script lang="ts">
    import type {
        ImportDestinationMode,
        ImportPartitionOption,
        ImportVolumeOption,
    } from '../../features/import/packageDestinations';
    import { dismissAutocompleteFromOutsidePointer } from '../autocomplete';
    import Icon from './Icon.svelte';

    interface Props {
        mode: ImportDestinationMode;
        partitionIndex: number | null;
        volumeName: string;
        partitions: ImportPartitionOption[];
        volumes: ImportVolumeOption[];
        disabled: boolean;
        onmode: (mode: ImportDestinationMode) => void;
        onvolume: (partitionIndex: number | null, volumeName: string) => void;
        onpartition: (partitionIndex: number) => void;
        onname: (name: string) => void;
    }

    let {
        mode,
        partitionIndex,
        volumeName,
        partitions,
        volumes,
        disabled,
        onmode,
        onvolume,
        onpartition,
        onname,
    }: Props = $props();

    let query = $state('');
    let activeIndex = $state(-1);
    let listOpen = $state(false);
    let filtering = $state(false);
    let synchronizedSelection = $state('');
    let synchronizedPartition = $state<number | null>(null);
    let synchronizedMode = $state<ImportDestinationMode>('existing');
    let volumeInput = $state<HTMLInputElement>();
    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const partitionVolumes = $derived(volumes.filter((option) => option.partitionIndex === partitionIndex));
    const filteredVolumes = $derived(
        filtering
            ? partitionVolumes.filter((option) => option.volumeName.toLocaleLowerCase().includes(normalizedQuery))
            : partitionVolumes,
    );
    const selectedVolume = $derived(
        volumes.find((option) => option.partitionIndex === partitionIndex && option.volumeName === volumeName),
    );

    $effect(() => {
        const nextSelection = selectedVolume ? volumeKey(selectedVolume) : '';
        const partitionChanged = partitionIndex !== synchronizedPartition;
        const modeChanged = mode !== synchronizedMode;
        if (selectedVolume && (nextSelection !== synchronizedSelection || partitionChanged || modeChanged)) {
            query = selectedVolume.volumeName;
            filtering = false;
            activeIndex = Math.max(0, partitionVolumes.indexOf(selectedVolume));
        } else if (partitionChanged || modeChanged) {
            query = '';
            filtering = false;
            activeIndex = -1;
            listOpen = false;
        }
        synchronizedSelection = nextSelection;
        synchronizedPartition = partitionIndex;
        synchronizedMode = mode;
    });

    function volumeKey(option: ImportVolumeOption): string {
        return `${option.partitionIndex}:${option.volumeName}`;
    }

    function optionId(option: ImportVolumeOption): string {
        return `import-volume-option-${option.partitionIndex}-${volumes.indexOf(option)}`;
    }

    function selectVolume(option: ImportVolumeOption): void {
        if (disabled) return;
        query = option.volumeName;
        filtering = false;
        synchronizedSelection = volumeKey(option);
        closeList();
        onvolume(option.partitionIndex, option.volumeName);
    }

    function updateQuery(value: string): void {
        query = value;
        filtering = true;
        activeIndex = -1;
        listOpen = true;
        onvolume(partitionIndex, '');
    }

    function openList(): void {
        if (disabled || partitionVolumes.length === 0) return;
        if (selectedVolume) filtering = false;
        listOpen = true;
        activeIndex = selectedVolume ? filteredVolumes.indexOf(selectedVolume) : -1;
    }

    function clearVolume(): void {
        if (disabled) return;
        query = '';
        filtering = false;
        activeIndex = -1;
        listOpen = partitionVolumes.length > 0;
        synchronizedSelection = selectedVolume ? volumeKey(selectedVolume) : '';
        onvolume(partitionIndex, '');
        queueMicrotask(() => volumeInput?.focus());
    }

    function closeList(): void {
        listOpen = false;
        activeIndex = -1;
    }

    function changePartition(value: string): void {
        if (disabled || value === '') return;
        query = '';
        filtering = false;
        activeIndex = -1;
        listOpen = false;
        onpartition(Number(value));
    }

    function handleKey(event: KeyboardEvent): void {
        if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
            event.preventDefault();
            listOpen = true;
            const direction = event.key === 'ArrowDown' ? 1 : -1;
            const startingIndex = activeIndex < 0 ? (direction > 0 ? -1 : filteredVolumes.length) : activeIndex;
            activeIndex = Math.max(0, Math.min(filteredVolumes.length - 1, startingIndex + direction));
        } else if (event.key === 'Home' || event.key === 'End') {
            event.preventDefault();
            listOpen = true;
            activeIndex = event.key === 'Home' ? 0 : Math.max(0, filteredVolumes.length - 1);
        } else if (event.key === 'Enter' && listOpen && filteredVolumes[activeIndex]) {
            event.preventDefault();
            selectVolume(filteredVolumes[activeIndex]);
        } else if (event.key === 'Escape' && listOpen) {
            event.preventDefault();
            event.stopPropagation();
            closeList();
        }
    }
</script>

<section class="import-destination" aria-label="Import destination">
    <strong class="destination-label">Destination volume</strong>
    <div class="destination-mode dialog-segmented-control" role="group" aria-label="Destination volume type">
        <button
            type="button"
            aria-pressed={mode === 'existing'}
            disabled={disabled || volumes.length === 0}
            onclick={() => onmode('existing')}>Existing</button
        >
        <button type="button" aria-pressed={mode === 'create'} {disabled} onclick={() => onmode('create')}>New</button>
    </div>

    <select
        class="destination-partition dialog-field-control"
        aria-label="Destination partition"
        value={partitionIndex ?? ''}
        disabled={disabled || partitions.length === 0}
        onchange={(event) => changePartition(event.currentTarget.value)}
    >
        {#if partitionIndex === null}
            <option value="" disabled>Select partition</option>
        {/if}
        {#each partitions as option (option.partitionIndex)}
            <option value={option.partitionIndex}>{option.name}</option>
        {/each}
    </select>

    {#if mode === 'existing'}
        <div
            class="volume-combobox dialog-autocomplete-control"
            use:dismissAutocompleteFromOutsidePointer={{ expanded: listOpen, ondismiss: closeList }}
        >
            <input
                bind:this={volumeInput}
                id="import-volume-search"
                class="dialog-field-control"
                type="text"
                role="combobox"
                aria-label="Destination volume"
                aria-autocomplete="list"
                aria-expanded={listOpen}
                aria-controls="import-volume-options"
                aria-activedescendant={listOpen && filteredVolumes[activeIndex]
                    ? optionId(filteredVolumes[activeIndex])
                    : undefined}
                placeholder={partitionVolumes.length === 0 ? 'No volumes in this partition' : 'Select a volume'}
                value={query}
                disabled={disabled || partitionIndex === null || partitionVolumes.length === 0}
                autocomplete="off"
                oninput={(event) => updateQuery(event.currentTarget.value)}
                onclick={openList}
                onkeydown={handleKey}
            />
            {#if query.length > 0 || selectedVolume}
                <button
                    class="dialog-autocomplete-clear"
                    type="button"
                    aria-label="Clear volume"
                    title="Clear volume"
                    {disabled}
                    onclick={clearVolume}><Icon name="close" size={14} /></button
                >
            {/if}
            {#if listOpen}
                <div
                    id="import-volume-options"
                    class="volume-options dialog-autocomplete-list"
                    role="listbox"
                    aria-label="Volumes"
                >
                    {#each filteredVolumes as option, index (volumeKey(option))}
                        <button
                            id={optionId(option)}
                            type="button"
                            role="option"
                            class="dialog-autocomplete-option"
                            aria-selected={selectedVolume === option}
                            class:active={index === activeIndex}
                            onpointermove={() => (activeIndex = index)}
                            onclick={() => selectVolume(option)}
                        >
                            {option.volumeName}
                        </button>
                    {:else}
                        <p class="dialog-autocomplete-empty">No matching volumes</p>
                    {/each}
                </div>
            {/if}
        </div>
    {:else}
        <input
            class="new-volume-name dialog-field-control"
            type="text"
            aria-label="New volume name"
            minlength="1"
            maxlength="16"
            value={volumeName}
            placeholder="Enter a volume name"
            {disabled}
            autocomplete="off"
            oninput={(event) => onname(event.currentTarget.value)}
        />
    {/if}
</section>

<style>
    .import-destination {
        display: grid;
        grid-template-columns: max-content max-content minmax(140px, 0.35fr) minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        padding: 10px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: rgb(255 255 255 / 2%);
    }

    .destination-label {
        color: var(--color-text-strong);
        font-size: var(--dialog-section-font-size);
        white-space: nowrap;
    }

    .destination-mode {
        grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    .destination-partition,
    .new-volume-name {
        min-width: 0;
        width: 100%;
    }

    .volume-options {
        position: absolute;
        z-index: 4;
        top: calc(100% + 4px);
        right: 0;
        left: 0;
    }

    .volume-options p {
        margin: 0;
        padding: 9px;
    }

    @media (max-width: 760px) {
        .import-destination {
            grid-template-columns: minmax(0, 1fr) max-content;
        }

        .destination-partition,
        .volume-combobox,
        .new-volume-name {
            grid-column: 1 / -1;
        }
    }
</style>

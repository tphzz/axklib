<script lang="ts">
    import type {
        ImportDestinationMode,
        ImportPartitionOption,
        ImportVolumeOption,
    } from '../../features/import/packageDestinations';
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
    let volumeInput = $state<HTMLInputElement>();
    const normalizedQuery = $derived(query.trim().toLocaleLowerCase());
    const filteredVolumes = $derived(
        filtering ? volumes.filter((option) => option.label.toLocaleLowerCase().includes(normalizedQuery)) : volumes,
    );
    const selectedVolume = $derived(
        volumes.find((option) => option.partitionIndex === partitionIndex && option.volumeName === volumeName),
    );

    $effect(() => {
        const nextSelection = selectedVolume ? volumeKey(selectedVolume) : '';
        if (nextSelection && nextSelection !== synchronizedSelection && selectedVolume) {
            query = selectedVolume.label;
            filtering = false;
            activeIndex = Math.max(0, volumes.indexOf(selectedVolume));
        }
        synchronizedSelection = nextSelection;
    });

    function volumeKey(option: ImportVolumeOption): string {
        return `${option.partitionIndex}:${option.volumeName}`;
    }

    function optionId(option: ImportVolumeOption): string {
        return `import-volume-option-${option.partitionIndex}-${volumes.indexOf(option)}`;
    }

    function selectVolume(option: ImportVolumeOption): void {
        if (disabled) return;
        query = option.label;
        filtering = false;
        synchronizedSelection = volumeKey(option);
        listOpen = false;
        onvolume(option.partitionIndex, option.volumeName);
    }

    function updateQuery(value: string): void {
        query = value;
        filtering = true;
        activeIndex = -1;
        listOpen = true;
        onvolume(null, '');
    }

    function openList(): void {
        if (disabled || volumes.length === 0) return;
        if (selectedVolume) filtering = false;
        listOpen = true;
        activeIndex = selectedVolume ? filteredVolumes.indexOf(selectedVolume) : -1;
    }

    function clearVolume(): void {
        if (disabled) return;
        query = '';
        filtering = false;
        activeIndex = -1;
        listOpen = volumes.length > 0;
        synchronizedSelection = selectedVolume ? volumeKey(selectedVolume) : '';
        onvolume(null, '');
        queueMicrotask(() => volumeInput?.focus());
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
            listOpen = false;
        }
    }
</script>

<section class="import-destination" aria-label="Import destination">
    <div class="destination-heading">
        <strong>Destination</strong>
        <div class="destination-mode" role="group" aria-label="Destination type">
            <button
                type="button"
                class:active={mode === 'existing'}
                aria-pressed={mode === 'existing'}
                disabled={disabled || volumes.length === 0}
                onclick={() => onmode('existing')}>Existing volume</button
            >
            <button
                type="button"
                class:active={mode === 'create'}
                aria-pressed={mode === 'create'}
                {disabled}
                onclick={() => onmode('create')}>New volume</button
            >
        </div>
    </div>

    {#if mode === 'existing'}
        <label class="target-field" for="import-volume-search">Volume</label>
        <div class="volume-combobox">
            <input
                bind:this={volumeInput}
                id="import-volume-search"
                class="dialog-field-control"
                type="text"
                role="combobox"
                aria-autocomplete="list"
                aria-expanded={listOpen}
                aria-controls="import-volume-options"
                aria-activedescendant={listOpen && filteredVolumes[activeIndex]
                    ? optionId(filteredVolumes[activeIndex])
                    : undefined}
                placeholder="Select a volume"
                value={query}
                disabled={disabled || volumes.length === 0}
                autocomplete="off"
                oninput={(event) => updateQuery(event.currentTarget.value)}
                onfocus={openList}
                onclick={openList}
                onkeydown={handleKey}
            />
            {#if query.length > 0 || selectedVolume}
                <button
                    class="volume-clear"
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
                            {option.label}
                        </button>
                    {:else}
                        <p class="dialog-autocomplete-empty">No matching volumes</p>
                    {/each}
                </div>
            {/if}
        </div>
    {:else}
        <div class="new-volume-fields">
            <label class="target-field">
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
            <label class="target-field">
                <span>Volume name</span>
                <input
                    class="dialog-field-control"
                    type="text"
                    minlength="1"
                    maxlength="16"
                    value={volumeName}
                    placeholder="Enter a volume name"
                    {disabled}
                    autocomplete="off"
                    oninput={(event) => onname(event.currentTarget.value)}
                />
            </label>
        </div>
    {/if}
</section>

<style>
    .import-destination {
        display: grid;
        gap: 9px;
        margin-bottom: 12px;
        padding: 10px;
        border: 1px solid var(--color-border);
        border-radius: 5px;
        background: rgb(255 255 255 / 2%);
    }

    .destination-heading {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 12px;
    }

    .destination-heading > strong {
        color: var(--color-text-strong);
        font-size: 11px;
    }

    .destination-mode {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        overflow: hidden;
        border: 1px solid var(--color-border);
        border-radius: 4px;
        background: var(--color-bg-deep);
    }

    .destination-mode button {
        min-height: 27px;
        padding: 0 10px;
        color: var(--color-text-muted);
        border: 0;
        border-right: 1px solid var(--color-border);
        background: transparent;
        cursor: pointer;
        font-size: 10px;
    }

    .destination-mode button:last-child {
        border-right: 0;
    }

    .destination-mode button.active {
        color: #fff;
        background: var(--color-accent-strong);
    }

    .destination-mode button:disabled {
        cursor: default;
        opacity: 0.45;
    }

    .volume-combobox {
        position: relative;
    }

    .volume-combobox > input {
        width: 100%;
        padding-right: 30px;
    }

    .volume-clear {
        position: absolute;
        z-index: 5;
        top: 1px;
        right: 1px;
        display: grid;
        width: 25px;
        height: 24px;
        padding: 0;
        place-items: center;
        color: var(--color-text-muted);
        border: 0;
        background: var(--color-bg-deep);
        cursor: pointer;
        font-size: 15px;
    }

    .volume-clear:hover:not(:disabled) {
        color: var(--color-text-strong);
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

    .new-volume-fields {
        display: grid;
        grid-template-columns: minmax(140px, 0.7fr) minmax(180px, 1fr);
        gap: 10px;
    }

    .target-field {
        display: grid;
        min-width: 0;
        gap: 4px;
        color: var(--color-text-muted);
        font-size: var(--dialog-label-font-size);
    }

    .new-volume-fields :global(.dialog-field-control) {
        min-width: 0;
        width: 100%;
    }

    @media (max-width: 640px) {
        .destination-heading,
        .new-volume-fields {
            grid-template-columns: 1fr;
        }

        .destination-heading {
            display: grid;
        }
    }
</style>

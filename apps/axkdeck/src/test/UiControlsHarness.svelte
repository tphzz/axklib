<script lang="ts">
    import ImageSelector from '../features/workspace/ImageSelector.svelte';
    import EditorChoice from '../features/object-editor/EditorChoice.svelte';
    import EditorAutocomplete from '../features/object-editor/EditorAutocomplete.svelte';
    import ImportDestinationChooser from '../lib/components/ImportDestinationChooser.svelte';
    import AudioImportTargetSettings from '../lib/components/AudioImportTargetSettings.svelte';
    import VolumeActionDialog from '../lib/components/VolumeActionDialog.svelte';
    import SelectControl from '../lib/components/SelectControl.svelte';
    import LogViewerHarness from './LogViewerHarness.svelte';
    import { nativeWorkspaceFixture } from './nativeWorkspaceFixture';
    import type { DiskTreeItem } from '../lib/types';
    import type { AudioImportGrouping } from '../lib/audioImportOptions';
    import type { KnownSampleFormat } from '../lib/aSeriesPreferences.svelte';
    import { modal } from '../lib/modal';
    import '../features/object-editor/editor.css';
    const initial = nativeWorkspaceFixture().sources[0];
    const sources = [
        initial,
        {
            ...initial,
            id: 'source-2',
            source: {
                kind: 'FILE' as const,
                file: { rootId: 'root', relativePath: 'A very long device-backed image name for truncation.hds' },
            },
        },
    ];
    const options = [
        { value: 0, label: 'StereoOut' },
        { value: 1, label: 'Ef1' },
        { value: 2, label: 'AssignOut L&R', extended: true },
        { value: 3, label: 'Unavailable effect', disabled: true, reason: 'Not supported for this format.' },
    ];
    let selected = $state('source-1');
    let value = $state(0);
    let changes = $state(0);
    let disabled = $state(false);
    let rename = $state(false);
    let picker = $state(false);
    let nested = $state(false);
    let partition = $state(0);
    let volume = $state('Strings');
    let mode = $state<'existing' | 'create'>('existing');
    let importMode = $state<AudioImportGrouping['kind']>('SAMPLES');
    let bankName = $state('New Bank');
    let format = $state<KnownSampleFormat>('A4000_A5000_224');
    const item: DiskTreeItem = { id: 'volume', name: 'New Volume', kind: 'volume', childCount: 0, partitionIndex: 0 };
    function change(next: number) {
        value = next;
        changes++;
    }
</script>

<main class="ui-controls-fixture" style:zoom={Number(new URLSearchParams(location.search).get('localScale') ?? 1)}>
    <header class="app-header">
        <strong>axkdeck</strong><ImageSelector
            {sources}
            {selected}
            disabled={false}
            onselect={async (id) => {
                selected = id;
            }}
            onerror={() => {}}
        />
    </header>
    <section class="device-editor">
        <label>Output<EditorChoice label="Output" {value} {options} segmented={false} onchange={change} /></label>
        <label>Controller<EditorAutocomplete label="Controller" {value} {options} onchange={change} /></label>
    </section>
    <ImportDestinationChooser
        {mode}
        partitionIndex={partition}
        volumeName={volume}
        partitions={[
            { partitionIndex: 0, name: 'PARTITION 1' },
            { partitionIndex: 1, name: 'PARTITION 2' },
        ]}
        volumes={[
            { partitionIndex: 0, name: 'PARTITION 1', volumeName: 'Strings', label: 'Strings' },
            { partitionIndex: 0, name: 'PARTITION 1', volumeName: 'Piano', label: 'Piano' },
        ]}
        disabled={false}
        onmode={(next) => (mode = next)}
        onpartition={(next) => (partition = next)}
        onvolume={(next, name) => {
            partition = next ?? 0;
            volume = name;
        }}
        onname={(name) => (volume = name)}
    />
    <AudioImportTargetSettings
        bind:mode={importMode}
        bind:sampleBankName={bankName}
        bind:sampleFormat={format}
        error=""
        disabled={false}
    />
    <section class="fixture-tools">
        <SelectControl
            label="Empty device list"
            value={undefined}
            options={[]}
            emptyLabel="No devices available"
            onchange={() => changes++}
        />
        <fieldset {disabled}><SelectControl label="Fieldset choice" {value} {options} onchange={change} /></fieldset>
        <button type="button" class="secondary-button" onclick={() => (disabled = !disabled)}>Toggle fieldset</button>
        <button type="button" class="secondary-button" onclick={() => (rename = true)}>Rename fixture volume</button>
        <button type="button" class="secondary-button" onclick={() => (picker = true)}>Open control dialog</button>
        <output aria-label="Selection changes">{changes}</output>
    </section>
    <div class="log-slot"><LogViewerHarness /></div>
    {#if rename}
        <VolumeActionDialog
            action="rename-volume"
            items={[item]}
            busy={false}
            phase="idle"
            error=""
            deletionInspection={null}
            oncancel={() => (rename = false)}
            onsubmit={() => changes++}
        />
    {/if}
    {#if picker}
        <div class="dialog-backdrop">
            <div
                class="dialog-shell"
                role="dialog"
                aria-modal="true"
                aria-label="Control test dialog"
                use:modal={{ onescape: () => (picker = false) }}
            >
                <header class="dialog-header"><h2>Control test</h2></header>
                <div class="volume-action-content">
                    <SelectControl label="Modal choice" {value} {options} onchange={change} />
                    <button class="secondary-button" type="button" onclick={() => (nested = true)}
                        >Open nested dialog</button
                    >
                </div>
                <footer class="dialog-footer">
                    <button class="secondary-button" type="button" onclick={() => (picker = false)}
                        >Close control dialog</button
                    >
                </footer>
            </div>
        </div>
    {/if}
    {#if nested}
        <div class="dialog-backdrop">
            <div
                class="dialog-shell"
                role="dialog"
                aria-modal="true"
                aria-label="Nested control dialog"
                use:modal={{ onescape: () => (nested = false) }}
            >
                <header class="dialog-header"><h2>Nested control</h2></header>
                <button class="secondary-button" type="button" onclick={() => (nested = false)}
                    >Close nested dialog</button
                >
            </div>
        </div>
    {/if}
</main>

<style>
    main {
        display: grid;
        grid-template-columns: minmax(0, 1fr);
        align-content: start;
        gap: 16px;
        padding: 12px;
        height: 100%;
        overflow: auto;
    }
    header {
        display: flex;
        gap: 16px;
        padding: 8px;
        min-height: 44px;
    }
    section.device-editor {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 12px;
    }
    label {
        display: grid;
        gap: 4px;
        min-width: 0;
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .fixture-tools {
        display: grid;
        grid-template-columns: repeat(2, minmax(0, 1fr));
        gap: 8px;
    }
    fieldset {
        min-width: 0;
        padding: 0;
        border: 0;
        margin: 0;
    }
    .log-slot {
        height: 200px;
        min-height: 200px;
        overflow: hidden;
    }
    .log-slot :global(.log-viewer) {
        height: 200px;
    }
</style>

<script lang="ts">
    import { onMount } from 'svelte';
    import FilesInspector from '../features/files/FilesInspector.svelte';
    import InspectorModeFooter from '../lib/components/InspectorModeFooter.svelte';
    import ObjectInspector from '../lib/components/ObjectInspector.svelte';
    import type { FilesystemEntry } from '../lib/filesystem';
    import { InspectorPanels, provideInspectorPanels } from '../lib/inspectorPanels.svelte';
    import type { InspectorSelection, SampleWaveformPreview } from '../lib/types';
    import { inspectorRelationshipFixture } from './inspectorRelationshipFixture';
    import { sampleFormatFixture } from './sampleFormatFixture';

    interface Configuration {
        kind: string;
        width: number;
        scale: number;
        shown: boolean;
        version: number;
        playhead: number;
    }
    let config = $state<Configuration>({ kind: 'sample', width: 320, scale: 1, shown: true, version: 0, playhead: 0 });
    let navigation = $state('');
    const panels = provideInspectorPanels(new InspectorPanels());
    const scopes = ['program', 'sample-bank', 'sample', 'wave-data', 'sequence', 'files'];
    const sections = ['preview', 'properties', 'relationships', 'stored-format', 'storage'];

    function populatedPreview(preview: SampleWaveformPreview): SampleWaveformPreview {
        return {
            ...preview,
            preview: {
                objectId: preview.item.objectId,
                lanes: preview.waveData.map(({ role, waveData }) => ({
                    role: role === 'left' ? 'LEFT' : 'RIGHT',
                    sourceObjectId: waveData.objectKey,
                    sampleRate: 44100,
                    storedFrameCount: 44100,
                    playbackStartFrame: 0,
                    playbackLengthFrames: 44100,
                    loopStartFrame: 11025,
                    loopLengthFrames: 22050,
                    bins: waveData.waveform,
                })),
            },
        };
    }

    const selection = $derived.by<InspectorSelection>(() => {
        const value = inspectorRelationshipFixture(config.kind)!;
        if (value.kind === 'sample') {
            if (config.kind === 'sequence') {
                const object = { ...value.item.object, key: 'sequence', objectType: 'SEQU', name: 'Test sequence' };
                return {
                    kind: 'sequence',
                    sequence: { id: object.key, objectId: object.key, name: object.name, object },
                };
            }
            const item = {
                ...value.item,
                objectId: `sample-${config.version}`,
                name: `Sample ${config.version} with a deliberately long name`,
                object: { ...value.item.object, sampleFormat: sampleFormatFixture('A3000_188').sampleFormat },
            };
            return { ...value, item, preview: populatedPreview({ ...value.preview, item }) };
        }
        if (value.kind === 'sample-bank')
            return { ...value, memberPreviews: value.memberPreviews.map(populatedPreview) };
        return value;
    });
    const playingObjectId = $derived(
        selection?.kind === 'sample'
            ? selection.item.objectId
            : selection?.kind === 'wave-data'
              ? selection.waveData.objectKey
              : null,
    );
    const entry = $derived<FilesystemEntry>({
        id: `entry-${config.version}`,
        parentId: 'root',
        rootId: 'root',
        ancestorIds: ['root'],
        name: `Sample ${config.version} with a deliberately long name`,
        path: '/DEMO/A deliberately long path/sample-file',
        kind: 'file',
        sizeBytes: 356,
        childCount: 0,
        objectId: 'sample',
        contentScopeId: null,
        interpretation: '',
        storage: `Record ${config.version}`,
        issue: 'Constructed fixture issue stays visible outside collapsed sections.',
        filesystemMetadata: false,
        rawAttributes: 'SFS 0x94000000',
        attributes: [],
    });

    onMount(() => {
        const host = window as typeof window & {
            inspectorPanelsFixture?: { configure: (values: Partial<Configuration>) => void; reset: () => void };
        };
        host.inspectorPanelsFixture = {
            configure(values) {
                config = { ...config, ...values };
            },
            reset() {
                const defaults = new InspectorPanels();
                for (const scope of scopes)
                    for (const section of sections)
                        panels.setExpanded(scope, section, defaults.expanded(scope, section));
                navigation = '';
            },
        };
        return () => delete host.inspectorPanelsFixture;
    });
</script>

<div
    class="inspector-panels-fixture"
    style:width={`${config.width}px`}
    style:zoom={config.scale}
    data-navigation={navigation}
>
    {#if config.shown}
        {#if config.kind === 'files'}
            <FilesInspector {entry} canShowDevice onshowdevice={(id) => (navigation = `device:${id}`)} />
        {:else}
            <ObjectInspector
                {selection}
                {playingObjectId}
                playheadFrame={config.playhead}
                onrelationshipnavigate={(id, focus) => (navigation = `${id}:${focus}`)}
            />
            <InspectorModeFooter mode="files" onclick={() => (navigation = 'files')} />
        {/if}
    {/if}
</div>

<style>
    .inspector-panels-fixture {
        display: flex;
        flex-direction: column;
        height: 620px;
        margin: 12px;
        background: var(--color-panel);
    }
    .inspector-panels-fixture :global(.inspector) {
        flex: 1;
        min-height: 0;
    }
</style>

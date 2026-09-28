<script lang="ts">
    import ObjectEditor from '../lib/components/ObjectEditor.svelte';
    import type { InspectorSelection, ProgramSampleSelectRow } from '../lib/types';
    import type { ObjectDetail } from '../lib/transport';
    import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import { sampleFields } from '../features/devices/a-series/sample/fields';
    import DeviceEditorHostHarness from './DeviceEditorHostHarness.svelte';
    import { sampleConversionFixture, sampleFormatFixture } from './sampleFormatFixture';
    import { bankEditorUnits } from './bankEditorFixture';

    const workspace = new URLSearchParams(location.search).has('workspace');
    let later = $state(false);
    let comparison = $state(false);
    let populated = $state(false);
    const sample = $derived(later ? 'sample-later' : 'sample');
    const bank = $derived(later ? 'bank-later' : 'bank');
    const assignment: ProgramSampleSelectRow = {
        id: 'assignment',
        targetType: 'SBNK',
        targetName: 'Assigned Sample',
        navigable: true,
        assigned: true,
        receiveChannelDisplays: ['=Smp'],
        sourceLoad: false,
        relationships: [],
    };
    const program = $derived({
        kind: 'program',
        item: { objectId: 'program', name: 'Program' },
        sampleSelect: { assigned: populated ? [assignment] : [], all: populated ? [assignment] : [] },
    } as unknown as InspectorSelection);

    function detail(id: string): ObjectDetail {
        const isBank = id.startsWith('bank');
        const native = !id.endsWith('later');
        const format = native ? 'A3000_188' : 'A4000_A5000_224';
        const parameters: Record<string, unknown> = {};
        for (const field of sampleFields) {
            if (field.key.startsWith('playback.')) continue;
            const keys = field.key.split('.');
            let group = parameters;
            for (const key of keys.slice(0, -1)) group = (group[key] ??= {}) as Record<string, unknown>;
            group[keys.at(-1)!] = field.boolean ? false : (field.options?.[0]?.value ?? Math.max(0, field.min));
        }
        return {
            image: { revision: 1 },
            object: { id, key: id, name: native ? 'A' : 'A much longer object name', type: isBank ? 'SBAC' : 'SBNK' },
            formatConversion: sampleConversionFixture(format),
            editing: {
                profile: isBank ? 'a-series/sample-bank' : 'a-series/sample',
                ...(isBank ? { bankOverrides: { units: bankEditorUnits(native, new Set()), members: [] } } : {}),
                editable: true,
                reason: '',
                payloadSha256: 'a'.repeat(64),
                parameters: { ...parameters, level: 100, loop_mode: 4, loop_start_frame: 0, loop_length_frames: 16384 },
                playbackWindow: { start_frame: 0, length_frames: 16384 },
                maximumFrames: 16384,
                canEditPlayback: true,
                eqCoefficients: [-15904, 7738, 8192, 15904, -7738],
                blockedParameters: [],
                blockedParameterReasons: {},
                ...sampleFormatFixture(format),
                unavailableParameters: {},
                partitionIndex: 0,
                volumeName: 'Volume',
                sources: [],
            },
        } as unknown as ObjectDetail;
    }
    const workflow = new ObjectEditorWorkflow({
        transport: {
            objectDetail: async (_: number, id: string) => detail(id),
            startObjectParameterEdit: async () => {
                throw new Error('Read-only fixture');
            },
            waitForJob: async () => {
                throw new Error('Read-only fixture');
            },
        },
        refresh: async () => {},
        stopPlayback: () => {},
        status: () => {},
    });
    function phase(value: 'editable' | 'saving' | 'refresh-failed' | 'unconfirmed') {
        for (const id of [sample, bank]) {
            const document = workflow.find(1, id);
            if (!document) continue;
            document.phase = value;
            document.jobId = value === 'unconfirmed' ? 1 : null;
            if (value === 'editable') document.draft.set('level', 99);
        }
    }
</script>

<nav aria-label="Fixture controls">
    <button onclick={() => (populated = !populated)}>Toggle assignments</button>
    <button onclick={() => (later = !later)}>Change objects</button>
    <button onclick={() => phase('editable')}>Dirty</button>
    <button onclick={() => phase('saving')}>Saving</button>
    <button onclick={() => phase('refresh-failed')}>Refresh recovery</button>
    <button onclick={() => phase('unconfirmed')}>Status recovery</button>
    <button
        onclick={() => {
            comparison = !comparison;
            workflow.comparison.entries = comparison
                ? [
                      { id: sample, name: 'A', values: { level: 100 } },
                      { id: bank, name: 'B', values: { level: 99 } },
                  ]
                : [];
        }}>Comparison</button
    >
</nav>
<div class:workspace class="layout">
    {#if workspace}<aside aria-label="Library">Library</aside>{/if}
    <main>
        <div data-editor-fixture="program">
            <ObjectEditor
                selection={program}
                assignmentQuery=""
                onassignmentquerychange={() => {}}
                onassignmentselect={() => {}}
            />
        </div>
        <div data-editor-fixture="sample"><DeviceEditorHostHarness {workflow} {sample} /></div>
        <div data-editor-fixture="bank"><DeviceEditorHostHarness {workflow} sample={bank} kind="sample-bank" /></div>
    </main>
    {#if workspace}<aside aria-label="Inspector">Inspector</aside>{/if}
</div>

<style>
    :global(body) {
        margin: 0;
    }
    nav {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        padding: 4px 8px;
        min-height: 32px;
    }
    nav button {
        font-size: 11px;
    }
    .layout,
    main {
        min-width: 0;
    }
    .workspace {
        display: grid;
        grid-template-columns: 288px minmax(0, 1fr) 268px;
    }
    aside {
        padding: 8px;
        color: var(--color-text-muted);
        border: 1px solid var(--color-border);
    }
    [data-editor-fixture] {
        min-width: 0;
        height: 250px;
        border-bottom: 1px solid var(--color-border);
    }
    [data-editor-fixture='program'] {
        height: 160px;
    }
</style>

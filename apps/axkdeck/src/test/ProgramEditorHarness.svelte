<script lang="ts">
    import DeviceEditorHost from '../features/object-editor/DeviceEditorHost.svelte';
    import { provideObjectEditors } from '../features/object-editor/context';
    import { ObjectEditorWorkflow } from '../features/object-editor/workflow.svelte';
    import type { InspectorSelection } from '../lib/types';
    import { HttpImageTransport } from '../lib/httpTransport';
    import { programEditorFixture } from './programEditorFixture';

    const params = new URLSearchParams(location.search);
    const native = params.has('native');
    const compact = params.get('lower') === '320';
    const fixture = programEditorFixture(native, !params.has('readonly'));
    const targetCount = Math.max(2, Math.min(2048, Number(params.get('targets')) || 2));
    for (let index = fixture.editing.targets.length; index < targetCount; ++index) {
        fixture.editing.targets.push({
            ...fixture.editing.targets[1]!,
            objectId: `target-${index + 1}`,
            name: `Target ${String(index + 1).padStart(4, '0')}`,
        });
    }
    let assignmentQuery = $state('');
    const { catalog } = fixture;
    const http = new HttpImageTransport({
        baseUrl: `${location.origin}/api/v1`,
        bearerToken: 'program-editor-browser-fixture',
        mode: 'local',
    });
    const workflow = new ObjectEditorWorkflow({
        transport: {
            objectDetail: async () => fixture.document.detail!,
            programEditorCatalog: () => http.programEditorCatalog(),
            startObjectParameterEdit: async () => {
                throw new Error('Browser layout fixture must not write');
            },
            waitForJob: async () => {
                throw new Error('Browser layout fixture has no jobs');
            },
        },
        refresh: async () => {},
        stopPlayback: () => {},
        status: () => {},
    });
    const document = $derived(workflow.find(1, 'program') ?? fixture.document);
    const navigation = workflow.navigation('a-series/program');
    navigation.tab = params.get('tab') === 'sample-select' ? 'sample-select' : 'effects';
    provideObjectEditors(workflow);
    const selection: InspectorSelection = {
        kind: 'program',
        program: {
            id: 'program',
            objectId: 'program',
            slot: '033',
            programNumber: 33,
            name: 'Ambient pads',
            object: {
                key: 'program',
                objectType: 'PROG',
                name: 'Ambient pads',
                partitionIndex: 0,
                partitionName: 'PARTITION 1',
                volumeName: 'Volume',
                categoryName: 'PROG',
                objectEncoding: native ? 'legacy' : 'current',
                directoryEntryName: '033',
                sfsId: 0,
                storedSizeBytes: 0,
                sizeWithDependenciesBytes: 0,
                sampleRate: 0,
                rootKey: 0,
                storedFrameCount: 0,
                waveStartFrame: 0,
                waveLengthFrames: 0,
                storageState: 'COMPLETE',
                sampleWidthBytes: 0,
            },
        },
        assignments: [],
        sampleSelect: { assigned: [], all: [] },
    };
</script>

<main class="fixture-workspace">
    <aside aria-label="Fixture image navigation">
        <h2>Volumes</h2>
        <p>Volume</p>
        <p>Programs</p>
    </aside>
    <div class="fixture-center" style:grid-template-rows={compact ? 'minmax(100px, 1fr) 320px' : undefined}>
        <section class="fixture-collection" aria-label="Fixture Program collection">
            <h2>Programs</h2>
            <p>033 Ambient pads <span>{native ? 'a3k' : 'a4k/a5k'}</span></p>
        </section>
        <section class="fixture-editor" aria-label="Fixture lower editor pane">
            <DeviceEditorHost
                sessionId={1}
                {selection}
                {assignmentQuery}
                onassignmentquerychange={(value) => (assignmentQuery = value)}
            />
        </section>
    </div>
    <aside aria-label="Fixture inspector">
        <h2>Program</h2>
        <p>Ambient pads</p>
        <p>Stored assignments: 2</p>
    </aside>
</main>
<output hidden data-program-catalog>{JSON.stringify(catalog)}</output>
<output hidden data-program-state
    >{JSON.stringify({
        values: document.draft.values,
        changes: document.draft.changes,
        dirty: document.draft.dirty,
        tab: navigation.tab,
        page: navigation.page,
        slot: navigation.effectSlot,
        assignment: document.programAssignmentId,
        assignmentQuery,
        targetCount,
    })}</output
>

<style>
    .fixture-workspace {
        display: grid;
        grid-template-columns: 240px minmax(0, 1fr) 244px;
        gap: 8px;
        padding: 8px;
        height: calc(100dvh / var(--fixture-zoom, 1));
        overflow: hidden;
    }
    .fixture-center {
        display: grid;
        grid-template-rows: minmax(100px, 28%) minmax(0, 1fr);
        gap: 8px;
        min-width: 0;
        min-height: 0;
    }
    .fixture-editor {
        min-width: 0;
        min-height: 0;
    }
    aside,
    .fixture-collection {
        background: var(--color-panel);
        padding: 8px;
    }
    h2,
    p {
        margin: 0 0 8px;
        font-size: 11px;
    }
    h2 {
        font-weight: 600;
    }
    span {
        color: var(--color-text-muted);
        margin-left: 12px;
    }
    @media (max-width: 900px) {
        .fixture-workspace {
            grid-template-columns: minmax(0, 1fr);
        }
        aside {
            display: none;
        }
    }
</style>

<script lang="ts">
    import OpenSourceDialog from '../features/dialogs/OpenSourceDialog.svelte';
    import DocumentSelector from '../features/workspace/DocumentSelector.svelte';
    import ImageSelector from '../features/workspace/ImageSelector.svelte';
    import Tx16wPresentation from '../features/devices/tx16w/Presentation.svelte';
    import WorkspaceShell from '../features/workspace/WorkspaceShell.svelte';
    import FilesInspector from '../features/files/FilesInspector.svelte';
    import ImageNavigator from '../lib/components/ImageNavigator.svelte';
    import { ViewMemory, shellViewState } from '../features/workspace/viewMemory';
    import { provideASeriesPreferences, ASeriesPreferences } from '../lib/aSeriesPreferences.svelte';
    import { nativeWorkspaceFixture } from './nativeWorkspaceFixture';
    import { nativeDocumentFixture, nativeObjects, tx16wDocument } from './nativeDocumentFixture';
    import type { ImageLocation } from '../lib/storageLocations';
    import type { DevicePresentationState, WorkspacePresentation } from '../features/workspace/contracts';

    provideASeriesPreferences(new ASeriesPreferences());
    const transport = nativeDocumentFixture();
    const memory = new ViewMemory();
    const sources = [
        ...nativeWorkspaceFixture().sources,
        ...[2, 3].map((index) => ({
            id: `source-${index}`,
            source: { kind: 'FILE' as const, file: { rootId: 'root', relativePath: `tx16w-${index - 1}.img` } },
            format: 'fat12',
            filesystemRootIds: [`source-${index}:fat12`],
            diagnostics: [],
        })),
    ];
    const documents = [nativeWorkspaceFixture().documents[0]!, tx16wDocument];
    let opening = $state(true);
    let selections = $state<ImageLocation[]>([]);
    let documentId = $state<string | null>(tx16wDocument.id);
    let activeSourceMemberId = $state('source-2');
    let mode = $state<'device' | 'files'>('device');
    let reveal = $state('');
    let conversions = $state(0);
    let companions = $state(0);
    const remember = $derived(memory.forDocument(1, documentId));
    async function select(id: string | null) {
        documentId = id;
        mode = id ? 'device' : 'files';
    }
</script>

{#snippet imageActions()}<button class="secondary-button" type="button" onclick={() => (opening = true)}>Open</button
    >{/snippet}
{#snippet selector()}
    <ImageSelector
        {sources}
        selected={activeSourceMemberId}
        disabled={false}
        onerror={() => undefined}
        onselect={async (id) => {
            activeSourceMemberId = id;
            await select(documents.find((document) => document.sourceMemberIds.includes(id))?.id ?? null);
        }}
    />
    <DocumentSelector
        documents={documents.filter((document) => document.sourceMemberIds.includes(activeSourceMemberId))}
        selected={documentId}
        disabled={false}
        onselect={select}
        onresolve={async () => undefined}
        discovery={{
            selectionRequired: false,
            candidates: [
                {
                    formatId: 'yamaha.tx16w',
                    state: 'INCOMPLETE',
                    sourceMemberIds: ['source-2'],
                    diagnostics: ['Missing wave companion'],
                },
            ],
        }}
    />
{/snippet}
{#snippet files()}<p class="empty-copy">Files workspace</p>
    <output aria-label="Selected source count">{selections.length}</output>{/snippet}
{#snippet aSeriesNavigation()}
    <ImageNavigator
        navigationOnly
        image={{
            kind: 'server-file',
            reference: { rootId: 'root', relativePath: 'Disk1.hds' },
            displayName: 'Disk1.hds',
        }}
        items={[{ id: 'partition', kind: 'partition', name: 'Partition 0', childCount: 0 }]}
        selectedId="partition"
        opening={false}
        storageLocationsAvailable={false}
        onopen={() => undefined}
        oncreate={() => undefined}
        onclose={() => undefined}
        onmanagelocations={() => undefined}
        onselect={() => undefined}
        onloadchildren={async () => ({ items: [], totalCount: 0 })}
        volumeActionsEnabled={false}
        partitionActionsEnabled={false}
        onimageaction={() => undefined}
    />
{/snippet}
{#snippet filesInspector()}
    <FilesInspector
        {documents}
        entry={{
            id: 'voice-file',
            parentId: 'root',
            rootId: 'root',
            ancestorIds: ['root'],
            name: 'VOICE.V01',
            path: '/VOICE.V01',
            kind: 'file',
            sizeBytes: 32768,
            childCount: 0,
            sourceMemberId: 'source-2',
            bindings: nativeObjects.map((object) => ({
                documentId: tx16wDocument.id,
                objectId: object.id,
                contentScopeId: null,
            })),
            interpretation: '',
            storage: '',
            issue: '',
            filesystemMetadata: false,
            rawAttributes: '',
            attributes: [],
        }}
        onshowdevice={(binding) => void select(binding.documentId)}
    />
{/snippet}
{#snippet shell(presentation: WorkspacePresentation, _state: DevicePresentationState)}
    <WorkspaceShell
        {mode}
        {imageActions}
        documentSelector={selector}
        viewState={remember('shell', shellViewState)}
        imageName="Native source set"
        context={documentId ?? 'Files'}
        status="Read only"
        deviceAvailable={!!documentId}
        onmodechange={(value) => (mode = value)}
        presentation={mode === 'device'
            ? presentation
            : { navigation: files, content: files, inspector: filesInspector }}
    />
{/snippet}
{#key documentId}
    {#if documentId === tx16wDocument.id}
        <Tx16wPresentation
            {transport}
            {sources}
            {remember}
            sessionId={1}
            revision={1}
            document={tx16wDocument}
            onconvert={() => conversions++}
            onaddcompanions={() => companions++}
            onrevealFiles={(id) => {
                reveal = id;
                mode = 'files';
            }}
            children={shell}
        />
    {:else}
        {@render shell(
            { navigation: aSeriesNavigation, content: files },
            { lowerOpen: false, setLowerOpen: () => undefined, showObject: async () => undefined },
        )}
    {/if}
{/key}
<output hidden aria-label="Revealed object">{reveal}</output>
<output hidden aria-label="Conversion requests">{conversions}</output>
<output hidden aria-label="Companion requests">{companions}</output>
{#if opening}
    <OpenSourceDialog
        {transport}
        mode="media-source"
        extensions={[]}
        onselect={() => undefined}
        onselectsources={(items) => {
            selections = items;
            opening = false;
        }}
        oncancel={() => (opening = false)}
    />
{/if}

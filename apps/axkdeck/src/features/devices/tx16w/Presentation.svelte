<script lang="ts">
    import { onDestroy, untrack, type Snippet } from 'svelte';
    import type { RememberView } from '../../workspace/viewMemory';
    import type { components } from '../../../lib/generated/axklibApiV1';
    import type { ImageTransport, NativeDocumentSummary, OpenedImage } from '../../../lib/transport';
    import type { DevicePresentationState, WorkspacePresentation } from '../../workspace/contracts';
    import CollectionRow from '../../../lib/components/CollectionRow.svelte';
    import CollectionIdentity from '../../../lib/components/CollectionIdentity.svelte';
    import WaveCollectionRow from '../../../lib/components/WaveCollectionRow.svelte';
    import {
        emptyCollectionSelection,
        updateCollectionSelection,
        collectionSelectionMode,
        type CollectionSelectionMode,
    } from '../../../lib/collectionSelection';
    import { DocumentAudio, previewTimeline } from '../../../lib/audio/documentAudio.svelte';
    import ObjectDetail from './ObjectDetail.svelte';
    import Icon from '../../../lib/components/Icon.svelte';
    import CollectionToolbar from '../../../lib/components/CollectionToolbar.svelte';
    import ObjectContextMenu from '../../../lib/components/ObjectContextMenu.svelte';
    import StructureNavigation from '../../../lib/components/StructureNavigation.svelte';
    import {
        collectionPageStep,
        hasDisallowedNavigationModifier,
        keyboardSelectionMode,
        focusCollectionIndex,
        linearNavigationIndex,
        revealCollectionObject,
    } from '../../../lib/collectionNavigation';
    import { userFacingMessage } from '../../../lib/userFacingMessage';

    let {
        transport,
        sessionId,
        revision,
        document,
        sources,
        onrevealFiles,
        onconvert,
        visible: presentationVisible = true,
        children,
        remember,
    }: {
        transport: ImageTransport;
        sessionId: number;
        revision: number;
        document: NativeDocumentSummary;
        sources: OpenedImage['sources'];
        onrevealFiles: (objectId: string) => void;
        onconvert?: () => void;
        visible?: boolean;
        children: Snippet<[WorkspacePresentation, DevicePresentationState]>;
        remember: RememberView;
    } = $props();
    const audio = untrack(() => new DocumentAudio(transport, () => sessionId));
    let objects = $state<components['schemas']['ImageObjectItem'][]>([]);
    let relationships = $state<components['schemas']['ImageRelationshipItem'][]>([]);
    let detail = $state<components['schemas']['Tx16wObjectDetail'] | null>(null);
    const tabs = [
        { kind: 'performance', label: 'Performances', icon: 'program-multi' },
        { kind: 'voice', label: 'Voices', icon: 'music' },
        { kind: 'timbre', label: 'Timbres', icon: 'sliders' },
        { kind: 'wave', label: 'Waves', icon: 'waveform' },
        { kind: 'setup', label: 'Setups', icon: 'settings' },
    ] as const;
    type Kind = (typeof tabs)[number]['kind'];
    const emptyCollection = () => ({ selected: '', query: '', selection: emptyCollectionSelection() });
    const retained = untrack(() =>
        remember('tx16w', () => ({
            kind: 'performance' as Kind,
            structureQuery: '',
            collections: {
                performance: emptyCollection(),
                voice: emptyCollection(),
                timbre: emptyCollection(),
                wave: emptyCollection(),
                setup: emptyCollection(),
            },
        })),
    );
    let kind = $state(retained.kind);
    let structureQuery = $state(retained.structureQuery);
    let collections = $state(retained.collections);
    const current = $derived(collections[kind]);
    const selected = $derived(current.selected);
    const selectedIds = $derived(new Set(current.selection.items.map((item) => item.objectId)));
    const title = $derived(tabs.find((tab) => tab.kind === kind)!.label);
    const collection = $derived(objects.filter((object) => object.type === kind));
    let contentElement = $state<HTMLElement | null>(null);
    let menu = $state<{ left: number; top: number } | null>(null);
    let loading = $state(false);
    let error = $state('');
    let generation = 0;
    let detailGeneration = 0;
    let pendingLoad: Promise<void> = Promise.resolve();
    const visible = $derived(
        collection.filter((object) => object.name.toLowerCase().includes(current.query.toLowerCase())),
    );
    const selectedEdges = $derived(
        relationships.filter(
            (edge) =>
                edge.sourceObjectId === selected ||
                edge.targetObjectId === selected ||
                edge.candidateObjectIds.includes(selected),
        ),
    );
    $effect(() => {
        const currentRevision = revision;
        const currentSession = sessionId;
        const currentDocument = document.id;
        const token = ++generation;
        ++detailGeneration;
        detail = null;
        objects = [];
        relationships = [];
        menu = null;
        error = '';
        loading = true;
        untrack(() => audio.reset());
        pendingLoad = load(currentSession, currentRevision, currentDocument, token);
        return () => {
            ++generation;
            ++detailGeneration;
        };
    });
    async function load(id: number, expectedRevision: number, documentId: string, token: number) {
        try {
            const nextObjects = await transport.nativeObjects(id);
            const nextRelationships = await transport.nativeRelationships(id);
            if (token !== generation || revision !== expectedRevision || document.id !== documentId) return;
            objects = nextObjects;
            relationships = nextRelationships;
            for (const tab of tabs) {
                const state = collections[tab.kind];
                const available = new Set(
                    objects.filter((object) => object.type === tab.kind).map((object) => object.id),
                );
                if (!available.has(state.selected)) state.selected = '';
                state.selection.items = state.selection.items.filter((item) => available.has(item.objectId));
                state.selection.anchors = Object.fromEntries(
                    Object.entries(state.selection.anchors).filter(([, anchor]) => available.has(anchor)),
                );
            }
            if (selected) void choose(selected);
        } catch (reason) {
            if (token === generation) error = userFacingMessage(reason);
        } finally {
            if (token === generation) loading = false;
        }
    }
    async function choose(objectId: string): Promise<void> {
        const token = ++detailGeneration;
        const documentId = document.id;
        if (audio.state.objectId !== objectId) audio.stop();
        current.selected = objectId;
        detail = null;
        error = '';
        try {
            const value = await transport.nativeObjectDetail(sessionId, objectId);
            if (token !== detailGeneration) return;
            if (value.formatId !== 'yamaha.tx16w' || value.documentId !== documentId || value.revision !== revision)
                throw new Error('Native document changed during loading');
            detail = value;
            if (value.details.type === 'wave') audio.request(objectId);
        } catch (reason) {
            if (token === detailGeneration) error = userFacingMessage(reason);
        }
    }
    function selectKind(next: Kind): void {
        if (next !== kind) audio.stop();
        kind = next;
        ++detailGeneration;
        detail = null;
        error = '';
        if (selected && !loading) void choose(selected);
    }
    export async function showObject(id: string): Promise<void> {
        const token = generation;
        await pendingLoad;
        if (token !== generation) return;
        const object = objects.find((item) => item.id === id);
        const tab = tabs.find((item) => item.kind === object?.type);
        if (!object || !tab) return;
        kind = tab.kind;
        if (!object.name.toLowerCase().includes(current.query.toLowerCase())) current.query = '';
        select(id, 'replace');
        if (contentElement) await revealCollectionObject(contentElement, kind, id, 'center', true);
    }
    function select(id: string, mode: CollectionSelectionMode): void {
        const identity = (item: (typeof objects)[number]) => ({ objectId: item.id });
        current.selection = updateCollectionSelection(
            current.selection,
            kind,
            collection.map(identity),
            visible.map(identity),
            id,
            mode,
        ).selection;
        if (mode === 'replace') void choose(id);
    }
    function navigate(event: KeyboardEvent, index: number): void {
        if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a' && !event.altKey) {
            event.preventDefault();
            select(visible[index]!.id, 'all');
            return;
        }
        if (hasDisallowedNavigationModifier(event)) return;
        const next = linearNavigationIndex(event.key, index, visible.length, collectionPageStep(event.currentTarget));
        if (next === null) return;
        event.preventDefault();
        select(visible[next]!.id, keyboardSelectionMode(event));
        void focusCollectionIndex(event.currentTarget, next);
    }
    $effect(() => {
        if (!presentationVisible || (audio.state.objectId && !visible.some((item) => item.id === audio.state.objectId)))
            untrack(() => audio.stop());
    });
    function openMenu(event: MouseEvent | KeyboardEvent): void {
        if (!onconvert) return;
        event.preventDefault();
        const target = event.currentTarget as HTMLElement;
        target.focus();
        const rect = target.getBoundingClientRect();
        menu = {
            left: event instanceof MouseEvent ? event.clientX : rect.left,
            top: event instanceof MouseEvent ? event.clientY : rect.bottom,
        };
    }
    onDestroy(() => {
        audio.dispose();
        ++generation;
        ++detailGeneration;
        Object.assign(retained, { kind, collections, structureQuery });
    });
</script>

{#snippet navigation()}
    <StructureNavigation
        label="TX16W structure"
        heading="TX16W floppy image"
        bind:query={structureQuery}
        searchLabel="Search TX16W structure"
        navigation
    >
        {#if document.companions.requirements.length}
            <details class="source-status">
                <summary
                    >{document.companions.requirements.some((item) => item.state === 'MISSING')
                        ? 'Missing companion files'
                        : 'Source issues'}</summary
                >
                <ul>
                    {#each document.companions.requirements as item}
                        <li>{item.name}: {item.state.toLowerCase()}</li>
                    {/each}
                </ul>
            </details>
        {:else if document.completeness === 'INVALID'}
            <p class="source-status">Invalid native data</p>
        {/if}
        <div class="roots">
            {#if '/'.includes(structureQuery.trim())}
                <button
                    type="button"
                    class="root selected"
                    aria-pressed="true"
                    title="/"
                    oncontextmenu={openMenu}
                    onkeydown={(event) => {
                        if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) openMenu(event);
                    }}
                >
                    <Icon name="hard-drive" size={15} /><span>/</span>
                </button>
            {:else}<p class="empty-copy">No matching contents</p>{/if}
        </div>
    </StructureNavigation>
    {#if menu}<ObjectContextMenu
            objectName="/"
            left={menu.left}
            top={menu.top}
            {onconvert}
            convertLabel="Convert..."
            onclose={() => (menu = null)}
        />{/if}
{/snippet}
{#snippet deviceTabs()}
    <nav class="workspace-tabs" aria-label="Workspace views">
        {#each tabs as tab (tab.kind)}
            <button
                type="button"
                class:active={kind === tab.kind}
                aria-pressed={kind === tab.kind}
                aria-label={tab.label}
                title={tab.label}
                onclick={() => selectKind(tab.kind)}
            >
                <Icon name={tab.icon} size={16} /><span>{tab.label}</span>
            </button>
        {/each}
    </nav>
{/snippet}
{#snippet content()}
    <section class="tx-objects" aria-label="TX16W native objects" aria-busy={loading} bind:this={contentElement}>
        <CollectionToolbar
            {title}
            count={collection.length}
            query={current.query}
            onquerychange={(value) => (current.query = value)}
        />
        {#if error}<p role="alert">{error}</p>{/if}
        <div
            class="collection-body"
            class:compact-collection-list={kind !== 'wave'}
            class:empty-collection={visible.length === 0}
            class:wave-data-list={kind === 'wave'}
            data-collection-list={kind}
            data-navigation-list
            aria-label={title}
        >
            {#each visible as object, index (object.id)}
                {#if kind === 'wave'}
                    {@const preview = audio.previews[object.id]}
                    {@const lane = preview?.lanes[0]}
                    <WaveCollectionRow
                        objectId={object.id}
                        name={object.name || object.id}
                        {index}
                        active={selected === object.id}
                        selected={selectedIds.has(object.id)}
                        metadata={lane
                            ? `${lane.sampleRateApproximate ? 'Approx. ' : ''}${(lane.storedFrameCount / lane.sampleRate).toFixed(3)} s · ${lane.sampleRate.toLocaleString()} Hz`
                            : `${object.sizeBytes.toLocaleString()} B`}
                        format={lane ? `${lane.nativeBitDepth}-bit · Mono` : '12-bit · Mono'}
                        values={lane?.bins ?? []}
                        timeline={previewTimeline(preview)}
                        playing={audio.state.objectId === object.id && audio.state.status === 'playing'}
                        preparing={audio.state.objectId === object.id && audio.state.status === 'preparing'}
                        playheadFrame={audio.state.playheadFrame}
                        unavailable={audio.errors[object.id] ?? ''}
                        onselect={(event) => select(object.id, collectionSelectionMode(event))}
                        onkeydown={(event) => navigate(event, index)}
                        onpreview={(bins) => audio.request(object.id, bins)}
                        onprefetch={() => audio.prefetch(object.id)}
                        onplay={() => {
                            select(object.id, 'replace');
                            audio.play(object.id);
                        }}
                        onstop={() => audio.stop()}
                        onseek={(ratio) => {
                            if (selected !== object.id || selectedIds.size !== 1 || !selectedIds.has(object.id))
                                select(object.id, 'replace');
                            audio.seek(object.id, ratio);
                        }}
                    />
                {:else}
                    <CollectionRow
                        active={selected === object.id}
                        selected={selectedIds.has(object.id)}
                        data-collection-object-id={object.id}
                        data-navigation-index={index}
                        onclick={(event) => select(object.id, collectionSelectionMode(event))}
                        onkeydown={(event) => navigate(event, index)}
                    >
                        <CollectionIdentity
                            name={object.name || object.id}
                            metadata={`${object.sizeBytes.toLocaleString()} B`}
                        />
                    </CollectionRow>
                {/if}
            {:else}<p class="empty-copy">
                    {loading
                        ? 'Reading native objects'
                        : current.query
                          ? 'No matching objects'
                          : `No ${title.toLowerCase()}`}
                </p>{/each}
        </div>
    </section>
{/snippet}
{#snippet inspector()}
    <ObjectDetail
        {detail}
        preview={detail ? audio.previews[detail.objectId] : undefined}
        audioError={detail
            ? (audio.errors[detail.objectId] ??
              (audio.state.objectId === detail.objectId ? (audio.state.error ?? '') : ''))
            : ''}
        playingObjectId={audio.state.objectId}
        playheadFrame={audio.state.playheadFrame}
        relationships={selectedEdges}
        {objects}
        {sources}
        onselect={(id) => void showObject(id)}
        {onrevealFiles}
    />
{/snippet}
{@render children(
    { navigation, content, inspector, tabs: deviceTabs },
    { lowerOpen: false, setLowerOpen: () => undefined, showObject },
)}

<style>
    .source-status {
        margin: 0 9px 8px;
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .source-status ul {
        padding-left: 14px;
        max-height: 120px;
        overflow: auto;
        overflow-wrap: anywhere;
    }
    .roots {
        padding: 0 8px 8px;
    }
    .root {
        display: flex;
        align-items: center;
        gap: 7px;
        width: 100%;
        text-align: left;
        min-height: 26px;
        padding: 3px 6px;
        border: 1px solid color-mix(in srgb, var(--state-active-border) 35%, transparent);
        border-radius: 4px;
        background: var(--state-selected-surface);
        color: var(--color-text);
        font-size: 11px;
    }
    .tx-objects {
        height: 100%;
        min-height: 0;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }
</style>

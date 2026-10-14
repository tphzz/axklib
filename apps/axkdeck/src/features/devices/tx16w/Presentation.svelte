<script lang="ts">
    import { onDestroy, untrack, type Snippet } from 'svelte';
    import type { RememberView } from '../../workspace/viewMemory';
    import type { components } from '../../../lib/generated/axklibApiV1';
    import type { ImageTransport, NativeDocumentSummary, OpenedImage } from '../../../lib/transport';
    import type { DevicePresentationState, WorkspacePresentation } from '../../workspace/contracts';
    import ObjectDetail from './ObjectDetail.svelte';
    import Icon from '../../../lib/components/Icon.svelte';
    import CollectionToolbar from '../../../lib/components/CollectionToolbar.svelte';
    import ObjectContextMenu from '../../../lib/components/ObjectContextMenu.svelte';
    import StructureNavigation from '../../../lib/components/StructureNavigation.svelte';
    import {
        collectionPageStep,
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
        onaddcompanions,
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
        onaddcompanions?: () => void;
        children: Snippet<[WorkspacePresentation, DevicePresentationState]>;
        remember: RememberView;
    } = $props();
    let objects = $state<components['schemas']['ImageObjectItem'][]>([]);
    let relationships = $state<components['schemas']['ImageRelationshipItem'][]>([]);
    let detail = $state<components['schemas']['Tx16wObjectDetail'] | null>(null);
    const tabs = [
        { kind: 'performance', label: 'Performances', icon: 'program-multi' },
        { kind: 'voice', label: 'Voices', icon: 'music' },
        { kind: 'timbre', label: 'Timbres', icon: 'sliders' },
        { kind: 'wave', label: 'Waves', icon: 'waveform' },
        { kind: 'setup', label: 'Setups', icon: 'settings' },
        { kind: 'native-file', label: 'Native Files', icon: 'file' },
    ] as const;
    type Kind = (typeof tabs)[number]['kind'];
    const emptyCollection = () => ({ selected: '', query: '' });
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
                'native-file': emptyCollection(),
            },
        })),
    );
    let kind = $state(retained.kind);
    let structureQuery = $state(retained.structureQuery);
    let collections = $state(retained.collections);
    const current = $derived(collections[kind]);
    const selected = $derived(current.selected);
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
                if (!objects.some((object) => object.id === state.selected && object.type === tab.kind))
                    state.selected = '';
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
        current.selected = objectId;
        detail = null;
        error = '';
        try {
            const value = await transport.nativeObjectDetail(sessionId, objectId);
            if (token !== detailGeneration) return;
            if (value.formatId !== 'yamaha.tx16w' || value.documentId !== documentId || value.revision !== revision)
                throw new Error('Native document changed during loading');
            detail = value;
        } catch (reason) {
            if (token === detailGeneration) error = userFacingMessage(reason);
        }
    }
    function selectKind(next: Kind): void {
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
        void choose(id);
        if (contentElement) await revealCollectionObject(contentElement, kind, id, 'center', true);
    }
    function navigate(event: KeyboardEvent, index: number): void {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
        const row = (event.currentTarget as HTMLElement).closest('tr');
        const next = linearNavigationIndex(event.key, index, visible.length, collectionPageStep(row));
        if (next === null) return;
        event.preventDefault();
        void choose(visible[next]!.id);
        void focusCollectionIndex(event.currentTarget, next);
    }
    function openMenu(event: MouseEvent | KeyboardEvent): void {
        if (!onconvert && !onaddcompanions) return;
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
            {onaddcompanions}
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
        <div class="object-scroll" data-collection-list={kind} data-navigation-list aria-label={title}>
            <table>
                <thead><tr><th>Name</th><th>Bytes</th></tr></thead><tbody>
                    {#each visible as object, index (object.id)}
                        <tr class:selected={selected === object.id}
                            ><td
                                ><button
                                    type="button"
                                    data-collection-object-id={object.id}
                                    data-navigation-index={index}
                                    aria-pressed={selected === object.id}
                                    onclick={() => void choose(object.id)}
                                    onkeydown={(event) => navigate(event, index)}>{object.name || object.id}</button
                                ></td
                            ><td>{object.sizeBytes.toLocaleString()}</td></tr
                        >
                    {:else}<tr
                            ><td colspan="2"
                                >{loading
                                    ? 'Reading native objects'
                                    : current.query
                                      ? 'No matching objects'
                                      : `No ${title.toLowerCase()}`}</td
                            ></tr
                        >{/each}
                </tbody>
            </table>
        </div>
    </section>
{/snippet}
{#snippet inspector()}
    <ObjectDetail
        {detail}
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
        border: 1px solid rgb(104 151 187 / 35%);
        border-radius: 4px;
        background: linear-gradient(90deg, rgb(55 91 119 / 76%), rgb(55 91 119 / 32%));
        color: #e8edf1;
        font-size: 11px;
    }
    tr.selected {
        background: linear-gradient(120deg, rgb(55 91 119 / 68%), rgb(49 60 68 / 68%));
    }
    .tx-objects {
        height: 100%;
        min-height: 0;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }
    .object-scroll {
        overflow: auto;
        flex: 1;
    }
    table {
        width: 100%;
        border-collapse: collapse;
        font-size: 11px;
    }
    th {
        position: sticky;
        top: 0;
        background: var(--color-panel);
        font-size: 10px;
    }
    th,
    td {
        text-align: left;
        padding: 6px 8px;
        border-bottom: 1px solid var(--color-border);
    }
    td:first-child {
        width: 65%;
    }
    td button {
        background: none;
        border: 0;
        color: var(--color-text);
        font-size: 11px;
        text-align: left;
        overflow-wrap: anywhere;
    }
</style>

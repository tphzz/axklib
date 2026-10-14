<script lang="ts">
    import { onDestroy, untrack, type Snippet } from 'svelte';
    import type { RememberView } from '../../workspace/viewMemory';
    import type { components } from '../../../lib/generated/axklibApiV1';
    import type { ImageTransport, NativeDocumentSummary, OpenedImage } from '../../../lib/transport';
    import type { DevicePresentationState, WorkspacePresentation } from '../../workspace/contracts';
    import ObjectDetail from './ObjectDetail.svelte';
    import { userFacingMessage } from '../../../lib/userFacingMessage';

    let {
        transport,
        sessionId,
        revision,
        document,
        sources,
        onrevealFiles,
        children,
        remember,
    }: {
        transport: ImageTransport;
        sessionId: number;
        revision: number;
        document: NativeDocumentSummary;
        sources: OpenedImage['sources'];
        onrevealFiles: (objectId: string) => void;
        children: Snippet<[WorkspacePresentation, DevicePresentationState]>;
        remember: RememberView;
    } = $props();
    let objects = $state<components['schemas']['ImageObjectItem'][]>([]);
    let relationships = $state<components['schemas']['ImageRelationshipItem'][]>([]);
    let detail = $state<components['schemas']['Tx16wObjectDetail'] | null>(null);
    const retained = untrack(() => remember('tx16w', () => ({ selected: '', kind: '', query: '' })));
    let selected = $state(retained.selected);
    let kind = $state(retained.kind);
    let query = $state(retained.query);
    let loading = $state(false);
    let error = $state('');
    let generation = 0;
    let detailGeneration = 0;
    const kinds = ['setup', 'performance', 'voice', 'timbre', 'wave', 'native-file'];
    const visible = $derived(
        objects.filter(
            (object) => (!kind || object.type === kind) && object.name.toLowerCase().includes(query.toLowerCase()),
        ),
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
        const token = ++generation;
        ++detailGeneration;
        detail = null;
        error = '';
        loading = true;
        void load(currentSession, currentRevision, token);
        return () => {
            ++generation;
            ++detailGeneration;
        };
    });
    async function load(id: number, expectedRevision: number, token: number) {
        try {
            const nextObjects = await transport.nativeObjects(id);
            const nextRelationships = await transport.nativeRelationships(id);
            if (token !== generation || revision !== expectedRevision) return;
            objects = nextObjects;
            relationships = nextRelationships;
            if (!objects.some((object) => object.id === selected)) selected = '';
            if (selected) await choose(selected);
        } catch (reason) {
            if (token === generation) error = userFacingMessage(reason);
        } finally {
            if (token === generation) loading = false;
        }
    }
    async function choose(objectId: string): Promise<void> {
        const token = ++detailGeneration;
        const documentId = document.id;
        selected = objectId;
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
    export async function showObject(id: string): Promise<void> {
        kind = '';
        query = '';
        await choose(id);
    }
    onDestroy(() => {
        ++generation;
        ++detailGeneration;
        Object.assign(retained, { selected, kind, query });
    });
</script>

{#snippet navigation()}
    <nav class="tx-navigation" aria-label="TX16W objects">
        <h2>Yamaha TX16W</h2>
        <p>{document.completeness === 'INCOMPLETE' ? 'Incomplete source set' : 'Read only'}</p>
        <button type="button" class:active={!kind} onclick={() => (kind = '')}
            >All objects <span>{objects.length}</span></button
        >
        {#each kinds as type}
            <button type="button" class:active={kind === type} onclick={() => (kind = type)}
                >{type}<span>{objects.filter((object) => object.type === type).length}</span></button
            >
        {/each}
    </nav>
{/snippet}
{#snippet content()}
    <section class="tx-objects" aria-label="TX16W native objects" aria-busy={loading}>
        <header>
            <h2>{kind || 'Objects'}</h2>
            <input aria-label="Search native objects" placeholder="Search" bind:value={query} />
        </header>
        {#if error}<p role="alert">{error}</p>{/if}
        <div class="object-scroll">
            <table>
                <thead><tr><th>Name</th><th>Type</th><th>Bytes</th></tr></thead><tbody>
                    {#each visible as object (object.id)}
                        <tr class:selected={selected === object.id}
                            ><td
                                ><button type="button" onclick={() => void choose(object.id)}
                                    >{object.name || object.id}</button
                                ></td
                            ><td>{object.type}</td><td>{object.sizeBytes.toLocaleString()}</td></tr
                        >
                    {:else}<tr><td colspan="3">{loading ? 'Reading native objects' : 'No objects'}</td></tr>{/each}
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
{@render children({ navigation, content, inspector }, { lowerOpen: false, setLowerOpen: () => undefined, showObject })}

<style>
    .tx-navigation {
        padding: 10px;
        overflow: auto;
        font-size: 11px;
    }
    h2 {
        font-size: 13px;
        margin: 0;
        text-transform: capitalize;
    }
    p {
        font-size: 11px;
        color: var(--color-text-muted);
    }
    .tx-navigation button {
        display: flex;
        justify-content: space-between;
        width: 100%;
        text-align: left;
        padding: 7px;
        text-transform: capitalize;
        background: none;
        border: 0;
        color: var(--color-text);
        font-size: 11px;
    }
    .tx-navigation button.active,
    tr.selected {
        background: var(--color-panel-active, #38454a);
    }
    .tx-objects {
        height: 100%;
        min-height: 0;
        display: flex;
        flex-direction: column;
        overflow: hidden;
    }
    header {
        display: flex;
        align-items: center;
        justify-content: space-between;
        gap: 8px;
        padding: 8px;
    }
    input {
        width: 180px;
        min-width: 0;
        font-size: 11px;
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

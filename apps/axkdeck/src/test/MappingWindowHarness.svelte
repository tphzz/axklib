<script lang="ts">
    import { onMount } from 'svelte';
    import ProgramEditor from '../features/devices/a-series/program/ProgramEditor.svelte';
    import { ProgramDraft } from '../features/devices/a-series/program/draft.svelte';
    import { EditorNavigation } from '../features/object-editor/navigation.svelte';
    import type { ObjectEditorDocument } from '../features/object-editor/workflow.svelte';
    import MappingWindow from '../features/program-mapping/MappingWindow.svelte';
    import { MappingController, provideMappingEditor } from '../features/program-mapping/controller.svelte';
    import type { MappingCommand, MappingMessage } from '../features/program-mapping/protocol';
    import { programEditorFixture } from './programEditorFixture';

    const params = new URLSearchParams(location.search);
    const child = params.get('view') === 'mapping';
    const channelName = `mapping-layout-${params.get('channel') ?? 'fixture'}`;
    const channel = new BroadcastChannel(channelName);
    function post(
        message: { direction: 'command'; command: MappingCommand } | { direction: 'state'; message: MappingMessage },
    ) {
        // Match the native JSON boundary rather than structured-cloning Svelte proxies.
        channel.postMessage(JSON.parse(JSON.stringify(message)));
    }
    const fixtures = ['Warm pad', 'Bright keys', 'Read-only Program'].map((name, index) => {
        const fixture = programEditorFixture(false, index !== 2);
        fixture.editing.programName = name;
        fixture.document.detail = {
            ...fixture.document.detail!,
            object: { ...fixture.document.detail!.object, id: `program-${index}`, name },
        };
        const draft = fixture.document.draft as ProgramDraft;
        draft.acceptProgram(
            {
                ...draft.values,
                'assignments.0.key_low': index === 1 ? 12 : 24,
                'assignments.0.key_high': index === 1 ? 90 : 100,
                'assignments.1.key_low': 48,
                'assignments.1.key_high': 110,
            },
            fixture.editing.assignments,
        );
        return fixture;
    });
    let selected = $state(0);
    let saves = $state(0);
    let discards = $state(0);
    let commands = $state<MappingCommand[]>([]);
    let received = $state.raw<MappingMessage | null>(null);
    const selectedDocument = $derived(fixtures[selected]?.document ?? null);
    const delegate = {
        locked: false,
        async save(document: ObjectEditorDocument) {
            saves++;
            document.draft.accept({ ...document.draft.values });
            document.status = 'Saved in browser fixture';
        },
        async discard(document: ObjectEditorDocument) {
            discards++;
            document.draft.discard();
        },
        async recover() {
            throw new Error('The browser fixture has no write jobs');
        },
    };
    const controller = new MappingController(delegate);
    provideMappingEditor(controller);
    const navigation = new EditorNavigation();
    navigation.tab = 'easy-edit';
    navigation.page = 'range';
    const mappingAdapter = {
        async send(command: MappingCommand) {
            post({ direction: 'command', command });
        },
        async listen(callback: (message: MappingMessage) => void) {
            const listener = (event: MessageEvent<{ direction: string; message: MappingMessage }>) => {
                if (event.data.direction !== 'state') return;
                received = event.data.message;
                callback(event.data.message);
            };
            channel.addEventListener('message', listener);
            return () => channel.removeEventListener('message', listener);
        },
    };
    $effect(() => {
        if (!child) controller.refresh(selectedDocument);
    });
    onMount(() => {
        if (!child)
            void controller.connect({
                async open() {
                    const url = new URL(location.href);
                    url.searchParams.set('view', 'mapping');
                    window.open(url, 'mapping-editor-fixture', 'popup,width=1040,height=680');
                },
                async publish(message) {
                    post({ direction: 'state', message });
                },
                async listen(callback) {
                    const listener = (event: MessageEvent<{ direction: string; command: MappingCommand }>) => {
                        if (event.data.direction !== 'command') return;
                        commands.push(event.data.command);
                        callback(event.data.command);
                    };
                    channel.addEventListener('message', listener);
                    return () => channel.removeEventListener('message', listener);
                },
            });
        return () => {
            if (!child) controller.dispose();
            channel.close();
        };
    });
</script>

{#if child}
    <MappingWindow adapter={mappingAdapter} />
    <output hidden data-mapping-snapshot>{JSON.stringify(received)}</output>
{:else}
    <main class="mapping-fixture">
        <nav aria-label="Fixture Program selection">
            {#each fixtures as fixture, index}
                <button aria-label={`Select ${fixture.editing.programName}`} onclick={() => (selected = index)}
                    >{fixture.editing.programName}</button
                >
            {/each}
            <button onclick={() => (selected = -1)}>Close image</button>
            <button disabled={!selectedDocument?.draft.canUndo} onclick={() => selectedDocument?.draft.undo()}
                >Main undo</button
            >
        </nav>
        <section class="main-editor device-editor" aria-label="Main Program editor">
            {#if selectedDocument}
                {#key selectedDocument}
                    <ProgramEditor document={selectedDocument} {navigation} panelId="mapping-main-editor" />
                {/key}
            {/if}
        </section>
    </main>
    <output hidden data-mapping-main
        >{JSON.stringify({
            ready: controller.available,
            owner: controller.state.owner,
            version: controller.state.version,
            selected,
            assignment: selectedDocument?.programAssignmentId,
            values: selectedDocument?.draft.values,
            dirty: selectedDocument?.draft.dirty,
            canUndo: selectedDocument?.draft.canUndo,
            saves,
            discards,
            commands,
        })}</output
    >
{/if}

<style>
    .mapping-fixture {
        display: flex;
        flex-direction: column;
        height: 100dvh;
        padding: 12px;
        gap: 12px;
        background: var(--color-panel-deep);
    }
    nav {
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        font-size: 11px;
    }
    nav button {
        padding: 6px 8px;
        border: 1px solid var(--color-border);
    }
    .main-editor {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
        background: var(--color-panel);
    }
</style>

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
    import { MappingAuditionFixture } from './mappingAuditionFixture.svelte';

    const params = new URLSearchParams(location.search);
    const child = params.get('view') === 'mapping';
    const benchmark = params.has('benchmark');
    const count = Math.max(1, Math.min(512, Number(params.get('count')) || 2));
    const dense = params.has('dense');
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
        if (benchmark) {
            fixture.editing.targets[0]!.values.key_low = 0;
            fixture.editing.targets[0]!.values.key_high = 127;
            const base = Object.fromEntries(
                Object.entries(draft.values)
                    .filter(([key]) => key.startsWith('assignments.0.'))
                    .map(([key, value]) => [key.slice('assignments.0.'.length), value]),
            );
            fixture.editing.assignments = Array.from({ length: count }, (_, ordinal) => ({
                ordinal,
                kind: 'SBNK',
                name: 'Duplicate',
                targetObjectId: 'sample',
            }));
            draft.acceptProgram(
                {
                    ...draft.values,
                    ...Object.fromEntries(
                        fixture.editing.assignments.flatMap(({ ordinal }) =>
                            Object.entries({
                                ...base,
                                key_low: dense ? 0 : Math.floor((ordinal * 128) / count),
                                key_high: dense
                                    ? 127
                                    : Math.max(
                                          Math.floor((ordinal * 128) / count),
                                          Math.floor(((ordinal + 1) * 128) / count) - 1,
                                      ),
                                velocity_low: dense ? ordinal % 64 : 0,
                                velocity_high: dense ? 64 + (ordinal % 64) : 127,
                            }).map(([key, value]) => [`assignments.${ordinal}.${key}`, value]),
                        ),
                    ),
                },
                fixture.editing.assignments,
            );
            return fixture;
        }
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
    const audio = params.has('audio') && !child ? new MappingAuditionFixture() : null;
    const controller = new MappingController(
        delegate,
        'program',
        audio
            ? {
                  play: (document, role, token, note, velocity) => {
                      audio.notes.push({ note, velocity });
                      return audio.mapping.play(document, role, token, note, velocity);
                  },
                  release: (token) => audio.mapping.release(token),
              }
            : undefined,
    );
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
            void audio?.dispose();
        };
    });
</script>

{#if child}
    <MappingWindow adapter={mappingAdapter} />
    <output hidden data-mapping-snapshot
        >{JSON.stringify(
            benchmark
                ? {
                      requestId: received?.requestId,
                      state: {
                          version: received?.state.version,
                          editRevision: received?.state.editRevision,
                          selectionId: received?.state.selectionId,
                      },
                  }
                : received,
        )}</output
    >
{:else}
    {#if audio}<output hidden data-mapping-audio
            >{JSON.stringify({
                state: audio.state,
                notes: audio.notes,
                voices: audio.voices,
                outputState: audio.outputState,
                peak: audio.peak,
            })}</output
        >{/if}
    <main class="mapping-fixture">
        <nav aria-label="Fixture Program selection">
            {#if benchmark}<button onclick={() => controller.open()}>Mapping Editor</button>{/if}
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
            {#if selectedDocument && !benchmark}
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
            values: benchmark ? undefined : selectedDocument?.draft.values,
            dirty: selectedDocument?.draft.dirty,
            canUndo: selectedDocument?.draft.canUndo,
            saves,
            discards,
            commands: benchmark ? undefined : commands,
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

<script lang="ts">
    import { onMount } from 'svelte';
    import { ObjectEditorWorkflow, type ObjectEditorDocument } from '../features/object-editor/workflow.svelte';
    import { provideObjectEditors } from '../features/object-editor/context';
    import { MappingController, provideMappingEditor } from '../features/program-mapping/controller.svelte';
    import MappingWindow from '../features/program-mapping/MappingWindow.svelte';
    import SampleMapping from '../features/devices/a-series/sample/SampleMapping.svelte';
    import BankPreview from '../features/devices/a-series/bank/BankPreview.svelte';
    import type { MappingCommand, MappingMessage, MappingRole } from '../features/program-mapping/protocol';
    import type { ObjectParameterEdit } from '../lib/objectEditing';
    import { mappingDetail } from './mappingEditorFixture';
    const query = new URLSearchParams(location.search);
    const role = query.get('role') as MappingRole | null;
    const channel = new BroadcastChannel(`bank-mapping-${query.get('channel')}`);
    type Message = { direction: 'state'; message: MappingMessage } | { direction: 'command'; command: MappingCommand };
    const post = (message: Message) => channel.postMessage(JSON.parse(JSON.stringify(message)));
    const stored = new Map(['bank', 'a', 'b', 'other'].map((id) => [id, mappingDetail(id)]));
    for (const [id, low, high, root] of [
        ['a', 0, 47, 36],
        ['b', 48, 127, 72],
    ] as const) {
        const editing = stored.get(id)!.editing;
        if (editing?.profile === 'a-series/sample')
            Object.assign(editing.parameters, { key_low: low, key_high: high, root_key: root });
    }
    if (query.has('override')) {
        const sample = stored.get('a')!.editing;
        const bank = stored.get('bank')!.editing;
        if (sample?.profile === 'a-series/sample') sample.parameters.velocity_high = 84;
        if (bank?.profile === 'a-series/sample-bank')
            bank.bankOverrides!.units.find((unit) => unit.id === 37)!.activeSelectors = [37];
    }
    let revision = 1;
    let writes = $state<ObjectParameterEdit[]>([]);
    let bank = $state.raw<ObjectEditorDocument | null>(null);
    let a = $state.raw<ObjectEditorDocument | null>(null);
    let b = $state.raw<ObjectEditorDocument | null>(null);
    let received = $state.raw<MappingMessage | null>(null);
    const workflow = new ObjectEditorWorkflow({
        transport: {
            objectDetail: async (_session: number, id: string) => {
                const detail = structuredClone(stored.get(id)!);
                detail.image.revision = revision;
                return detail;
            },
            startObjectParameterEdit: async (_session: number, edit: ObjectParameterEdit) => {
                writes.push(edit);
                for (const operation of edit.operations) {
                    if (
                        operation.type !== 'update_sbnk_parameters' &&
                        operation.type !== 'update_sample_bank_overrides'
                    )
                        throw new Error('Unexpected fixture operation');
                    const id =
                        operation.type === 'update_sbnk_parameters'
                            ? operation.sample_name
                            : operation.sample_bank_name;
                    const detail = stored.get(id)!;
                    if (
                        detail.editing?.profile === 'a-series/sample' ||
                        detail.editing?.profile === 'a-series/sample-bank'
                    ) {
                        Object.assign(detail.editing.parameters, operation.parameters);
                        detail.editing.payloadSha256 = `${id}-${revision + 1}`.padEnd(64, '0');
                        if (operation.type === 'update_sample_bank_overrides')
                            for (const unit of detail.editing.bankOverrides!.units) {
                                if (operation.enable.includes(unit.id)) unit.activeSelectors = [...unit.selectors];
                                if (operation.disable.includes(unit.id)) unit.activeSelectors = [];
                            }
                    }
                }
                revision++;
                return { jobId: revision, kind: 'edit', status: 'queued' as const };
            },
            waitForJob: async (jobId: number) => ({ jobId, kind: 'edit', status: 'completed' as const }),
        },
        refresh: async () => {},
        stopPlayback: () => {},
        status: () => {},
    });
    provideObjectEditors(workflow);
    const controllers = (['bank', 'members', 'sample'] as const).map((role) => new MappingController(workflow, role));
    for (const controller of controllers) provideMappingEditor(controller);
    $effect(() => {
        for (const controller of controllers) controller.refresh(controller.role === 'sample' ? a : bank);
    });
    const adapter = {
        async send(command: MappingCommand) {
            post({ direction: 'command', command });
        },
        async listen(callback: (message: MappingMessage) => void) {
            const listener = (event: MessageEvent<Message>) => {
                if (event.data.direction !== 'state' || event.data.message.state.role !== role) return;
                received = event.data.message;
                callback(event.data.message);
            };
            channel.addEventListener('message', listener);
            return () => channel.removeEventListener('message', listener);
        },
    };
    onMount(() => {
        let mounted = true;
        if (!role)
            void (async () => {
                for (const controller of controllers)
                    await controller.connect({
                        async open() {
                            const url = new URL(location.href);
                            url.searchParams.set('role', controller.role);
                            window.open(url, `mapping-${controller.role}-fixture`, 'popup,width=1040,height=680');
                        },
                        async publish(message) {
                            post({ direction: 'state', message });
                        },
                        async listen(callback) {
                            const listener = (event: MessageEvent<Message>) => {
                                if (event.data.direction === 'command') callback(event.data.command);
                            };
                            channel.addEventListener('message', listener);
                            return () => channel.removeEventListener('message', listener);
                        },
                    });
                const documents = await Promise.all(['bank', 'a', 'b'].map((id) => workflow.load(1, id)));
                if (mounted) {
                    [bank, a, b] = documents;
                    bank!.previewMemberId = 'a';
                }
            })();
        return () => {
            mounted = false;
            controllers.forEach((controller) => controller.dispose());
            channel.close();
        };
    });
</script>

{#if role}
    <MappingWindow {role} {adapter} />
    <output hidden data-mapping-snapshot>{JSON.stringify(received)}</output>
{:else}
    <main class="device-editor fixture">
        {#if bank && a && b}
            <h1>Sample Bank</h1>
            <BankPreview document={bank} onready={() => {}} />
            <SampleMapping document={bank} disabled={false} />
            <h1>Sample</h1>
            <SampleMapping document={a} disabled={false} />
            <button class="editor-action" onclick={() => bank!.draft.set('level', 80)}>Change bank level</button>
        {/if}
    </main>
    <output hidden data-bank-main
        >{JSON.stringify({
            ready: !!bank && controllers.every((controller) => controller.available),
            writes,
            bank: bank?.draft.changes,
            a: a?.draft.changes,
            b: b?.draft.changes,
            aValues: a?.draft.values,
            bValues: b?.draft.values,
        })}</output
    >
{/if}

<style>
    .fixture {
        height: 100dvh;
        background: var(--color-panel);
        padding: 12px;
        color: var(--color-text);
    }
    h1 {
        font-size: 13px;
        margin: 8px 0;
    }
</style>

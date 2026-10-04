<script lang="ts">
    import '../object-editor/editor.css';
    import { onMount } from 'svelte';
    import KeyboardMapping from '../object-editor/KeyboardMapping.svelte';
    import EditorChoice from '../object-editor/EditorChoice.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { noteName } from '../devices/a-series/sample/geometry';
    import type { KeyboardRange } from '../object-editor/keyboardMapping';
    import type { MappingAction, MappingCommand, MappingMessage, MappingSnapshot } from './protocol';
    let {
        adapter,
    }: {
        adapter: {
            send(command: MappingCommand): Promise<void>;
            listen(callback: (message: MappingMessage) => void): Promise<() => void>;
        };
    } = $props();
    let snapshot = $state.raw<MappingSnapshot | null>(null);
    let status = $state('Connecting to the Program editor');
    let pending = $state('');
    let preview = $state<KeyboardRange | null>(null);
    let gesture: { context: string; version: number; assignmentId: number } | null = null;
    let timeout: ReturnType<typeof setTimeout> | undefined;
    let mounted = true;
    const retiredOwners = new Set<string>();
    async function send(action: MappingAction, identity = snapshot) {
        if (pending) return;
        const requestId = crypto.randomUUID();
        pending = requestId;
        status = '';
        timeout = setTimeout(() => {
            if (pending === requestId) {
                pending = '';
                status = 'The main editor did not respond. Refresh to check its current snapshot.';
            }
        }, 10000);
        try {
            await adapter.send({
                requestId,
                context: identity?.context ?? '',
                version: identity?.version ?? 0,
                action,
            });
        } catch (error) {
            if (mounted && pending === requestId) {
                clearTimeout(timeout);
                pending = '';
                status = String(error);
            }
        }
    }
    function begin() {
        if (snapshot?.editable && snapshot.assignmentId !== null && !pending) {
            gesture = { context: snapshot.context, version: snapshot.version, assignmentId: snapshot.assignmentId };
            preview = snapshot.limits ? { ...snapshot.limits } : null;
        }
    }
    function end() {
        const range = preview,
            identity = gesture;
        preview = null;
        gesture = null;
        if (
            range &&
            identity &&
            snapshot &&
            identity.context === snapshot.context &&
            identity.version === snapshot.version
        )
            void send({ kind: 'range', assignmentId: identity.assignmentId, range });
    }
    onMount(() => {
        mounted = true;
        let stop: (() => void) | undefined;
        void adapter
            .listen((message) => {
                if (!mounted) return;
                if (
                    !retiredOwners.has(message.state.owner) &&
                    (!snapshot || message.state.owner !== snapshot.owner || message.state.version >= snapshot.version)
                ) {
                    if (snapshot && message.state.owner !== snapshot.owner) retiredOwners.add(snapshot.owner);
                    if (
                        gesture &&
                        (gesture.context !== message.state.context || gesture.version !== message.state.version)
                    ) {
                        gesture = null;
                        preview = null;
                    }
                    snapshot = message.state;
                }
                if (message.requestId === pending) {
                    clearTimeout(timeout);
                    pending = '';
                    status = message.error ?? '';
                }
            })
            .then((dispose) => {
                if (!mounted) {
                    dispose();
                    return;
                }
                stop = dispose;
                void send({ kind: 'ready' });
            })
            .catch((error) => {
                if (mounted) status = String(error);
            });
        const heartbeat = setInterval(() => {
            if (!gesture)
                void adapter
                    .send({
                        requestId: crypto.randomUUID(),
                        context: snapshot?.context ?? '',
                        version: snapshot?.version ?? 0,
                        action: { kind: 'ready' },
                    })
                    .catch((error) => {
                        if (mounted) status = String(error);
                    });
        }, 5000);
        return () => {
            mounted = false;
            gesture = null;
            preview = null;
            clearTimeout(timeout);
            clearInterval(heartbeat);
            stop?.();
        };
    });
</script>

<main class="device-editor mapping-window">
    <header>
        <h1>Mapping Editor{snapshot?.title ? `: ${snapshot.title}` : ''}</h1>
        <div class="actions">
            <button
                class="editor-icon"
                title="Undo"
                aria-label="Undo"
                disabled={!!pending || !snapshot?.canUndo}
                onclick={() => void send({ kind: 'undo' })}><Icon name="undo" size={14} /></button
            >
            <button
                class="editor-icon"
                title="Redo"
                aria-label="Redo"
                disabled={!!pending || !snapshot?.canRedo}
                onclick={() => void send({ kind: 'redo' })}><Icon name="redo" size={14} /></button
            >
            <button
                class="action"
                disabled={!!pending || !snapshot?.canDiscard}
                onclick={() => void send({ kind: 'discard' })}>Discard</button
            >
            {#if snapshot?.recovery}<button
                    class="action"
                    disabled={!!pending}
                    onclick={() => void send({ kind: 'recover' })}
                    ><Icon name="refresh" size={14} />{snapshot.recovery}</button
                >
            {:else}<button
                    class="action"
                    disabled={!!pending || !snapshot?.canSave}
                    onclick={() => void send({ kind: 'save' })}><Icon name="save" size={14} />Save</button
                >{/if}
        </div>
    </header>
    <div class="assignment">
        <span>Sample/Bank</span>
        <EditorChoice
            label="Sample/Bank"
            value={snapshot?.assignmentId ?? undefined}
            options={snapshot?.assignments ?? []}
            segmented={false}
            disabled={!!pending || !snapshot?.assignments.length}
            onchange={(assignmentId) => void send({ kind: 'select', assignmentId })}
        />
    </div>
    <div class="canvas">
        {#if snapshot?.limits}
            {#key snapshot.context}
                <KeyboardMapping
                    mode="mapping"
                    zones={snapshot.zones}
                    limits={preview ?? snapshot.limits}
                    formatNote={noteName}
                    disabled={!!pending || !snapshot.editable}
                    onselect={(id) => void send({ kind: 'select', assignmentId: Number(id.split(':')[0]) })}
                    onbegin={begin}
                    onchange={(range) => {
                        if (gesture) preview = range;
                    }}
                    onend={end}
                />
            {/key}
        {:else}<p>{snapshot?.status ?? status}</p>{/if}
    </div>
    <footer>
        <span role="status">{status || (pending ? 'Updating' : snapshot?.status)}</span>
        <button
            class="editor-icon"
            title="Refresh mapping"
            aria-label="Refresh mapping"
            disabled={!!pending}
            onclick={() => void send({ kind: 'ready' })}><Icon name="refresh" size={14} /></button
        >
    </footer>
</main>

<style>
    .mapping-window {
        display: flex;
        flex-direction: column;
        height: 100dvh;
        background: var(--color-panel);
        color: var(--color-text);
        font-size: 11px;
    }
    header {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 6px 10px;
        border-bottom: 1px solid var(--color-border);
    }
    h1 {
        flex: 1;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        font-size: 13px;
        font-weight: 600;
    }
    .actions {
        display: flex;
        gap: 5px;
        align-items: center;
    }
    .action {
        display: inline-flex;
        gap: 6px;
        align-items: center;
        height: 26px;
        padding: 0 8px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
    }
    .assignment {
        display: grid;
        grid-template-columns: 70px minmax(0, 350px);
        align-items: center;
        padding: 8px 10px 0;
    }
    .assignment span {
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .canvas {
        flex: 1;
        min-height: 0;
        overflow: auto;
        padding: 0 10px 10px;
    }
    footer {
        display: flex;
        align-items: center;
        height: 30px;
        border-top: 1px solid var(--color-border);
        padding: 0 10px;
        color: var(--color-text-muted);
    }
    footer span {
        flex: 1;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
</style>

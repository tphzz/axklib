<script lang="ts">
    import '../object-editor/editor.css';
    import { onMount, untrack } from 'svelte';
    import KeyboardMapping from '../object-editor/KeyboardMapping.svelte';
    import EditorChoice from '../object-editor/EditorChoice.svelte';
    import EditorNumber from '../object-editor/EditorNumber.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { noteName } from '../devices/a-series/sample/geometry';
    import { MappingClient, type MappingWindowAdapter } from './client.svelte';
    import type { MappingRole } from './protocol';
    let {
        adapter,
        role = 'program',
    }: {
        adapter: MappingWindowAdapter;
        role?: MappingRole;
    } = $props();
    const client = untrack(() => new MappingClient(role, adapter));
    const snapshot = $derived(client.state);
    const status = $derived(client.status);
    const pending = $derived(client.locked);
    const targets = $derived(new Map(snapshot?.targets.map((row) => [row.id, row])));
    const send = (action: Parameters<typeof client.send>[0]) => client.send(action);
    let inputErrors = $state<Record<string, string>>({});
    let velocity = $state(100);
    $effect(() => {
        snapshot?.context;
        snapshot?.selectionId;
        inputErrors = {};
    });
    onMount(() => {
        let closed = false,
            stop: (() => void) | undefined;
        void client
            .connect()
            .then((dispose) => {
                if (closed) dispose();
                else stop = dispose;
            })
            .catch((error) => {
                client.status = String(error);
            });
        return () => {
            closed = true;
            stop?.();
        };
    });
</script>

<svelte:window onblur={() => client.release()} />

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
                    disabled={!!pending || !snapshot?.canSave || Object.values(inputErrors).some(Boolean)}
                    onclick={() => void send({ kind: 'save' })}><Icon name="save" size={14} />Save</button
                >{/if}
        </div>
    </header>
    {#if snapshot?.selections.length}<div class="assignment">
            <span>{snapshot.selectionLabel}</span>
            <EditorChoice
                label={snapshot.selectionLabel}
                value={snapshot.selectionId ?? undefined}
                options={snapshot.selections}
                segmented={false}
                disabled={!!pending}
                onchange={(selectionId) => client.select(selectionId)}
            />
        </div>{/if}
    {#if snapshot?.overrides.length}<div class="overrides">
            {#each snapshot.overrides as control (control.boundary)}
                <div class="override-control">
                    <span>{control.label}</span>
                    <EditorNumber
                        label={control.label}
                        value={snapshot.limits?.[control.boundary]}
                        min={control.boundary === 'velocityHigh' ? (snapshot.limits?.velocityLow ?? 0) : 0}
                        max={control.boundary === 'velocityLow' ? (snapshot.limits?.velocityHigh ?? 127) : 127}
                        disabled={!!pending || !snapshot.editable}
                        inherited={control.inherited}
                        oninvalid={(message) => (inputErrors[control.boundary] = message)}
                        onchange={(value) => {
                            if (snapshot?.limits && snapshot.selectionId !== null)
                                void send({
                                    kind: 'range',
                                    selectionId: snapshot.selectionId,
                                    range: { ...snapshot.limits, [control.boundary]: value },
                                    boundaries: [control.boundary],
                                });
                        }}
                    />
                    <button
                        class="editor-icon"
                        aria-label={`Inherit ${control.label.toLowerCase()}`}
                        title={`Use the Sample's ${control.label.toLowerCase()}`}
                        disabled={!!pending || !snapshot.editable || control.inherited}
                        onclick={() => void send({ kind: 'inherit', boundary: control.boundary })}
                        ><Icon name="undo" size={14} /></button
                    >
                </div>
            {/each}
        </div>{/if}
    <div class="canvas">
        {#if snapshot?.limits}
            {#key snapshot.context}
                <KeyboardMapping
                    mode="mapping"
                    {velocity}
                    onvelocity={(value) => {
                        client.release();
                        velocity = value;
                    }}
                    onpress={(note) => client.press(note, velocity)}
                    onrelease={() => client.release()}
                    zones={snapshot.zones}
                    limits={client.preview ?? snapshot.limits}
                    targets={snapshot.zones.flatMap((zone) => {
                        const target = targets.get(zone.selectionId!);
                        return target?.limits
                            ? [
                                  {
                                      id: zone.id,
                                      limits: target.limits,
                                      axes: target.editableAxes,
                                      editable: target.editable,
                                  },
                              ]
                            : [];
                    })}
                    editableAxes={snapshot.editableAxes}
                    rangeLabel={snapshot.rangeLabel}
                    rootEditable={snapshot.rootEditable}
                    rootIdentity={`${snapshot.context}:${snapshot.version}:${snapshot.selectionId}`}
                    onroot={(note) => {
                        if (snapshot?.selectionId !== null && snapshot)
                            void send({ kind: 'root', selectionId: snapshot.selectionId, note });
                    }}
                    formatNote={noteName}
                    disabled={!!pending || !snapshot.editable}
                    onselect={(id) => {
                        const selectionId = snapshot?.zones.find((zone) => zone.id === id)?.selectionId;
                        if (selectionId !== undefined && snapshot?.selections.length) client.select(selectionId);
                    }}
                    onbegin={(id) => client.begin(id)}
                    onchange={(range, changed) => client.change(range, changed)}
                    onend={(cancelled, move) => client.end(cancelled, move)}
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
    .overrides {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
        gap: 8px 18px;
        padding: 8px 10px 0;
    }
    .override-control {
        display: grid;
        grid-template-columns: 80px minmax(0, 1fr) 24px;
        align-items: center;
        gap: 6px;
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

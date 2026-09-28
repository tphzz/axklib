<script lang="ts">
    import './editor.css';
    import { onDestroy, untrack } from 'svelte';
    import { measureWidth } from './measureWidth';
    import { provideEditorWidth } from './editorWidth';
    import type { InspectorSelection } from '../../lib/types';
    import { objectEditors } from './context';
    import { objectEditorAdapter } from './registry';
    import type { ObjectEditorDocument } from './workflow.svelte';
    import SampleEditor from '../devices/a-series/sample/SampleEditor.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import SampleFormatBadge from './SampleFormatBadge.svelte';
    import { sampleConversionTitle } from '../../lib/sampleFormatLabels';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import SampleNavigation from '../devices/a-series/sample/SampleNavigation.svelte';
    let { sessionId, selection }: { sessionId: number | null; selection: InspectorSelection } = $props();
    const editors = objectEditors();
    const panelId = $props.id();
    let width = $state(0);
    provideEditorWidth(() => Math.max(0, width - 16));
    let document = $state<ObjectEditorDocument | null>(null);
    let message = $state('');
    let pending = $state(false);
    let previousPreview = $state<import('../../lib/types').SampleWaveformPreview>();
    let resolved = $state.raw<ObjectEditorDocument | null>(null);
    const editing = $derived(document?.detail && objectEditorAdapter(document.detail) ? document.detail.editing : null);
    const navigation = $derived(editing && editors?.navigation(editing.profile));
    const sampleId = $derived(
        selection?.kind === 'sample' || selection?.kind === 'sample-bank' ? selection.item.objectId : null,
    );
    const matches = $derived(document?.sessionId === sessionId && document?.detail?.object.id === sampleId);
    const inactive = $derived(pending || !matches);
    const displayedPreview = $derived(matches && selection?.kind === 'sample' ? selection.preview : previousPreview);
    const conversionTitle = $derived(
        sampleConversionTitle(document?.detail?.formatConversion?.formatConversions[0]?.targetFormat),
    );
    $effect(() => {
        if (editors) editors.visible = !!navigation;
    });
    onDestroy(() => {
        if (editors) editors.visible = false;
    });
    $effect(() => {
        const id = sampleId;
        const session = sessionId;
        let current = true;
        resolved = null;
        if (!editors || session === null || !id) {
            document = null;
            previousPreview = undefined;
            pending = false;
            message = '';
            return;
        }
        pending = true;
        message = 'Loading Sample parameters';
        if (editors && session !== null && id)
            void untrack(() => editors.load(session, id))
                .then((value) => {
                    if (!current) return;
                    resolved = value;
                    message =
                        value?.detail && objectEditorAdapter(value.detail)
                            ? ''
                            : 'Editing is not available for this Sample format';
                    if (!value?.detail || !objectEditorAdapter(value.detail)) {
                        resolved = null;
                        pending = false;
                        document = null;
                    }
                })
                .catch((error) => {
                    if (current) {
                        pending = false;
                        message = userFacingMessage(error);
                    }
                });
        return () => {
            current = false;
        };
    });
    $effect(() => {
        if (!resolved || resolved.detail?.object.id !== sampleId) return;
        if (
            navigation?.page === 'waveform' &&
            document &&
            selection?.kind === 'sample' &&
            selection.preview.previewState === 'loading'
        ) {
            message = 'Loading waveform';
            return;
        }
        document = resolved;
        previousPreview = selection?.kind === 'sample' ? selection.preview : undefined;
        pending = false;
        message = '';
    });
    $effect(() => {
        if (matches && selection?.kind === 'sample') previousPreview = selection.preview;
    });
    function inertEditor(node: HTMLElement, value: boolean) {
        node.toggleAttribute('inert', value);
        return { update: (next: boolean) => node.toggleAttribute('inert', next) };
    }
</script>

<section
    class="device-editor"
    aria-label="Sample editor"
    aria-busy={pending}
    use:measureWidth={{ scope: 'editor', change: (value) => (width = value) }}
>
    {#if document && editors && navigation}
        <div class="editor-content" use:inertEditor={inactive} aria-hidden={inactive ? 'true' : undefined}>
            <header>
                <span class="family">A-series</span>
                <strong
                    class:dirty={document.draft.dirty}
                    title={`${document.detail?.object.name}${document.draft.dirty ? ' (unsaved changes)' : ''}`}
                    >{document.detail?.object.name}</strong
                >
                <SampleFormatBadge format={document.detail!.editing!.sampleFormat} />
                <div class="navigation"><SampleNavigation {navigation} {panelId} /></div>
                {#if editors.comparison.count > 1}<span
                        class="family"
                        role="status"
                        title={`Editing ${document.detail?.object.name} only`}>{editors.comparison.status}</span
                    >{/if}
                <div class="actions">
                    <button
                        class="icon-button"
                        title={conversionTitle}
                        aria-label={conversionTitle}
                        disabled={editors.locked || !document.detail?.formatConversion}
                        onclick={() => void editors.openConversion(document!.sessionId, document!.detail!.object.id)}
                        ><Icon name="refresh" size={14} /></button
                    >
                    <button
                        class="icon-button"
                        title="Undo"
                        aria-label="Undo Sample edit"
                        disabled={!document.draft.canUndo || document.phase !== 'editable'}
                        onclick={() => document?.draft.undo()}><Icon name="undo" size={14} /></button
                    >
                    <button
                        class="icon-button"
                        title="Redo"
                        aria-label="Redo Sample edit"
                        disabled={!document.draft.canRedo || document.phase !== 'editable'}
                        onclick={() => document?.draft.redo()}><Icon name="redo" size={14} /></button
                    >
                    <button
                        class="editor-action"
                        disabled={(!document.draft.dirty && !document.conflict) || document.phase !== 'editable'}
                        onclick={() =>
                            void editors.discard(document!).catch((error) => {
                                document!.status = userFacingMessage(error);
                            })}>Discard</button
                    >
                    {#if document.phase === 'refresh-failed' || (document.phase === 'unconfirmed' && document.jobId !== null)}
                        <button class="editor-action" onclick={() => void editors.recover(document!)}
                            ><Icon name="refresh" size={14} />{document.phase === 'refresh-failed'
                                ? 'Refresh'
                                : 'Check status'}</button
                        >
                    {:else}
                        <button
                            class="editor-action save"
                            disabled={!document.canSave || editors.locked}
                            onclick={() => void editors.save(document!)}><Icon name="save" size={14} />Save</button
                        >
                    {/if}
                </div>
            </header>
            {#key document}
                <SampleEditor {document} {navigation} {panelId} preview={displayedPreview} {inactive} />
            {/key}
        </div>
        {#if inactive}<div class="transition-status" role="status">{message}</div>{/if}
    {:else}<p role="status">{message}</p>{/if}
</section>

<style>
    .device-editor {
        position: relative;
        height: 100%;
        min-height: 0;
        display: flex;
        flex-direction: column;
        overflow: hidden;
        background: var(--color-panel);
        font-size: 11px;
    }
    .editor-content {
        display: flex;
        flex-direction: column;
        flex: 1;
        min-height: 0;
    }
    .transition-status {
        position: absolute;
        bottom: 6px;
        right: 8px;
        max-width: calc(100% - 16px);
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        padding: 2px 6px;
        background: var(--color-panel-raised);
        color: var(--color-text-muted);
    }
    header {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 8px;
        padding: 0 8px;
        border-bottom: 1px solid var(--color-border);
        flex: 0 0 auto;
        min-height: 38px;
    }
    strong {
        position: relative;
        padding-right: 12px;
        max-width: 200px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
    }
    strong::after {
        content: '*';
        position: absolute;
        right: 0;
        width: 8px;
        visibility: hidden;
    }
    strong.dirty::after {
        visibility: visible;
    }
    .family,
    p {
        color: var(--color-text-muted);
    }
    .family {
        font-size: 10px;
        white-space: nowrap;
    }
    .navigation {
        flex: 1;
        min-width: 0;
    }
    .actions {
        display: flex;
        align-items: center;
        gap: 5px;
        margin-left: auto;
        padding-block: 4px;
    }
    :global([data-editor-under~='850']) .navigation {
        order: 1;
        flex-basis: 100%;
        border-top: 1px solid var(--color-border);
    }
    :global([data-editor-under~='850']) header {
        gap: 0 8px;
    }
    :global([data-editor-under~='500']) strong {
        max-width: 120px;
    }
    :global([data-editor-under~='500']) header {
        gap: 5px;
    }
    :global([data-editor-under~='500']) .family {
        display: none;
    }
    .editor-action {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        border: 1px solid var(--color-border);
        border-radius: 4px;
        padding: 4px 9px;
        height: 28px;
        white-space: nowrap;
    }
    .save {
        border-color: var(--color-accent);
    }
    button:disabled {
        opacity: 0.4;
    }
    p {
        padding: 12px;
    }
</style>

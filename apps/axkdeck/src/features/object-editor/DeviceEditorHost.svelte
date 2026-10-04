<script lang="ts">
    import './editor.css';
    import { onDestroy, untrack, type Snippet } from 'svelte';
    import { measureWidth } from './measureWidth';
    import { provideEditorWidth } from './editorWidth';
    import type { InspectorSelection, ProgramSampleSelectRow } from '../../lib/types';
    import { objectEditors } from './context';
    import { objectEditorAdapter } from './registry';
    import type { ObjectEditorDocument } from './workflow.svelte';
    import SampleEditor from '../devices/a-series/sample/SampleEditor.svelte';
    import ProgramEditor from '../devices/a-series/program/ProgramEditor.svelte';
    import { programTabs } from '../devices/a-series/program/fields';
    import EditorTabs from '../../lib/components/EditorTabs.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import EditorHeader from '../../lib/components/EditorHeader.svelte';
    import { sampleConversionTitle } from '../../lib/sampleFormatLabels';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import SampleNavigation from '../devices/a-series/sample/SampleNavigation.svelte';
    let {
        sessionId,
        selection,
        assignmentQuery = '',
        onassignmentquerychange = () => {},
        onassignmentselect = () => {},
        multiPartContext = null,
        fallback,
    }: {
        sessionId: number | null;
        selection: InspectorSelection;
        assignmentQuery?: string;
        onassignmentquerychange?: (value: string) => void;
        onassignmentselect?: (row: ProgramSampleSelectRow) => void;
        multiPartContext?: { partLabel: string; programNumber: number } | null;
        fallback?: Snippet;
    } = $props();
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
        selection?.kind === 'sample' || selection?.kind === 'sample-bank'
            ? selection.item.objectId
            : selection?.kind === 'program'
              ? selection.program.objectId
              : null,
    );
    const matches = $derived(document?.sessionId === sessionId && document?.detail?.object.id === sampleId);
    const inactive = $derived(pending || !matches);
    const displayedPreview = $derived(matches && selection?.kind === 'sample' ? selection.preview : previousPreview);
    const conversion = $derived(document?.detail?.formatConversion);
    const conversionTitle = $derived(
        conversion?.kind === 'PROGRAM'
            ? 'Program format conversion'
            : sampleConversionTitle(conversion?.formatConversions[0]?.targetFormat),
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
        message = `Loading ${selection?.kind === 'program' ? 'Program' : 'Sample'} parameters`;
        if (editors && session !== null && id)
            void untrack(() => editors.load(session, id))
                .then((value) => {
                    if (!current) return;
                    resolved = value;
                    message =
                        value?.detail && objectEditorAdapter(value.detail)
                            ? ''
                            : 'Editing is not available for this object format';
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
    aria-label={selection?.kind === 'program' ? 'Program editor' : 'Sample editor'}
    aria-busy={pending}
    use:measureWidth={{ scope: 'editor', change: (value) => (width = value) }}
>
    {#if document && editors && navigation}
        {@const current = document}
        <div
            class="editor-content"
            role="group"
            aria-label={`${document.noun}: ${document.detail?.object.name}${document.draft.dirty ? ' (unsaved changes)' : ''}`}
            use:inertEditor={inactive}
            aria-hidden={inactive ? 'true' : undefined}
        >
            <EditorHeader>
                {#if editing?.profile === 'a-series/program'}
                    <EditorTabs
                        tabs={programTabs}
                        active={navigation.tab}
                        onselect={(id) => navigation.selectTab(id)}
                        label="Program parameter tabs"
                        idPrefix={panelId}
                        {panelId}
                    />
                {:else}<SampleNavigation {navigation} {panelId} />{/if}
                {#snippet status()}
                    {#if editing?.profile !== 'a-series/program' && editors.comparison.count > 1}<span
                            class="comparison"
                            role="status"
                            title={`Editing ${current.detail?.object.name} only`}>{editors.comparison.status}</span
                        >{/if}
                {/snippet}
                {#snippet tools()}
                    <div class="actions">
                        <button
                            class="icon-button"
                            title={conversionTitle}
                            aria-label={conversionTitle}
                            disabled={editors.locked || !current.detail?.formatConversion}
                            onclick={() => void editors.openConversion(current.sessionId, current.detail!.object.id)}
                            ><Icon name="refresh" size={14} /></button
                        >
                        <button
                            class="icon-button"
                            title="Undo"
                            aria-label={`Undo ${current.noun} edit`}
                            disabled={!current.draft.canUndo || current.phase !== 'editable'}
                            onclick={() => current.draft.undo()}><Icon name="undo" size={14} /></button
                        >
                        <button
                            class="icon-button"
                            title="Redo"
                            aria-label={`Redo ${current.noun} edit`}
                            disabled={!current.draft.canRedo || current.phase !== 'editable'}
                            onclick={() => current.draft.redo()}><Icon name="redo" size={14} /></button
                        >
                        <button
                            class="editor-action"
                            title="Discard"
                            aria-label="Discard"
                            disabled={(!current.draft.dirty && !current.conflict) || current.phase !== 'editable'}
                            onclick={() =>
                                void editors.discard(current).catch((error) => {
                                    current.status = userFacingMessage(error);
                                })}
                            ><span class="compact-icon"><Icon name="close" size={14} /></span><span class="action-label"
                                >Discard</span
                            ></button
                        >
                        {#if current.phase === 'refresh-failed' || (current.phase === 'unconfirmed' && current.jobId !== null)}
                            <button
                                class="editor-action"
                                title={current.phase === 'refresh-failed' ? 'Refresh' : 'Check status'}
                                aria-label={current.phase === 'refresh-failed' ? 'Refresh' : 'Check status'}
                                onclick={() => void editors.recover(current)}
                                ><Icon name="refresh" size={14} /><span class="action-label"
                                    >{current.phase === 'refresh-failed' ? 'Refresh' : 'Check status'}</span
                                ></button
                            >
                        {:else}
                            <button
                                class="editor-action save"
                                title="Save"
                                aria-label="Save"
                                disabled={!current.canSave || editors.locked}
                                onclick={() => void editors.save(current)}
                                ><Icon name="save" size={14} /><span class="action-label">Save</span></button
                            >
                        {/if}
                    </div>
                {/snippet}
            </EditorHeader>
            {#key document}
                {#if editing?.profile === 'a-series/program'}
                    <ProgramEditor
                        {document}
                        {navigation}
                        {panelId}
                        {inactive}
                        {selection}
                        {assignmentQuery}
                        {onassignmentquerychange}
                        {onassignmentselect}
                        {multiPartContext}
                    />
                {:else}<SampleEditor {document} {navigation} {panelId} preview={displayedPreview} {inactive} />{/if}
            {/key}
        </div>
        {#if inactive}<div class="transition-status" role="status">{message}</div>{/if}
    {:else if !pending && fallback}{@render fallback()}
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
    .comparison,
    p {
        color: var(--color-text-muted);
    }
    .comparison {
        display: block;
        min-width: 0;
        max-width: 140px;
        overflow: hidden;
        text-overflow: ellipsis;
        font-size: 10px;
        white-space: nowrap;
    }
    .actions {
        display: flex;
        flex: 0 0 auto;
        align-items: center;
        gap: 5px;
        margin-left: auto;
    }
    .editor-action {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        border: 1px solid var(--color-border);
        border-radius: 4px;
        padding: 0 9px;
        height: var(--density-control);
        font-size: 11px;
        white-space: nowrap;
    }
    .save {
        border-color: var(--color-accent);
    }
    .compact-icon {
        display: none;
    }
    .device-editor:global([data-editor-under~='420']) .actions {
        gap: 3px;
    }
    .device-editor:global([data-editor-under~='420']) .actions button {
        width: 26px;
        min-width: 26px;
        padding: 0;
        justify-content: center;
    }
    .device-editor:global([data-editor-under~='420']) .action-label {
        display: none;
    }
    .device-editor:global([data-editor-under~='420']) .compact-icon {
        display: flex;
    }
    button:disabled {
        opacity: 0.4;
    }
    p {
        padding: 12px;
    }
</style>

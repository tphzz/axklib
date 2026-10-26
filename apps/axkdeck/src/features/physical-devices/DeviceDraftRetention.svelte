<script lang="ts">
    import SelectControl from '../../lib/components/SelectControl.svelte';
    import { onMount, untrack } from 'svelte';
    import type { ObjectEditorWorkflow } from '../object-editor/workflow.svelte';
    import type { ImageSessionWorkflow } from '../image-session/workflow.svelte';
    import { DeviceDraftStore, type RetainedDeviceDraft } from './drafts';
    import { modal } from '../../lib/modal';
    import { userFacingMessage } from '../../lib/userFacingMessage';
    import type { DeviceLocation } from '../../lib/storageLocations';
    let { editors, images }: { editors: ObjectEditorWorkflow; images: ImageSessionWorkflow } = $props();
    let store = $state.raw<DeviceDraftStore>();
    let records = $state.raw<RetainedDeviceDraft[]>([]);
    let visible = $state(false);
    let selected = $state('');
    let status = $state('');
    const record = $derived(records.find((item) => item.id === selected));
    const location = $derived(images.location?.kind === 'device' ? images.location : null);
    const matching = $derived(record && store?.matching(record, location, editors.documents));
    let timer: ReturnType<typeof setTimeout> | undefined;
    let capturedLocation: DeviceLocation | null = null;
    function persist() {
        if (!store || !capturedLocation) return;
        try {
            store.sync(capturedLocation, editors.documents);
            records = store.list();
        } catch (error) {
            records = store.list();
            status = `${userFacingMessage(error)} Latest drafts are held in memory; export them before closing axkdeck.`;
            images.setStatus(`Device draft retention failed: ${status}`);
        }
    }
    $effect(() => {
        location;
        editors.documents.forEach((document) => {
            document.draft.revision;
            document.detail;
            document.phase;
        });
        untrack(() => {
            capturedLocation = location;
            clearTimeout(timer);
            timer = setTimeout(persist, 200);
        });
        return () => clearTimeout(timer);
    });
    onMount(() => {
        try {
            store = new DeviceDraftStore(window.localStorage);
            records = store.list();
        } catch (error) {
            store = new DeviceDraftStore(null);
            status = `${userFacingMessage(error)} New drafts will be held in memory; export them before closing axkdeck.`;
            images.setStatus(status);
        }
        editors.beforeClear = persist;
        window.addEventListener('beforeunload', persist);
        const visibility = () => {
            if (document.visibilityState === 'hidden') persist();
        };
        document.addEventListener('visibilitychange', visibility);
        return () => {
            clearTimeout(timer);
            persist();
            editors.beforeClear = undefined;
            window.removeEventListener('beforeunload', persist);
            document.removeEventListener('visibilitychange', visibility);
        };
    });
    function restore() {
        if (!record || !store || !location) return;
        try {
            store.restore(record, location, editors.documents);
            status = 'Draft restored locally; no device write was submitted.';
        } catch (error) {
            status = userFacingMessage(error);
        }
    }
    function discard() {
        if (!record || !store) return;
        try {
            store.discard(record.id);
            records = store.list();
            selected = '';
            status = '';
        } catch (error) {
            status = userFacingMessage(error);
            records = store.list();
            selected = '';
        }
    }
    function exportJson() {
        if (!record) return;
        const url = URL.createObjectURL(
            new Blob([JSON.stringify(record, null, 2) + '\n'], { type: 'application/json' }),
        );
        const link = document.createElement('a');
        link.href = url;
        link.download = 'device-parameter-draft.json';
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 0);
    }
</script>

{#if records.length || status}
    <div class="retained-drafts">
        <button
            class="secondary-button"
            onclick={() => {
                persist();
                visible = true;
            }}>Retained device drafts ({records.length})</button
        >
    </div>
{/if}
{#if visible}
    <div class="dialog-backdrop dialog-backdrop-top" role="presentation">
        <div
            class="dialog-shell retained-dialog"
            role="dialog"
            aria-modal="true"
            aria-labelledby="retained-drafts-title"
            use:modal={{ onescape: () => (visible = false) }}
        >
            <header class="dialog-header"><h2 id="retained-drafts-title">Retained device drafts</h2></header>
            <div class="retained-content">
                <label
                    >Draft<SelectControl
                        label="Draft"
                        value={selected}
                        onchange={(value) => (selected = value)}
                        options={[
                            { value: '', label: 'Select a retained draft' },
                            ...records.map((item) => ({ value: item.id, label: `${item.name} / ${item.device}` })),
                        ]}
                    /></label
                >
                {#if record}
                    <dl>
                        <dt>Device</dt>
                        <dd>{record.device}</dd>
                        <dt>Target</dt>
                        <dd>{record.target}</dd>
                        <dt>Base revision</dt>
                        <dd>{record.revision}</dd>
                        <dt>Base payload</dt>
                        <dd><code>{record.payloadSha256}</code></dd>
                    </dl>
                    {#if record.membershipChanged}<p class="dialog-warning">
                            Assignment membership changes are retained for manual JSON review.
                        </p>
                    {:else if !matching}<p class="dialog-warning">
                            The original object and unchanged base payload are not open.
                        </p>{/if}
                {/if}
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status" title={status}>{status}</span>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" disabled={!record} onclick={exportJson}>Export JSON</button>
                    <button class="danger-button" disabled={!record} onclick={discard}>Discard</button>
                    <button class="secondary-button" onclick={() => (visible = false)}>Close</button>
                    <button
                        class="primary-button"
                        disabled={!record ||
                            !matching ||
                            matching.draft.dirty ||
                            matching.phase !== 'editable' ||
                            record.membershipChanged}
                        onclick={restore}>Restore draft</button
                    >
                </div>
            </footer>
        </div>
    </div>
{/if}

<style>
    .retained-drafts {
        padding: 4px 8px;
        border-bottom: 1px solid var(--color-border);
    }
    .retained-dialog {
        width: min(640px, calc(100vw - 32px));
        max-height: calc(100vh - 32px);
    }
    .retained-content {
        padding: 12px;
        overflow: auto;
        font-size: var(--dialog-body-font-size);
    }
    label {
        display: grid;
        grid-template-columns: 70px minmax(0, 1fr);
        align-items: center;
        gap: 8px;
        font-size: var(--dialog-label-font-size);
    }
    dl {
        display: grid;
        grid-template-columns: 90px minmax(0, 1fr);
        gap: 8px;
    }
    dt {
        font-size: var(--dialog-label-font-size);
    }
    dd {
        margin: 0;
        overflow-wrap: anywhere;
    }
</style>

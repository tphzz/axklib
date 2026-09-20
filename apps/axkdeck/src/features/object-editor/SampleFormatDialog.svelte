<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import SampleFormatBadge from './SampleFormatBadge.svelte';
    import { sampleConversionTitle } from '../../lib/sampleFormatLabels';
    import type { ObjectEditorWorkflow, ObjectEditorDocument } from './workflow.svelte';
    let { workflow, document }: { workflow: ObjectEditorWorkflow; document: ObjectEditorDocument } = $props();
    const snapshot = $derived(document.detail!.editing!);
    const preview = $derived(snapshot.formatConversions[0]);
    const target = $derived(preview?.targetFormat);
    const title = $derived(sampleConversionTitle(target));
    const reason = $derived(workflow.conversionReason(document));
    const locked = $derived(document.phase !== 'editable');
    const recovery = $derived(
        document.phase === 'refresh-failed' || (document.phase === 'unconfirmed' && document.jobId !== null),
    );
    const canSubmit = $derived(!reason && preview?.allowed && !workflow.locked && !!target);
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell format-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: () => workflow.closeConversion() }}
    >
        <form
            onsubmit={(event) => {
                event.preventDefault();
                if (recovery) void workflow.recover(document);
                else if (canSubmit && target) void workflow.convert(document, target);
            }}
        >
            <header class="dialog-header">
                <h2>{title}</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={locked}
                    onclick={() => workflow.closeConversion()}><Icon name="close" size={15} /></button
                >
            </header>
            <div class="format-summary">
                <strong>{document.detail!.object.name}</strong><SampleFormatBadge format={snapshot.sampleFormat} />
                <p>
                    {target === 'A3000_188'
                        ? 'The sample becomes a3k format. Any incompatible settings must be changed and saved first so the conversion preserves your parameters.'
                        : target === 'A4000_A5000_224'
                          ? 'The sample becomes a4k/a5k format, even if no a4k/a5k-specific settings are used.'
                          : 'Conversion is unavailable for this sample format.'}
                </p>
                <p>The sample's name, relationships and Wave Data remain unchanged.</p>
            </div>
            <div class="format-results">
                {#each preview?.blockers ?? [] as issue}<p class="format-blocker">
                        {issue.key}{issue.storedValue === null ? '' : ` (${issue.storedValue})`}: {issue.message}
                    </p>{/each}
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status" title={document.status || reason}
                    >{document.status || reason || (preview?.allowed ? 'Ready to convert' : 'Conversion blocked')}</span
                >
                <div class="dialog-footer-actions">
                    <button
                        type="button"
                        class="secondary-button"
                        disabled={locked}
                        onclick={() => workflow.closeConversion()}>Cancel</button
                    >
                    <button type="submit" class="primary-button" disabled={!recovery && !canSubmit}
                        >{document.phase === 'refresh-failed'
                            ? 'Refresh'
                            : recovery
                              ? 'Check status'
                              : 'Convert'}</button
                    >
                </div>
            </footer>
        </form>
    </div>
</div>

<style>
    .format-dialog {
        width: min(560px, calc(100vw - 24px));
    }
    form {
        display: flex;
        flex-direction: column;
        min-height: 0;
        max-height: calc(100dvh - 32px);
    }
    .format-summary {
        padding: 10px 12px 0;
    }
    .format-summary strong {
        margin-right: 6px;
        overflow-wrap: anywhere;
    }
    p {
        margin: 7px 0;
    }
    .format-results {
        padding: 0 12px;
        min-height: 0;
        max-height: 220px;
        overflow-y: auto;
    }
    .format-blocker {
        color: var(--color-warning, #e6a34c);
        overflow-wrap: anywhere;
    }
</style>

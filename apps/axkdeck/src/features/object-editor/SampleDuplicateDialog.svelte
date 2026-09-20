<script lang="ts">
    import { modal } from '../../lib/modal';
    import Icon from '../../lib/components/Icon.svelte';
    import type { SampleDuplication } from './duplication.svelte';
    let { workflow }: { workflow: SampleDuplication } = $props();
    const recovery = $derived(workflow.phase === 'refresh-failed' || workflow.canCheck);
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell volume-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Duplicate Sample"
        use:modal={{ onescape: () => workflow.close() }}
    >
        <form
            class="volume-action-form"
            onsubmit={(event) => {
                event.preventDefault();
                if (recovery) void workflow.recover();
                else void workflow.submit();
            }}
        >
            <header class="dialog-header">
                <h2>Duplicate Sample</h2>
                <button
                    class="icon-button"
                    type="button"
                    aria-label="Close"
                    disabled={workflow.locked}
                    onclick={() => workflow.close()}><Icon name="close" size={15} /></button
                >
            </header>
            <div class="volume-action-content">
                <label
                    ><span>Sample name</span><input
                        class="dialog-field-control"
                        aria-label="Sample name"
                        bind:value={workflow.name}
                        maxlength="16"
                        autocomplete="off"
                        data-dialog-initial-focus="select"
                        disabled={workflow.phase !== 'editable'}
                    /></label
                >
                <span class="field-help">{workflow.source?.detail?.editing?.volumeName ?? ''}</span>
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status" title={workflow.message || workflow.validation}
                    >{workflow.message || workflow.validation}</span
                >
                <div class="dialog-footer-actions">
                    <button
                        class="secondary-button"
                        type="button"
                        disabled={workflow.locked}
                        onclick={() => workflow.close()}>Cancel</button
                    >
                    <button class="primary-button" type="submit" disabled={!recovery && !workflow.canSubmit}
                        >{workflow.phase === 'refresh-failed'
                            ? 'Refresh'
                            : workflow.canCheck
                              ? 'Check status'
                              : 'Duplicate'}</button
                    >
                </div>
            </footer>
        </form>
    </div>
</div>

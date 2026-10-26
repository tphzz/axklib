<script lang="ts">
    import Icon from './Icon.svelte';
    import type { ObjectRenameTarget } from '../types';
    import { modal } from '../modal';
    import type { VolumeActionPhase } from '../../features/mutation/volumeActionExecution.svelte';

    interface Props {
        target: ObjectRenameTarget;
        busy: boolean;
        phase: VolumeActionPhase;
        canDismiss: boolean;
        recovery: 'check' | 'refresh' | null;
        onrecover: () => void;
        error: string;
        oncancel: () => void;
        onsubmit: (name: string) => void;
    }

    let { target, busy, phase, canDismiss, recovery, onrecover, error, oncancel, onsubmit }: Props = $props();
    let value = $state('');
    let initialized = false;

    const subject = $derived(
        target.kind === 'program'
            ? 'Program'
            : target.kind === 'sequence'
              ? 'Sequence'
              : target.kind === 'sample-bank'
                ? 'Sample Bank'
                : target.kind === 'sample'
                  ? 'Sample'
                  : 'Wave Data',
    );
    const maximumLength = $derived(target.kind === 'program' ? 8 : 16);
    const trimmedValue = $derived(value.trim());
    const nameValid = $derived(
        trimmedValue.length > 0 && trimmedValue.length <= maximumLength && /^[\x20-\x7e]+$/.test(trimmedValue),
    );
    const canSubmit = $derived(phase === 'idle' && nameValid && trimmedValue !== target.name);
    const status = $derived(
        phase === 'checking'
            ? 'Checking image change'
            : phase === 'refreshing'
              ? 'Refreshing image'
              : phase === 'checking-status'
                ? 'Checking status'
                : busy
                  ? 'Renaming'
                  : '',
    );

    $effect(() => {
        if (initialized) return;
        value = target.name;
        initialized = true;
    });

    function submit(event: SubmitEvent): void {
        event.preventDefault();
        if (canSubmit) onsubmit(trimmedValue);
    }

    function cancel(): void {
        if (canDismiss) oncancel();
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell volume-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={`Rename ${subject}`}
        use:modal={{ onescape: cancel }}
    >
        <form class="volume-action-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>Rename {subject}</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={!canDismiss} onclick={cancel}
                    ><Icon name="close" size={15} /></button
                >
            </header>
            <div class="volume-action-content">
                <label>
                    <span>{subject} name</span>
                    <input
                        class="dialog-field-control"
                        bind:value
                        data-dialog-initial-focus="end"
                        disabled={phase !== 'idle'}
                        maxlength={maximumLength}
                        autocomplete="off"
                        aria-label={`${subject} name`}
                    />
                </label>
                {#if phase === 'idle' && trimmedValue && !nameValid}
                    <p class="field-help field-help-error">
                        Use 1–{maximumLength} printable ASCII characters.
                    </p>
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <span class="dialog-footer-status" role="status">{status}</span>
                <div class="dialog-footer-actions">
                    <button class="secondary-button" type="button" disabled={!canDismiss} onclick={cancel}
                        >{phase === 'refresh-failed' ? 'Done' : 'Cancel'}</button
                    >
                    {#if recovery}
                        <button class="primary-button" type="button" disabled={busy} onclick={onrecover}
                            >{recovery === 'check' ? 'Check status' : 'Refresh'}</button
                        >
                    {:else}
                        <button class="primary-button" type="submit" disabled={!canSubmit}>Rename</button>
                    {/if}
                </div>
            </footer>
        </form>
    </div>
</div>

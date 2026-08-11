<script lang="ts">
    import type { ObjectRenameTarget } from '../types';
    import { modal } from '../modal';

    interface Props {
        target: ObjectRenameTarget;
        busy: boolean;
        error: string;
        oncancel: () => void;
        onsubmit: (name: string) => void;
    }

    let { target, busy, error, oncancel, onsubmit }: Props = $props();
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
    const canSubmit = $derived(!busy && nameValid && trimmedValue !== target.name);

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
        if (!busy) oncancel();
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
                <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}>×</button>
            </header>
            <div class="volume-action-content">
                <label>
                    <span>{subject} name</span>
                    <input
                        bind:value
                        data-dialog-initial-focus="select"
                        disabled={busy}
                        maxlength={maximumLength}
                        autocomplete="off"
                        aria-label={`${subject} name`}
                    />
                </label>
                {#if trimmedValue && !nameValid}
                    <p class="field-help field-help-error">
                        Use 1–{maximumLength} printable ASCII characters.
                    </p>
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={busy} onclick={cancel}>Cancel</button>
                <button class="primary-button" type="submit" disabled={!canSubmit}>
                    {busy ? 'Renaming' : 'Rename'}
                </button>
            </footer>
        </form>
    </div>
</div>

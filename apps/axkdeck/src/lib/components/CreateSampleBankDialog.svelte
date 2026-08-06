<script lang="ts">
    import { validSamplerName } from '../audioImport';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        volumeName: string;
        sampleCount: number;
        existingNames: string[];
        busy: boolean;
        error: string;
        oncancel: () => void;
        onsubmit: (name: string) => void;
    }

    let { volumeName, sampleCount, existingNames, busy, error, oncancel, onsubmit }: Props = $props();
    let value = $state('');
    const trimmedValue = $derived(value.trim());
    const duplicate = $derived(
        existingNames.some((name) => name.toLocaleLowerCase() === trimmedValue.toLocaleLowerCase()),
    );
    const nameError = $derived(
        trimmedValue.length === 0
            ? ''
            : !validSamplerName(trimmedValue)
              ? 'Use 1-16 printable ASCII characters.'
              : duplicate
                ? `Sample Bank already exists: ${trimmedValue}`
                : '',
    );
    const canSubmit = $derived(!busy && validSamplerName(trimmedValue) && !duplicate);

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
        aria-label="Create Sample Bank"
        use:modal={{ onescape: cancel }}
    >
        <form class="volume-action-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>Create Sample Bank</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}>
                    <Icon name="close" size={15} />
                </button>
            </header>
            <div class="volume-action-content">
                <p>
                    Add {sampleCount} selected {sampleCount === 1 ? 'Sample' : 'Samples'} from {volumeName} to a new Sample
                    Bank.
                </p>
                <label>
                    <span>Sample Bank name</span>
                    <input
                        bind:value
                        data-dialog-initial-focus="select"
                        disabled={busy}
                        maxlength="16"
                        autocomplete="off"
                        aria-invalid={nameError !== ''}
                    />
                </label>
                {#if nameError}<p class="field-help field-help-error">{nameError}</p>{/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={busy} onclick={cancel}>Cancel</button>
                <button class="primary-button" type="submit" disabled={!canSubmit}>
                    {busy ? 'Creating' : 'Create Sample Bank'}
                </button>
            </footer>
        </form>
    </div>
</div>

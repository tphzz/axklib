<script lang="ts">
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import { modal } from '../modal';

    interface Props {
        action: ImageTreeAction;
        item: DiskTreeItem;
        busy: boolean;
        error: string;
        oncancel: () => void;
        onsubmit: (name: string) => void;
    }

    let { action, item, busy, error, oncancel, onsubmit }: Props = $props();
    let value = $state('');
    let initialized = false;

    $effect(() => {
        if (initialized) return;
        value = action === 'rename-volume' || action === 'rename-partition' ? item.name : '';
        initialized = true;
    });

    const title = $derived(
        action === 'add-volume'
            ? 'Add volume'
            : action === 'rename-volume'
              ? 'Rename volume'
              : action === 'rename-partition'
                ? 'Rename partition'
                : 'Delete volume',
    );
    const subject = $derived(action === 'rename-partition' ? 'Partition' : 'Volume');
    const submitLabel = $derived(
        busy
            ? action === 'add-volume'
                ? 'Adding'
                : action === 'rename-volume' || action === 'rename-partition'
                  ? 'Renaming'
                  : 'Deleting'
            : action === 'add-volume'
              ? 'Add'
              : action === 'rename-volume' || action === 'rename-partition'
                ? 'Rename'
                : 'Delete permanently',
    );
    const trimmedValue = $derived(value.trim());
    const nameValid = $derived(
        trimmedValue.length > 0 && trimmedValue.length <= 16 && /^[\x20-\x7e]+$/.test(trimmedValue),
    );
    const canSubmit = $derived(
        !busy &&
            (action === 'delete-volume'
                ? true
                : nameValid &&
                  (action !== 'rename-volume' && action !== 'rename-partition' ? true : trimmedValue !== item.name)),
    );

    function submit(event: SubmitEvent): void {
        event.preventDefault();
        if (canSubmit) onsubmit(action === 'delete-volume' ? item.name : trimmedValue);
    }
</script>

<div class="dialog-backdrop dialog-backdrop-raised" role="presentation">
    <div
        class="dialog-shell volume-action-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: oncancel }}
    >
        <form class="volume-action-form" onsubmit={submit}>
            <header class="dialog-header">
                <h2>{title}</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}
                    >×</button
                >
            </header>
            <div class="volume-action-content">
                {#if action === 'delete-volume'}
                    <strong>Permanently delete “{item.name}”?</strong>
                    <p>The volume and all objects it contains will be destroyed. This action cannot be undone.</p>
                {:else}
                    <label>
                        <span>{subject} name</span>
                        <input
                            bind:value
                            disabled={busy}
                            maxlength="16"
                            autocomplete="off"
                            aria-label={`${subject} name`}
                        />
                    </label>
                    {#if trimmedValue && !nameValid}
                        <p class="field-help field-help-error">Use 1–16 printable ASCII characters.</p>
                    {/if}
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </div>
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
                <button
                    class={action === 'delete-volume' ? 'danger-button' : 'primary-button'}
                    type="submit"
                    disabled={!canSubmit}
                >
                    {submitLabel}
                </button>
            </footer>
        </form>
    </div>
</div>

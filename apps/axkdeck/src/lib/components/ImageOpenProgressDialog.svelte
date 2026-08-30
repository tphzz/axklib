<script lang="ts">
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        label: string;
        completed: number;
        total?: number;
        cancellable: boolean;
        cancelling: boolean;
        oncancel: () => void;
    }

    let { label, completed, total, cancellable, cancelling, oncancel }: Props = $props();
    const hasProgress = $derived(total !== undefined && total > 0);
</script>

<div class="dialog-backdrop dialog-backdrop-top" role="presentation">
    <div
        class="dialog-shell image-open-progress-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Open image progress"
        aria-busy="true"
        use:modal={{ onescape: cancellable && !cancelling ? oncancel : undefined }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="folder-open" size={16} />
                <h2>Open image</h2>
            </div>
        </header>

        <div class="image-open-progress-content" aria-live="polite">
            <p class="dialog-progress" role="status">{label}</p>
            {#if hasProgress}
                <progress value={completed} max={total} aria-label={label}></progress>
            {:else}
                <progress aria-label={label}></progress>
            {/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={!cancellable || cancelling} onclick={oncancel}
                >{cancelling ? 'Cancelling' : 'Cancel'}</button
            >
        </footer>
    </div>
</div>

<style>
    .image-open-progress-dialog {
        width: min(440px, calc(100vw - 32px));
    }

    .image-open-progress-content {
        display: grid;
        gap: 10px;
        padding: 16px 14px;
    }

    .image-open-progress-content .dialog-progress {
        margin: 0;
    }

    progress {
        width: 100%;
        height: 7px;
        accent-color: var(--color-accent);
    }
</style>

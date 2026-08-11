<script lang="ts">
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        onacknowledge: () => void;
    }

    let { onacknowledge }: Props = $props();
</script>

<div class="dialog-backdrop dialog-backdrop-top" role="presentation">
    <div
        class="dialog-shell experimental-warning-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="experimental-warning-title"
        aria-describedby="experimental-warning-description experimental-warning-disclaimer"
        use:modal
    >
        <header class="dialog-header">
            <div>
                <span class="experimental-warning-icon" aria-hidden="true">
                    <Icon name="triangle-alert" size={17} />
                </span>
                <h2 id="experimental-warning-title">Experimental software</h2>
            </div>
        </header>

        <div class="experimental-warning-content">
            <p id="experimental-warning-description">
                axkdeck is experimental software. Alteration features modify disk images and may corrupt or destroy disk
                images.
            </p>
            <p>Back up disk images before making changes.</p>
            <p id="experimental-warning-disclaimer" class="experimental-warning-disclaimer">
                The developer is not responsible for data loss.
            </p>
        </div>

        <footer class="dialog-footer">
            <button class="primary-button" type="button" data-dialog-initial-focus onclick={onacknowledge}>
                I understand
            </button>
        </footer>
    </div>
</div>

<style>
    .experimental-warning-dialog {
        width: min(480px, calc(100vw - 40px));
    }

    .experimental-warning-icon {
        display: inline-flex;
        color: var(--color-danger);
    }

    .experimental-warning-content {
        padding: 16px;
    }

    .experimental-warning-content p {
        margin: 0;
        line-height: 1.55;
    }

    .experimental-warning-content p + p {
        margin-top: 10px;
    }

    .experimental-warning-disclaimer {
        color: var(--color-text-muted);
        font-size: 10px;
    }
</style>

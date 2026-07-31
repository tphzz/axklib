<script lang="ts">
    import { modal } from '../modal';

    interface Props {
        title: string;
        message: string;
        onclose: () => void;
    }

    let { title, message, onclose }: Props = $props();
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell import-unavailable-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        use:modal={{ onescape: onclose }}
    >
        <header class="dialog-header">
            <h2>{title}</h2>
            <button class="icon-button" type="button" aria-label="Close" onclick={onclose}>×</button>
        </header>
        <div class="import-unavailable-content"><p>{message}</p></div>
        <footer class="dialog-footer">
            <button class="primary-button" type="button" onclick={onclose}>OK</button>
        </footer>
    </div>
</div>

<style>
    .import-unavailable-dialog {
        width: min(460px, calc(100vw - 32px));
    }

    .import-unavailable-content {
        padding: 14px 16px 18px;
    }

    p {
        margin: 0;
        color: var(--text-secondary);
        line-height: 1.45;
    }
</style>

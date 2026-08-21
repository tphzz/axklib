<script lang="ts">
    import Icon from './Icon.svelte';
    import { modal } from '../modal';

    interface Props {
        title: string;
        progressLabel: string;
        cancellable: boolean;
        oncancel: () => void;
    }

    let { title, progressLabel, cancellable, oncancel }: Props = $props();
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell export-progress-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={`${title} progress`}
        aria-busy="true"
        use:modal={{ onescape: cancellable ? oncancel : undefined }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="save" size={16} />
                <h2>{title}</h2>
            </div>
        </header>

        <div class="export-progress-content">
            <p class="dialog-progress" role="status">{progressLabel}</p>
        </div>

        {#if cancellable}
            <footer class="dialog-footer">
                <button class="secondary-button" type="button" onclick={oncancel}>Cancel export</button>
            </footer>
        {/if}
    </div>
</div>

<style>
    .export-progress-dialog {
        width: min(460px, calc(100vw - 32px));
    }

    .export-progress-content {
        padding: 20px;
    }

    .export-progress-content .dialog-progress {
        margin: 0;
    }
</style>

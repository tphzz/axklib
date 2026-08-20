<script lang="ts">
    import type { DesktopBuildInfoState } from '../desktopBuildInfo';
    import { modal } from '../modal';
    import Icon from './Icon.svelte';

    interface Props {
        state: DesktopBuildInfoState;
        onclose: () => void;
    }

    let { state, onclose }: Props = $props();
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell about-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="about-title"
        aria-describedby="about-description"
        use:modal={{ onescape: onclose }}
    >
        <header class="dialog-header">
            <div>
                <span class="about-mark" aria-hidden="true">
                    <Icon name="waveform" size={18} strokeWidth={2.1} />
                </span>
                <h2 id="about-title">About axkdeck</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close About" onclick={onclose}>×</button>
        </header>

        <div class="about-content">
            <p id="about-description">A-series sampler disk workspace</p>
            {#if state.status === 'loading'}
                <p class="about-status" aria-live="polite">Loading version information…</p>
            {:else if state.status === 'error'}
                <p class="about-status about-error" role="status">Version information is unavailable.</p>
            {:else}
                <dl>
                    <div>
                        <dt>Version</dt>
                        <dd>{state.buildInfo.semanticVersion}</dd>
                    </div>
                    <div>
                        <dt>Build</dt>
                        <dd>{state.buildInfo.sourceIdentity}</dd>
                    </div>
                </dl>
            {/if}
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" data-dialog-initial-focus onclick={onclose}>Close</button>
        </footer>
    </div>
</div>

<style>
    .about-dialog {
        width: min(440px, calc(100vw - 32px));
    }

    .about-mark {
        display: inline-grid;
        width: 28px;
        height: 28px;
        place-items: center;
        color: #f3d6bd;
        border: 1px solid rgb(204 120 50 / 55%);
        border-radius: 7px;
        background: linear-gradient(145deg, #b86c2c, #70421f);
    }

    .about-content {
        min-height: 118px;
        padding: 16px;
    }

    .about-content p {
        margin: 0;
    }

    #about-description {
        color: var(--color-text-muted);
    }

    .about-status {
        margin-top: 24px !important;
        color: var(--color-text-muted);
    }

    .about-error {
        color: var(--color-danger);
    }

    dl {
        margin: 20px 0 0;
    }

    dl div {
        display: grid;
        grid-template-columns: 72px minmax(0, 1fr);
        gap: 12px;
        padding: 7px 0;
        border-top: 1px solid var(--color-border);
    }

    dt {
        color: var(--color-text-muted);
    }

    dd {
        min-width: 0;
        margin: 0;
        overflow-wrap: anywhere;
        color: var(--color-text-strong);
    }
</style>

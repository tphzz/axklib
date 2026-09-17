<script lang="ts">
    import ExperimentalWarningDialog from './lib/components/ExperimentalWarningDialog.svelte';

    interface Props {
        status?: 'starting' | 'unavailable';
        message?: string;
        warningOpen?: boolean;
        onacknowledge: () => void;
        onretry: () => void;
        onopensettings: () => void;
    }

    let {
        status = 'starting',
        message = '',
        warningOpen = true,
        onacknowledge,
        onretry,
        onopensettings,
    }: Props = $props();
</script>

<div class="startup-shell" aria-busy={status === 'starting'}>
    <header class="startup-header">
        <div class="startup-brand"><span aria-hidden="true">~</span><strong>axkdeck</strong></div>
        <nav aria-label="Object views">
            <button type="button" disabled>Programs</button>
            <button type="button" disabled>Sample Banks</button>
            <button type="button" disabled>Samples</button>
            <button type="button" disabled>Wave Data</button>
            <button type="button" disabled>Sequences</button>
        </nav>
    </header>
    <aside class="startup-library">
        <p>IMAGE</p>
        <div class="startup-placeholder">Open Image</div>
        <p>CONTENTS</p>
        <strong>Partitions and volumes</strong>
    </aside>
    <main class="startup-workspace">
        <header><strong>Programs</strong><span>0 items</span></header>
        {#if status === 'unavailable'}
            <section class="startup-error" role="alert">
                <strong>axklib-server is unavailable</strong>
                <p>{message || 'The application could not connect to its data service.'}</p>
                <div>
                    <button type="button" class="secondary-button" onclick={onretry}>Retry</button>
                    <button type="button" class="secondary-button" onclick={onopensettings}>Connection settings</button>
                </div>
            </section>
        {:else}
            <p class="startup-empty">Starting services...</p>
        {/if}
    </main>
    <aside class="startup-inspector" aria-label="Object inspector">
        <div class="panel-heading">
            <div>
                <p class="eyebrow">Inspector</p>
                <h2>Object details</h2>
            </div>
        </div>
    </aside>
    <footer>
        <span class:startup-failed={status === 'unavailable'}></span>{status === 'starting'
            ? 'Starting services...'
            : 'Service unavailable'}
    </footer>
</div>

{#if warningOpen}
    <ExperimentalWarningDialog {onacknowledge} />
{/if}

<style>
    .startup-shell {
        display: grid;
        grid-template-columns: 248px minmax(520px, 1fr) 268px;
        grid-template-rows: 40px minmax(0, 1fr) 18px;
        width: 100%;
        height: 100%;
        min-width: 760px;
        background: var(--color-bg);
        color: var(--color-text);
    }
    .startup-header {
        grid-column: 1 / -1;
        display: grid;
        grid-template-columns: 248px minmax(0, 1fr);
        align-items: stretch;
        border-bottom: 1px solid var(--color-border);
        background: #1c1f21;
    }
    .startup-brand {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 0 10px;
        color: var(--color-text-strong);
    }
    .startup-brand span {
        display: grid;
        width: 28px;
        height: 28px;
        place-items: center;
        border: 1px solid rgb(204 120 50 / 55%);
        border-radius: 7px;
        background: #8e5428;
        font-weight: 700;
    }
    nav {
        display: flex;
    }
    nav button {
        padding: 0 14px;
        border: 0;
        background: transparent;
        color: var(--color-text-muted);
    }
    .startup-library {
        padding: 12px 10px;
        border-right: 1px solid var(--color-border);
    }
    .startup-inspector {
        border-right: 0;
        border-left: 1px solid var(--color-border);
    }
    .startup-library p {
        margin: 0 0 5px;
        color: var(--color-text-muted);
        font-size: 9px;
        font-weight: 700;
        letter-spacing: 0.12em;
    }
    .startup-placeholder {
        height: 44px;
        margin: 8px 0 18px;
        padding: 13px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        color: var(--color-text-muted);
    }
    .startup-workspace {
        min-width: 0;
    }
    .startup-workspace > header {
        display: flex;
        align-items: center;
        gap: 5px;
        height: 34px;
        padding: 0 8px;
        border-bottom: 1px solid var(--color-border);
    }
    .startup-workspace > header span {
        color: var(--color-text-muted);
        font-size: 9px;
    }
    .startup-empty {
        display: grid;
        height: calc(100% - 34px);
        margin: 0;
        place-items: center;
        color: var(--color-text-muted);
    }
    .startup-error {
        width: min(460px, calc(100% - 40px));
        margin: 64px auto;
        padding: 16px;
        border: 1px solid var(--color-border);
        border-radius: 6px;
        background: var(--color-panel);
    }
    .startup-error strong {
        color: var(--color-danger);
    }
    .startup-error p {
        line-height: 1.45;
    }
    .startup-error div {
        display: flex;
        gap: 8px;
    }
    footer {
        grid-column: 1 / -1;
        display: flex;
        align-items: center;
        gap: 6px;
        padding: 0 10px;
        border-top: 1px solid var(--color-border);
        font-size: 9px;
    }
    footer span {
        width: 5px;
        height: 5px;
        border-radius: 50%;
        background: var(--color-success);
    }
    footer span.startup-failed {
        background: var(--color-danger);
    }
</style>

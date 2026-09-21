<script lang="ts">
    import { untrack } from 'svelte';
    import { generationSampleFormat, type ASeriesPreferences } from '../aSeriesPreferences.svelte';
    import { modal } from '../modal';
    import { userFacingMessage } from '../userFacingMessage';
    import Icon from './Icon.svelte';
    import SampleFormatControl from './SampleFormatControl.svelte';

    let {
        preferences,
        oncancel,
        onsaved,
    }: {
        preferences: ASeriesPreferences;
        oncancel: () => void;
        onsaved: () => void;
    } = $props();
    let format = $state(untrack(() => generationSampleFormat(preferences.generation)));
    let busy = $state(false);
    let error = $state('');
    function cancel(): void {
        if (!busy) oncancel();
    }
    async function save(event: SubmitEvent): Promise<void> {
        event.preventDefault();
        if (busy || preferences.loadError) return;
        busy = true;
        error = '';
        try {
            await preferences.save(format === 'A3000_188' ? 'A3000' : 'A4000_A5000');
            onsaved();
        } catch (failure) {
            error = userFacingMessage(failure);
        } finally {
            busy = false;
        }
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell preferences-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Preferences"
        use:modal={{ onescape: cancel }}
    >
        <form onsubmit={save}>
            <header class="dialog-header">
                <h2>Preferences</h2>
                <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={cancel}
                    ><Icon name="close" size={15} /></button
                >
            </header>
            <section class="preferences-content" aria-labelledby="a-series-preferences-heading">
                <h3 id="a-series-preferences-heading">A-Series</h3>
                <div class="preference-row">
                    <span>Preferred A-Series generation</span>
                    <SampleFormatControl
                        bind:value={format}
                        label="Preferred A-Series generation"
                        disabled={busy || Boolean(preferences.loadError)}
                    />
                </div>
            </section>
            <footer class="dialog-footer">
                <span
                    class="dialog-footer-status"
                    class:dialog-error={Boolean(error || preferences.loadError)}
                    role="status"
                    title={error || preferences.loadError}
                    >{error || preferences.loadError || (busy ? 'Saving preferences...' : '')}</span
                >
                <div class="dialog-footer-actions">
                    <button class="secondary-button" type="button" disabled={busy} onclick={cancel}>Cancel</button>
                    <button class="primary-button" type="submit" disabled={busy || Boolean(preferences.loadError)}
                        >Save</button
                    >
                </div>
            </footer>
        </form>
    </div>
</div>

<style>
    .preferences-dialog {
        width: min(460px, calc(100vw - 32px));
    }
    .preferences-content {
        padding: 14px 16px;
        display: grid;
        gap: 12px;
    }
    h3 {
        margin: 0;
        font-size: var(--dialog-body-font-size);
    }
    .preference-row {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        justify-content: space-between;
        gap: 8px 16px;
    }
    .preference-row > span {
        font-size: var(--dialog-label-font-size);
        color: var(--color-text-muted);
    }
</style>

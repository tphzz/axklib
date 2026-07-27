<script lang="ts">
    import { modal } from '../modal';
    import type { ImageSessionAudioExportInspection } from '../transport';
    import type { PackageExportSelection } from '../types';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        items: PackageExportSelection[];
        inspection: ImageSessionAudioExportInspection | null;
        desktop: boolean;
        loading: boolean;
        busy: boolean;
        progressLabel: string;
        error: string;
        format: 'SFZ' | 'WAV';
        onformatchange: (format: 'SFZ' | 'WAV') => void;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let {
        items,
        inspection,
        desktop,
        loading,
        busy,
        progressLabel,
        error,
        format,
        onformatchange,
        onworkspace,
        onlocal,
        oncancel,
    }: Props = $props();

    const singleItem = $derived(items.length === 1 ? items[0] : undefined);
    const counts = $derived.by(() => {
        if (!inspection) return '';
        const values = [
            [inspection.programCount, 'Program'],
            [inspection.sampleBankCount, 'Sample Bank'],
            [inspection.sampleCount, 'Sample'],
            [inspection.waveDataCount, 'Wave Data'],
        ] as const;
        return values
            .filter(([count]) => count > 0)
            .map(([count, label]) => `${count} ${label === 'Wave Data' || count === 1 ? label : `${label}s`}`)
            .join(' · ');
    });
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export SFZ"
        aria-busy={loading || busy}
        use:modal={{ onescape: busy ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="music" size={16} />
                <h2>Export SFZ</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="Audio export destination">
                <h3>{singleItem ? `Export “${singleItem.name}”` : `Export ${items.length} objects`}</h3>
                <p>Export the selected objects and their playable audio dependencies.</p>

                {#if loading}
                    <p class="dialog-progress" role="status">Inspecting audio export…</p>
                {:else if inspection}
                    <p class="package-export-type-summary">{counts}</p>
                    <div class="audio-export-format" role="group" aria-label="Export format">
                        <button
                            class:active={format === 'SFZ'}
                            type="button"
                            disabled={busy || !inspection.sfzEligible}
                            aria-pressed={format === 'SFZ'}
                            onclick={() => onformatchange('SFZ')}>SFZ + WAV</button
                        >
                        <button
                            class:active={format === 'WAV'}
                            type="button"
                            disabled={busy}
                            aria-pressed={format === 'WAV'}
                            onclick={() => onformatchange('WAV')}>WAV files</button
                        >
                    </div>
                    {#if inspection.sfzEligible}
                        <p>
                            {format === 'SFZ'
                                ? `${inspection.sfzFileCount} SFZ ${inspection.sfzFileCount === 1 ? 'file' : 'files'} and referenced WAV files will be created.`
                                : 'All resolved Wave Data will be exported as WAV files without SFZ mappings.'}
                        </p>
                    {:else}
                        <p class="audio-export-guidance">
                            SFZ mappings are unavailable for this selection. Export the resolved audio as WAV files
                            instead.
                        </p>
                    {/if}
                    {#each inspection.issues as issue (`${issue.code}:${issue.message}`)}
                        <p class:dialog-error={issue.fatal} class="audio-export-issue">{issue.message}</p>
                    {/each}
                    <ExportDestinationChooser {desktop} {busy} {onworkspace} {onlocal} />
                {/if}
                {#if busy}<p class="dialog-progress" role="status">{progressLabel || 'Exporting audio…'}</p>{/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}
                >{busy ? 'Cancel export' : 'Cancel'}</button
            >
        </footer>
    </div>
</div>

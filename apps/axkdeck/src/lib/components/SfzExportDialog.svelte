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
        error: string;
        format: 'SFZ' | 'WAV';
        onformatchange: (format: 'SFZ' | 'WAV') => void;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    interface DisplayIssue {
        key: string;
        message: string;
        fatal: boolean;
    }

    let { items, inspection, desktop, loading, error, format, onformatchange, onworkspace, onlocal, oncancel }: Props =
        $props();

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
    const displayIssues = $derived.by(() => {
        const groups = new Map<string, DisplayIssue & { count: number }>();
        for (const issue of inspection?.issues ?? []) {
            const fatal = issue.fatal === true;
            const programAssignment =
                issue.code === 'unconfirmed_relationship_excluded' &&
                'relationshipType' in issue &&
                issue.relationshipType.startsWith('PROG_ASSIGNMENT_TO_');
            const key = programAssignment
                ? `program-assignment:${fatal}`
                : JSON.stringify([fatal, issue.code, issue.message]);
            const existing = groups.get(key);
            if (existing) {
                existing.count += 1;
            } else {
                groups.set(key, { key, message: issue.message, fatal, count: 1 });
            }
        }
        return [...groups.values()].map(({ count, ...issue }) => {
            if (issue.key.startsWith('program-assignment:')) {
                issue.message =
                    count === 1
                        ? '1 unconfirmed Program assignment was excluded from SFZ export.'
                        : `${count} unconfirmed Program assignments were excluded from SFZ export.`;
            } else if (count > 1) {
                issue.message = `${issue.message} (${count} occurrences)`;
            }
            return issue;
        });
    });
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export SFZ"
        aria-busy={loading}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="music" size={16} />
                <h2>Export SFZ</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
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
                            disabled={!inspection.sfzEligible}
                            aria-pressed={format === 'SFZ'}
                            onclick={() => onformatchange('SFZ')}>SFZ + WAV</button
                        >
                        <button
                            class:active={format === 'WAV'}
                            type="button"
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
                    {#each displayIssues as issue (issue.key)}
                        <p class:dialog-error={issue.fatal} class="audio-export-issue">{issue.message}</p>
                    {/each}
                    <ExportDestinationChooser {desktop} {onworkspace} {onlocal} />
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

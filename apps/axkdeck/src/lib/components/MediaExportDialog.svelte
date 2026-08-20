<script lang="ts">
    import { formatStoredSize } from '../formatBytes';
    import { modal } from '../modal';
    import type { MediaExportRequest } from '../../features/export/mediaWorkflow.svelte';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        request: MediaExportRequest;
        desktop: boolean;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let { request, desktop, onworkspace, onlocal, oncancel }: Props = $props();
    const multiFloppy = $derived(request.inspection?.artifactKind === 'FLOPPY_DISK_SET');
    const title = $derived(
        multiFloppy
            ? 'Export floppy disk set'
            : request.selection.format === 'ISO9660'
              ? 'Export CD-ROM image'
              : 'Export floppy image',
    );
    const description = $derived(
        multiFloppy && request.inspection
            ? `The complete volume will be written as ${request.inspection.floppyImageCount} ordered Yamaha-compatible 1.44 MB floppy images in a ZIP archive.`
            : request.selection.format === 'ISO9660'
              ? 'The complete partition will be written as a Yamaha-compatible CD-ROM image.'
              : 'The complete volume will be written as a Yamaha-compatible 1.44 MB floppy image.',
    );

    function formatMeasurement(value: number, unit: 'BYTES' | 'DIRECTORY_ENTRIES' | 'FLOPPY_IMAGES'): string {
        if (unit === 'BYTES') return formatStoredSize(value);
        if (unit === 'DIRECTORY_ENTRIES') return `${value} ${value === 1 ? 'entry' : 'entries'}`;
        return `${value} ${value === 1 ? 'floppy image' : 'floppy images'}`;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label={title}
        aria-busy={request.loading}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="hard-drive" size={16} />
                <h2>{title}</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="Media image destination">
                <h3>Export “{request.item.name}”</h3>
                <p>{description}</p>
                {#if request.loading}
                    <p class="dialog-progress" role="status">Checking media capacity…</p>
                {:else if request.inspection}
                    <p class="package-export-type-summary">
                        {request.inspection.objectCount}
                        {request.inspection.objectCount === 1 ? 'object' : 'objects'} ·
                        {formatStoredSize(request.inspection.payloadBytes)} payload ·
                        {formatStoredSize(request.inspection.projectedOutputBytes)}
                        {request.inspection.artifactKind === 'FLOPPY_DISK_SET' ? 'archive' : 'image'}
                    </p>
                    {#if request.inspection.volumes.length > 1}
                        <p>{request.inspection.volumes.length} volumes from {request.inspection.partitionName}</p>
                    {/if}
                    {#each request.inspection.issues as issue, index (`${issue.code}:${index}`)}
                        <div
                            class:dialog-error={issue.blocking}
                            class="audio-export-issue"
                            role={issue.blocking ? 'alert' : undefined}
                        >
                            <p>{issue.message}</p>
                            {#if issue.measurement}
                                <small>
                                    Required {formatMeasurement(issue.measurement.required, issue.measurement.unit)} · Available
                                    {formatMeasurement(issue.measurement.available, issue.measurement.unit)}
                                </small>
                            {/if}
                        </div>
                    {/each}
                    <ExportDestinationChooser
                        {desktop}
                        disabled={!request.inspection.canExport}
                        {onworkspace}
                        {onlocal}
                    />
                {/if}
                {#if request.error}<p class="dialog-error" role="alert">{request.error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

<script lang="ts">
    import type { ImageSessionVolumeFloppyExportInspection } from '../transport';
    import { modal } from '../modal';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        scopeName: string;
        inspection: ImageSessionVolumeFloppyExportInspection | null;
        desktop: boolean;
        loading: boolean;
        error: string;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let { scopeName, inspection, desktop, loading, error, onworkspace, onlocal, oncancel }: Props = $props();
    const unavailable = $derived(loading || !inspection || inspection.exportableCount === 0);

    function outcome(volume: NonNullable<typeof inspection>['volumes'][number]): string {
        if (volume.state === 'EMPTY') return 'Empty';
        if (volume.state === 'BLOCKED') return 'Blocked';
        return `${volume.floppyImageCount} ${volume.floppyImageCount === 1 ? 'disk' : 'disks'} · ${volume.directoryName}`;
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export volumes to floppies"
        aria-busy={loading}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="disc" size={16} />
                <h2>Export volumes to floppies</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="Volume floppy export destination">
                <h3>Export “{inspection?.scopeName ?? scopeName}”</h3>
                {#if inspection}
                    <div class="export-summary" aria-label="Volume floppy summary">
                        <span
                            >{inspection.exportableCount}
                            {inspection.exportableCount === 1 ? 'volume' : 'volumes'}</span
                        >
                        <span
                            >{inspection.totalFloppyImageCount} floppy
                            {inspection.totalFloppyImageCount === 1 ? 'image' : 'images'}</span
                        >
                        {#if inspection.emptyCount > 0}<span>{inspection.emptyCount} empty</span>{/if}
                        {#if inspection.blockedCount > 0}<span>{inspection.blockedCount} blocked</span>{/if}
                    </div>
                    <div class="package-object-list" aria-label="Volumes">
                        {#each inspection.volumes as volume (volume.contentId)}
                            <div class="package-object-row">
                                <strong>{volume.displayName}</strong>
                                <span>{outcome(volume)}</span>
                            </div>
                            {#if volume.state === 'BLOCKED' && volume.issues[0]}
                                <p class="volume-blocker">{volume.issues[0].message}</p>
                            {/if}
                        {/each}
                    </div>
                {/if}
                {#if !unavailable}
                    <ExportDestinationChooser {desktop} {onworkspace} {onlocal} />
                {/if}
                {#if loading}<p class="dialog-progress" role="status">Inspecting volumes…</p>{/if}
                {#if inspection?.exportableCount === 0}
                    <p class="dialog-error" role="alert">The selected partition has no exportable volumes.</p>
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

<style>
    .volume-blocker {
        margin: -5px 6px 3px;
        color: var(--color-danger-text);
        font-size: var(--dialog-body-font-size);
    }
</style>

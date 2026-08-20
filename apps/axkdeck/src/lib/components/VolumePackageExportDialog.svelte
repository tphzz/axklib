<script lang="ts">
    import type { ImageSessionVolumePackageExportInspection } from '../transport';
    import { modal } from '../modal';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        scopeName: string;
        inspection: ImageSessionVolumePackageExportInspection | null;
        desktop: boolean;
        loading: boolean;
        error: string;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let { scopeName, inspection, desktop, loading, error, onworkspace, onlocal, oncancel }: Props = $props();
    const unavailable = $derived(loading || !inspection || inspection.exportableCount === 0);
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export volume packages"
        aria-busy={loading}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2>Export volume packages</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="Volume package export destination">
                <h3>Export “{inspection?.scopeName ?? scopeName}”</h3>
                {#if inspection}
                    <div class="export-summary" aria-label="Volume package summary">
                        <span
                            >{inspection.exportableCount}
                            {inspection.exportableCount === 1 ? 'package' : 'packages'}</span
                        >
                        {#if inspection.emptyCount > 0}<span>{inspection.emptyCount} empty skipped</span>{/if}
                    </div>
                    <div class="package-object-list" aria-label="Volumes">
                        {#each inspection.volumes as volume (volume.contentId)}
                            <div class="package-object-row">
                                <strong>{volume.displayName}</strong>
                                <span>{volume.state === 'READY' ? volume.packagePath : 'Empty'}</span>
                            </div>
                        {/each}
                    </div>
                {/if}
                {#if !unavailable}
                    <ExportDestinationChooser {desktop} {onworkspace} {onlocal} />
                {/if}
                {#if loading}<p class="dialog-progress" role="status">Inspecting volumes…</p>{/if}
                {#if inspection?.exportableCount === 0}
                    <p class="dialog-error" role="alert">The selected scope has no non-empty volumes.</p>
                {/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

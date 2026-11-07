<script lang="ts">
    import type { ImageSessionVolumePackageExportInspection } from '../transport';
    import type { VolumePackageExportDestinationFlow } from '../../features/export/volumePackageWorkflow.svelte';
    import { modal } from '../modal';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        scopeName: string;
        inspection: ImageSessionVolumePackageExportInspection | null;
        desktop: boolean;
        destinationFlow?: VolumePackageExportDestinationFlow;
        loading: boolean;
        picking?: boolean;
        error: string;
        onworkspace: () => void;
        onlocal: () => void;
        onretry?: () => void;
        oncancel: () => void;
    }

    let {
        scopeName,
        inspection,
        desktop,
        destinationFlow = 'CHOOSER',
        loading,
        picking = false,
        error,
        onworkspace,
        onlocal,
        onretry,
        oncancel,
    }: Props = $props();
    const native = $derived(destinationFlow === 'DIRECT_COMPUTER');
    const unavailable = $derived(loading || !inspection || inspection.exportableCount === 0);
    let cancelButton = $state<HTMLButtonElement>();

    function retry(): void {
        cancelButton?.focus({ preventScroll: true });
        onretry?.();
    }
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export volume packages"
        aria-busy={loading || picking}
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                {#if !native}<Icon name="archive" size={16} />{/if}
                <h2>Export volume packages</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}
                ><Icon name="close" size={15} /></button
            >
        </header>

        {#if native}
            <!-- svelte-ignore a11y_no_noninteractive_tabindex (Overflowing source details need keyboard scrolling in native WebViews.) -->
            <section class="native-export-summary" aria-label="Volume package export source" tabindex="0">
                <strong>{inspection?.scopeName ?? scopeName}</strong>
                {#if inspection}
                    <p>
                        {inspection.exportableCount} volume {inspection.exportableCount === 1 ? 'package' : 'packages'}
                    </p>
                {/if}
                {#if loading}<p role="status">Inspecting volumes...</p>{/if}
                {#if inspection?.exportableCount === 0 && !loading}
                    <p class="dialog-error" role="alert">The selected scope has no non-empty volumes.</p>
                {/if}
                {#if error}
                    <!-- svelte-ignore a11y_no_noninteractive_tabindex (Bounded error details are keyboard-scrollable, verified in WebKitGTK.) -->
                    <p class="dialog-results dialog-error" role="alert" tabindex="0">{error}</p>
                {/if}
            </section>
        {:else}
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
        {/if}

        <footer class="dialog-footer">
            <p class="dialog-footer-status">{native && loading ? 'Inspecting source' : ''}</p>
            <div class="dialog-footer-actions">
                <button bind:this={cancelButton} class="secondary-button" type="button" onclick={oncancel}
                    >Cancel</button
                >
                {#if native && (loading || inspection?.exportableCount !== 0)}
                    <button class="primary-button" type="button" disabled={loading || picking} onclick={retry}
                        >Retry</button
                    >
                {/if}
            </div>
        </footer>
    </div>
</div>

<style>
    .native-export-summary {
        display: grid;
        min-height: 0;
        gap: 4px;
        padding: 12px;
        overflow-y: auto;
        overflow-wrap: anywhere;
    }

    .native-export-summary p {
        margin: 0;
    }

    .native-export-summary strong {
        font-weight: 600;
    }

    .native-export-summary:focus-visible,
    .native-export-summary .dialog-results:focus-visible {
        outline: 2px solid var(--state-focus);
        outline-offset: -2px;
    }
</style>

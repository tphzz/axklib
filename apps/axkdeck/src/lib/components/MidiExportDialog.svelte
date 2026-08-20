<script lang="ts">
    import { modal } from '../modal';
    import type { PackageExportSelection } from '../types';
    import ExportDestinationChooser from './ExportDestinationChooser.svelte';
    import Icon from './Icon.svelte';

    interface Props {
        items: PackageExportSelection[];
        desktop: boolean;
        error: string;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let { items, desktop, error, onworkspace, onlocal, oncancel }: Props = $props();
    const singleItem = $derived(items.length === 1 ? items[0] : undefined);
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export MIDI"
        use:modal={{ onescape: oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="music" size={16} />
                <h2>Export MIDI</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="MIDI export destination">
                <h3>{singleItem ? `Export “${singleItem.name}”` : `Export ${items.length} Sequences`}</h3>
                <p>
                    {singleItem
                        ? 'The selected Sequence will be saved as a standard MIDI file.'
                        : 'Each selected Sequence will be saved as a standard MIDI file in one folder.'}
                </p>
                <ExportDestinationChooser {desktop} {onworkspace} {onlocal} />
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

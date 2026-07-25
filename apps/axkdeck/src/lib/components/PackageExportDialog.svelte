<script lang="ts">
    import { modal } from '../modal';
    import type { PackageExportSelection } from '../types';
    import Icon from './Icon.svelte';

    interface Props {
        items: PackageExportSelection[];
        desktop: boolean;
        busy: boolean;
        progressLabel: string;
        error: string;
        onworkspace: () => void;
        onlocal: () => void;
        oncancel: () => void;
    }

    let { items, desktop, busy, progressLabel, error, onworkspace, onlocal, oncancel }: Props = $props();
    const singleItem = $derived(items.length === 1 ? items[0] : undefined);
    const typeSummary = $derived.by(() => {
        const labels = ['Program', 'Sample Bank', 'Sample', 'Wave Data'] as const;
        return labels
            .map((label) => {
                const count = items.filter((item) => item.typeLabel === label).length;
                if (count === 0) return '';
                const plural = label === 'Wave Data' ? label : count === 1 ? label : `${label}s`;
                return `${count} ${plural}`;
            })
            .filter(Boolean)
            .join(' · ');
    });
    const locationGroups = $derived.by(() => {
        const groups = new Map<string, { key: string; label: string; items: PackageExportSelection[] }>();
        for (const item of items) {
            const partitionName =
                'partitionName' in item && item.partitionName ? item.partitionName : `Partition ${item.partitionIndex}`;
            const numericPartitionName = `Partition ${item.partitionIndex}`;
            const partitionLabel =
                partitionName === numericPartitionName ? partitionName : `${partitionName} [${numericPartitionName}]`;
            const key = `${item.partitionIndex}\u0000${item.volumeName}`;
            const group = groups.get(key) ?? { key, label: `${partitionLabel} · ${item.volumeName}`, items: [] };
            group.items.push(item);
            groups.set(key, group);
        }
        return [...groups.values()];
    });
</script>

<div class="dialog-backdrop" role="presentation">
    <div
        class="dialog-shell package-export-dialog"
        role="dialog"
        aria-modal="true"
        aria-label="Export axklib package"
        aria-busy={busy}
        use:modal={{ onescape: busy ? undefined : oncancel }}
    >
        <header class="dialog-header">
            <div>
                <Icon name="archive" size={16} />
                <h2>Export package</h2>
            </div>
            <button class="icon-button" type="button" aria-label="Close" disabled={busy} onclick={oncancel}>×</button>
        </header>

        <div class="package-dialog-content">
            <section class="package-source-choice" aria-label="Package destination">
                <h3>{singleItem ? `Export “${singleItem.name}”` : `Export ${items.length} objects`}</h3>
                <p>
                    {singleItem
                        ? `The ${singleItem.typeLabel.toLocaleLowerCase()} and its complete dependency graph will be saved as an axklib package.`
                        : 'The selected objects and their combined dependency graph will be saved as one axklib package.'}
                </p>
                {#if items.length > 1}
                    <p class="package-export-type-summary">{typeSummary}</p>
                    <div class="package-export-items" aria-label="Selected objects">
                        {#each locationGroups as group (group.key)}
                            <section class="package-export-group">
                                <h4>{group.label}</h4>
                                {#each group.items as item (`${item.kind}:${'objectId' in item ? item.objectId : `${item.partitionIndex}:${item.volumeName}`}`)}
                                    <div><strong>{item.name}</strong><span>{item.typeLabel}</span></div>
                                {/each}
                            </section>
                        {/each}
                    </div>
                {/if}
                <button class="source-choice-button" type="button" disabled={busy} onclick={onworkspace}>
                    <Icon name="folder-open" size={18} />
                    <span><strong>Storage location</strong><small>Save to a configured workspace</small></span>
                </button>
                {#if desktop}
                    <button class="source-choice-button" type="button" disabled={busy} onclick={onlocal}>
                        <Icon name="hard-drive" size={18} />
                        <span><strong>This computer</strong><small>Save with the desktop file chooser</small></span>
                    </button>
                {/if}
                {#if busy}<p class="dialog-progress" role="status">{progressLabel || 'Exporting package…'}</p>{/if}
                {#if error}<p class="dialog-error" role="alert">{error}</p>{/if}
            </section>
        </div>

        <footer class="dialog-footer">
            <button class="secondary-button" type="button" disabled={busy} onclick={oncancel}>Cancel</button>
        </footer>
    </div>
</div>

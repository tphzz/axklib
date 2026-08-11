<script lang="ts">
    import { onMount } from 'svelte';
    import Icon from './Icon.svelte';

    interface Props {
        count: number;
        onexportpackage?: () => void;
        onexportsfz?: () => void;
        onexportmidi?: () => void;
        ondelete?: () => void;
        onclear: () => void;
    }

    let { count, onexportpackage, onexportsfz, onexportmidi, ondelete, onclear }: Props = $props();
    let exportMenuOpen = $state(false);
    let exportMenu = $state<HTMLDivElement>();

    onMount(() => {
        const closeFromOutside = (event: PointerEvent): void => {
            if (!exportMenuOpen || exportMenu?.contains(event.target as Node)) return;
            exportMenuOpen = false;
        };
        window.addEventListener('pointerdown', closeFromOutside, true);
        return () => window.removeEventListener('pointerdown', closeFromOutside, true);
    });
</script>

<svelte:window onkeydown={(event) => event.key === 'Escape' && (exportMenuOpen = false)} />

<div class="package-selection-controls" aria-label="Object selection">
    <span role="status">{count} selected</span>
    {#if onexportpackage || onexportsfz || onexportmidi}
        <div class="package-selection-export-menu" bind:this={exportMenu}>
            <button
                class="package-selection-action package-selection-export"
                type="button"
                aria-label={`Export ${count} selected ${count === 1 ? 'object' : 'objects'}`}
                aria-haspopup={[onexportpackage, onexportsfz, onexportmidi].filter(Boolean).length > 1
                    ? 'menu'
                    : undefined}
                aria-expanded={[onexportpackage, onexportsfz, onexportmidi].filter(Boolean).length > 1
                    ? exportMenuOpen
                    : undefined}
                title="Export selected objects"
                onclick={() => {
                    if ([onexportpackage, onexportsfz, onexportmidi].filter(Boolean).length > 1)
                        exportMenuOpen = !exportMenuOpen;
                    else if (onexportpackage) onexportpackage();
                    else if (onexportsfz) onexportsfz();
                    else onexportmidi?.();
                }}
            >
                <Icon name="archive" size={14} /><span>Export</span>
            </button>
            {#if exportMenuOpen}
                <div class="tree-context-menu package-selection-menu" role="menu" aria-label="Export format">
                    {#if onexportpackage}
                        <button
                            type="button"
                            role="menuitem"
                            onclick={() => {
                                exportMenuOpen = false;
                                onexportpackage?.();
                            }}>Export package…</button
                        >
                    {/if}
                    {#if onexportsfz}
                        <button
                            type="button"
                            role="menuitem"
                            onclick={() => {
                                exportMenuOpen = false;
                                onexportsfz?.();
                            }}>Export SFZ…</button
                        >
                    {/if}
                    {#if onexportmidi}
                        <button
                            type="button"
                            role="menuitem"
                            onclick={() => {
                                exportMenuOpen = false;
                                onexportmidi?.();
                            }}>Export MIDI…</button
                        >
                    {/if}
                </div>
            {/if}
        </div>
    {/if}
    {#if ondelete}
        <button
            class="package-selection-action package-selection-delete"
            type="button"
            aria-label={`Delete ${count} selected ${count === 1 ? 'object' : 'objects'}`}
            title="Delete selected objects"
            onclick={ondelete}
        >
            <Icon name="trash" size={14} /><span>Delete</span>
        </button>
    {/if}
    <button
        class="icon-button"
        type="button"
        aria-label="Clear object selection"
        title="Clear object selection"
        onclick={onclear}
    >
        <Icon name="close" size={13} />
    </button>
</div>

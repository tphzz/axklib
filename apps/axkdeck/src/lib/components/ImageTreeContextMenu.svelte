<script lang="ts">
    import { flushSync, onDestroy, onMount } from 'svelte';
    import type { DiskTreeItem, ImageTreeAction } from '../types';
    import Icon from './Icon.svelte';

    interface Props {
        item: DiskTreeItem;
        left: number;
        top: number;
        volumeActionsEnabled: boolean;
        partitionActionsEnabled: boolean;
        packageImportEnabled: boolean;
        packageExportEnabled: boolean;
        volumePackageExportEnabled: boolean;
        volumeFloppyExportEnabled: boolean;
        audioExportEnabled: boolean;
        mediaConversionEnabled: boolean;
        allocationInspectionEnabled: boolean;
        onaction: (action: ImageTreeAction) => void;
        onclose: () => void;
    }

    type Submenu = 'import' | 'export';

    let {
        item,
        left,
        top,
        volumeActionsEnabled,
        partitionActionsEnabled,
        packageImportEnabled,
        packageExportEnabled,
        volumePackageExportEnabled,
        volumeFloppyExportEnabled,
        audioExportEnabled,
        mediaConversionEnabled,
        allocationInspectionEnabled,
        onaction,
        onclose,
    }: Props = $props();
    let rootMenu = $state<HTMLDivElement>();
    let submenuMenu = $state<HTMLDivElement>();
    let importParent = $state<HTMLButtonElement>();
    let exportParent = $state<HTMLButtonElement>();
    let rootLeft = $state(0);
    let rootTop = $state(0);
    let submenuLeft = $state(0);
    let submenuTop = $state(0);
    let rootPositioned = $state(false);
    let submenuPositioned = $state(false);
    let activeSubmenu = $state<Submenu | null>(null);
    const invoker =
        typeof document !== 'undefined' && document.activeElement instanceof HTMLElement
            ? document.activeElement
            : null;
    const partitionHasImport = $derived(item.kind === 'partition' && packageImportEnabled);
    const partitionHasExport = $derived(
        item.kind === 'partition' &&
            (volumePackageExportEnabled || volumeFloppyExportEnabled || mediaConversionEnabled),
    );
    const partitionHasMutation = $derived(
        item.kind === 'partition' && (partitionActionsEnabled || volumeActionsEnabled),
    );
    const partitionHasTools = $derived(
        item.kind === 'partition' && (allocationInspectionEnabled || partitionActionsEnabled),
    );

    function directMenuItems(menu: HTMLDivElement | undefined): HTMLButtonElement[] {
        if (!menu) return [];
        return Array.from(menu.children).filter(
            (child): child is HTMLButtonElement =>
                child instanceof HTMLButtonElement && child.getAttribute('role') === 'menuitem',
        );
    }

    function focusItem(menu: HTMLDivElement | undefined, index: number): void {
        const items = directMenuItems(menu);
        if (items.length === 0) return;
        const normalized = (index + items.length) % items.length;
        items.forEach((menuItem, itemIndex) => (menuItem.tabIndex = itemIndex === normalized ? 0 : -1));
        items[normalized].focus();
    }

    function clamp(value: number, minimum: number, maximum: number): number {
        return Math.max(minimum, Math.min(value, maximum));
    }

    function positionRoot(): void {
        if (!rootMenu) return;
        rootLeft = clamp(left, 8, Math.max(8, window.innerWidth - rootMenu.offsetWidth - 8));
        rootTop = clamp(top, 8, Math.max(8, window.innerHeight - rootMenu.offsetHeight - 8));
    }

    function positionSubmenu(parent: HTMLButtonElement | undefined): void {
        if (!submenuMenu || !parent) return;
        const parentRect = parent.getBoundingClientRect();
        const preferredLeft = parentRect.right + 2;
        submenuLeft =
            preferredLeft + submenuMenu.offsetWidth <= window.innerWidth - 8
                ? preferredLeft
                : Math.max(8, parentRect.left - submenuMenu.offsetWidth - 2);
        submenuTop = clamp(parentRect.top, 8, Math.max(8, window.innerHeight - submenuMenu.offsetHeight - 8));
    }

    function submenuParent(kind: Submenu): HTMLButtonElement | undefined {
        return kind === 'import' ? importParent : exportParent;
    }

    function openSubmenu(kind: Submenu, focusFirst: boolean): void {
        activeSubmenu = kind;
        submenuPositioned = false;
        flushSync();
        positionSubmenu(submenuParent(kind));
        submenuPositioned = true;
        flushSync();
        if (focusFirst) focusItem(submenuMenu, 0);
    }

    function closeSubmenu(restoreParent: boolean): void {
        const parent = activeSubmenu ? submenuParent(activeSubmenu) : undefined;
        activeSubmenu = null;
        submenuPositioned = false;
        if (restoreParent) queueMicrotask(() => parent?.focus());
    }

    function handleRootKey(event: KeyboardEvent): void {
        const items = directMenuItems(rootMenu);
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
        const submenu = (document.activeElement as HTMLElement | null)?.dataset.submenu as Submenu | undefined;
        if (event.key === 'Escape') {
            event.preventDefault();
            onclose();
        } else if (event.key === 'ArrowDown') {
            event.preventDefault();
            closeSubmenu(false);
            focusItem(rootMenu, current + 1);
        } else if (event.key === 'ArrowUp') {
            event.preventDefault();
            closeSubmenu(false);
            focusItem(rootMenu, current - 1);
        } else if (event.key === 'Home') {
            event.preventDefault();
            closeSubmenu(false);
            focusItem(rootMenu, 0);
        } else if (event.key === 'End') {
            event.preventDefault();
            closeSubmenu(false);
            focusItem(rootMenu, items.length - 1);
        } else if (event.key === 'ArrowRight' && submenu) {
            event.preventDefault();
            openSubmenu(submenu, true);
        }
        event.stopPropagation();
    }

    function handleSubmenuKey(event: KeyboardEvent): void {
        const items = directMenuItems(submenuMenu);
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'Escape' || event.key === 'ArrowLeft') {
            event.preventDefault();
            closeSubmenu(true);
        } else if (event.key === 'ArrowDown') {
            event.preventDefault();
            focusItem(submenuMenu, current + 1);
        } else if (event.key === 'ArrowUp') {
            event.preventDefault();
            focusItem(submenuMenu, current - 1);
        } else if (event.key === 'Home') {
            event.preventDefault();
            focusItem(submenuMenu, 0);
        } else if (event.key === 'End') {
            event.preventDefault();
            focusItem(submenuMenu, items.length - 1);
        }
        event.stopPropagation();
    }

    function choose(action: ImageTreeAction): void {
        onaction(action);
        onclose();
    }

    onMount(() => {
        positionRoot();
        rootPositioned = true;
        flushSync();
        focusItem(rootMenu, 0);
        const dismissFromOutsidePointer = (event: PointerEvent): void => {
            const path = event.composedPath();
            if ((rootMenu && path.includes(rootMenu)) || (submenuMenu && path.includes(submenuMenu))) return;
            onclose();
        };
        const reposition = (): void => {
            positionRoot();
            if (activeSubmenu) positionSubmenu(submenuParent(activeSubmenu));
        };
        window.addEventListener('pointerdown', dismissFromOutsidePointer, true);
        window.addEventListener('resize', reposition);
        return () => {
            window.removeEventListener('pointerdown', dismissFromOutsidePointer, true);
            window.removeEventListener('resize', reposition);
        };
    });

    onDestroy(() => {
        if (invoker?.isConnected) invoker.focus();
    });
</script>

<div
    bind:this={rootMenu}
    class="tree-context-menu"
    role="menu"
    aria-label={`${item.name} actions`}
    tabindex="-1"
    style={`left: ${rootLeft}px; top: ${rootTop}px; visibility: ${rootPositioned ? 'visible' : 'hidden'}; pointer-events: ${rootPositioned ? 'auto' : 'none'};`}
    onclick={(event) => event.stopPropagation()}
    onkeydown={handleRootKey}
>
    {#if item.kind === 'partition'}
        {#if partitionHasImport}
            <button
                bind:this={importParent}
                class="context-submenu-trigger"
                type="button"
                role="menuitem"
                aria-haspopup="menu"
                aria-expanded={activeSubmenu === 'import'}
                data-submenu="import"
                onmouseenter={() => openSubmenu('import', false)}
                onclick={() => openSubmenu('import', true)}
            >
                <span>Import</span><Icon name="chevron" size={13} />
            </button>
        {/if}
        {#if partitionHasExport}
            <button
                bind:this={exportParent}
                class="context-submenu-trigger"
                type="button"
                role="menuitem"
                aria-haspopup="menu"
                aria-expanded={activeSubmenu === 'export'}
                data-submenu="export"
                onmouseenter={() => openSubmenu('export', false)}
                onclick={() => openSubmenu('export', true)}
            >
                <span>Export</span><Icon name="chevron" size={13} />
            </button>
        {/if}
        {#if (partitionHasImport || partitionHasExport) && (partitionHasMutation || partitionHasTools)}
            <div class="context-menu-separator" role="separator"></div>
        {/if}
        {#if partitionActionsEnabled}
            <button
                type="button"
                role="menuitem"
                onmouseenter={() => closeSubmenu(false)}
                onclick={() => choose('rename-partition')}>Rename partition…</button
            >
        {/if}
        {#if volumeActionsEnabled}
            <button
                type="button"
                role="menuitem"
                onmouseenter={() => closeSubmenu(false)}
                onclick={() => choose('add-volume')}>Add volume…</button
            >
        {/if}
        {#if partitionHasMutation && partitionHasTools}
            <div class="context-menu-separator" role="separator"></div>
        {/if}
        {#if allocationInspectionEnabled}
            <button
                type="button"
                role="menuitem"
                onmouseenter={() => closeSubmenu(false)}
                onclick={() => choose('inspect-allocation')}>Visualize partition allocation</button
            >
        {/if}
        {#if partitionActionsEnabled}
            <button
                type="button"
                role="menuitem"
                onmouseenter={() => closeSubmenu(false)}
                onclick={() => choose('repair-placement')}>Repair object placement…</button
            >
        {/if}
    {:else}
        {#if packageImportEnabled}
            <button type="button" role="menuitem" onclick={() => choose('import-package')}>Import package…</button>
        {/if}
        {#if packageExportEnabled}
            <button type="button" role="menuitem" onclick={() => choose('export-package')}>Export package…</button>
        {/if}
        {#if audioExportEnabled}
            <button type="button" role="menuitem" onclick={() => choose('export-sfz')}>Export SFZ…</button>
        {/if}
        {#if mediaConversionEnabled && item.volumeDirectoryId !== undefined}
            <button type="button" role="menuitem" onclick={() => choose('export-floppy')}>Export floppy image…</button>
        {/if}
        {#if volumeActionsEnabled}
            {#if packageImportEnabled || packageExportEnabled || audioExportEnabled || mediaConversionEnabled}
                <div class="context-menu-separator" role="separator"></div>
            {/if}
            <button type="button" role="menuitem" onclick={() => choose('repair-placement')}
                >Repair object placement…</button
            >
            <button type="button" role="menuitem" onclick={() => choose('rename-volume')}>Rename volume…</button>
            <button class="danger-menu-item" type="button" role="menuitem" onclick={() => choose('delete-volume')}
                >Delete volume</button
            >
        {/if}
    {/if}
</div>

{#if activeSubmenu}
    <div
        bind:this={submenuMenu}
        class="tree-context-menu tree-context-submenu"
        role="menu"
        aria-label={`${activeSubmenu === 'import' ? 'Import' : 'Export'} actions`}
        tabindex="-1"
        style={`left: ${submenuLeft}px; top: ${submenuTop}px; visibility: ${submenuPositioned ? 'visible' : 'hidden'}; pointer-events: ${submenuPositioned ? 'auto' : 'none'};`}
        onclick={(event) => event.stopPropagation()}
        onkeydown={handleSubmenuKey}
    >
        {#if activeSubmenu === 'import'}
            <button type="button" role="menuitem" onclick={() => choose('import-packages')}>Import packages…</button>
        {:else}
            {#if volumePackageExportEnabled}
                <button type="button" role="menuitem" onclick={() => choose('export-volume-packages')}
                    >Export volume packages…</button
                >
            {/if}
            {#if volumeFloppyExportEnabled}
                <button type="button" role="menuitem" onclick={() => choose('export-volume-floppies')}
                    >Export volumes to floppies…</button
                >
            {/if}
            {#if mediaConversionEnabled}
                <button type="button" role="menuitem" onclick={() => choose('export-cdrom')}
                    >Export CD-ROM image…</button
                >
            {/if}
        {/if}
    </div>
{/if}

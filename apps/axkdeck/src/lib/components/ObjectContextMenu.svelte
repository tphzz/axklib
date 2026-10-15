<script lang="ts">
    import { flushSync, onDestroy, onMount } from 'svelte';
    import Icon from './Icon.svelte';

    interface Props {
        objectName: string;
        selectionCount?: number;
        selectionNoun?: string;
        oncreatedirectory?: () => void;
        onimportfiles?: () => void;
        onimportdirectory?: () => void;
        additionalImports?: { label: string; action: () => void }[];
        left: number;
        top: number;
        onrename?: () => void;
        onduplicate?: () => void;
        onconvert?: () => void;
        convertLabel?: string;
        onassignsamplebank?: () => void;
        onexportpackage?: () => void;
        onexportwav?: () => void;
        onexportsfz?: () => void;
        onexportmidi?: () => void;
        onexportfiles?: () => void;
        ondelete?: () => void;
        onclose: () => void;
    }

    let {
        objectName,
        selectionCount = 1,
        selectionNoun = 'objects',
        oncreatedirectory,
        onimportfiles,
        onimportdirectory,
        additionalImports = [],
        left,
        top,
        onrename,
        onduplicate,
        onconvert,
        convertLabel = 'Convert Sample format...',
        onassignsamplebank,
        onexportpackage,
        onexportwav,
        onexportsfz,
        onexportmidi,
        onexportfiles,
        ondelete,
        onclose,
    }: Props = $props();
    let rootMenu = $state<HTMLDivElement>();
    let submenuMenu = $state<HTMLDivElement>();
    let exportParent = $state<HTMLButtonElement>();
    let importParent = $state<HTMLButtonElement>();
    let rootLeft = $state(0);
    let rootTop = $state(0);
    let submenuLeft = $state(0);
    let submenuTop = $state(0);
    let rootPositioned = $state(false);
    let submenuPositioned = $state(false);
    let openKind = $state<'import' | 'export' | null>(null);
    const invoker =
        typeof document !== 'undefined' && document.activeElement instanceof HTMLElement
            ? document.activeElement
            : null;
    const hasMutations = $derived(
        Boolean(oncreatedirectory || onrename || onduplicate || onconvert || onassignsamplebank || ondelete),
    );
    const hasExports = $derived(
        Boolean(onexportpackage || onexportwav || onexportsfz || onexportmidi || onexportfiles),
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
        items.forEach((item, itemIndex) => (item.tabIndex = itemIndex === normalized ? 0 : -1));
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

    function positionSubmenu(): void {
        const parent = openKind === 'import' ? importParent : exportParent;
        if (!submenuMenu || !parent) return;
        const parentRect = parent.getBoundingClientRect();
        const preferredLeft = parentRect.right + 2;
        submenuLeft =
            preferredLeft + submenuMenu.offsetWidth <= window.innerWidth - 8
                ? preferredLeft
                : Math.max(8, parentRect.left - submenuMenu.offsetWidth - 2);
        submenuTop = clamp(parentRect.top, 8, Math.max(8, window.innerHeight - submenuMenu.offsetHeight - 8));
    }

    function openTransfer(kind: 'import' | 'export', focusFirst: boolean): void {
        openKind = kind;
        submenuPositioned = false;
        flushSync();
        positionSubmenu();
        submenuPositioned = true;
        flushSync();
        if (focusFirst) focusItem(submenuMenu, 0);
    }

    function closeSubmenu(restoreParent: boolean): void {
        const parent = openKind === 'import' ? importParent : exportParent;
        openKind = null;
        submenuPositioned = false;
        if (restoreParent) queueMicrotask(() => parent?.focus());
    }

    function handleRootKey(event: KeyboardEvent): void {
        const items = directMenuItems(rootMenu);
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
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
        } else if (
            event.key === 'ArrowRight' &&
            (document.activeElement === exportParent || document.activeElement === importParent)
        ) {
            event.preventDefault();
            openTransfer(document.activeElement === importParent ? 'import' : 'export', true);
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

    function choose(action: (() => void) | undefined): void {
        action?.();
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
            if (openKind) positionSubmenu();
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
    aria-label={`${objectName} actions`}
    tabindex="-1"
    style={`left: ${rootLeft}px; top: ${rootTop}px; visibility: ${rootPositioned ? 'visible' : 'hidden'}; pointer-events: ${rootPositioned ? 'auto' : 'none'};`}
    onclick={(event) => event.stopPropagation()}
    onkeydown={handleRootKey}
>
    {#if oncreatedirectory}
        <button
            type="button"
            role="menuitem"
            onmouseenter={() => closeSubmenu(false)}
            onclick={() => choose(oncreatedirectory)}>New directory...</button
        >
    {/if}
    {#if onrename}
        <button type="button" role="menuitem" onmouseenter={() => closeSubmenu(false)} onclick={() => choose(onrename)}
            >Rename…</button
        >
    {/if}
    {#if onconvert}<button
            class="context-menu-item"
            type="button"
            role="menuitem"
            tabindex="-1"
            onclick={() => choose(onconvert)}>{convertLabel}</button
        >{/if}
    {#if onduplicate}
        <button
            type="button"
            role="menuitem"
            onmouseenter={() => closeSubmenu(false)}
            onclick={() => choose(onduplicate)}>Duplicate...</button
        >
    {/if}
    {#if onassignsamplebank}
        <button
            type="button"
            role="menuitem"
            onmouseenter={() => closeSubmenu(false)}
            onclick={() => choose(onassignsamplebank)}>Assign to Sample Bank…</button
        >
    {/if}
    {#if ondelete}
        <button
            class="danger-menu-item"
            type="button"
            role="menuitem"
            onmouseenter={() => closeSubmenu(false)}
            onclick={() => choose(ondelete)}
            >{selectionCount === 1 ? 'Delete…' : `Delete ${selectionCount} ${selectionNoun}…`}</button
        >
    {/if}
    {#if hasMutations && (hasExports || onimportfiles || onimportdirectory || additionalImports.length)}
        <div class="context-menu-separator" role="separator"></div>
    {/if}
    {#if onimportfiles || onimportdirectory || additionalImports.length}
        <button
            bind:this={importParent}
            class="context-submenu-trigger"
            type="button"
            role="menuitem"
            aria-haspopup="menu"
            aria-expanded={openKind === 'import'}
            onmouseenter={() => openTransfer('import', false)}
            onclick={() => openTransfer('import', true)}
        >
            <span>Import</span><Icon name="chevron" size={13} />
        </button>
    {/if}
    {#if hasExports}
        <button
            bind:this={exportParent}
            class="context-submenu-trigger"
            type="button"
            role="menuitem"
            aria-haspopup="menu"
            aria-expanded={openKind === 'export'}
            onmouseenter={() => openTransfer('export', false)}
            onclick={() => openTransfer('export', true)}
        >
            <span>Export</span><Icon name="chevron" size={13} />
        </button>
    {/if}
</div>

{#if openKind}
    <div
        bind:this={submenuMenu}
        class="tree-context-menu tree-context-submenu"
        role="menu"
        aria-label={openKind === 'import' ? 'Import actions' : 'Export actions'}
        tabindex="-1"
        style={`left: ${submenuLeft}px; top: ${submenuTop}px; visibility: ${submenuPositioned ? 'visible' : 'hidden'}; pointer-events: ${submenuPositioned ? 'auto' : 'none'};`}
        onclick={(event) => event.stopPropagation()}
        onkeydown={handleSubmenuKey}
    >
        {#if openKind === 'import'}
            {#if onimportfiles}<button type="button" role="menuitem" onclick={() => choose(onimportfiles)}
                    >Add files...</button
                >{/if}
            {#if onimportdirectory}<button type="button" role="menuitem" onclick={() => choose(onimportdirectory)}
                    >Import from disk...</button
                >{/if}
            {#each additionalImports as item}<button type="button" role="menuitem" onclick={() => choose(item.action)}
                    >{item.label}</button
                >{/each}
        {:else}
            {#if onexportfiles}
                <button type="button" role="menuitem" onclick={() => choose(onexportfiles)}>Export to disk...</button>
            {/if}
            {#if onexportpackage}
                <button type="button" role="menuitem" onclick={() => choose(onexportpackage)}>Export package…</button>
            {/if}
            {#if onexportwav}
                <button type="button" role="menuitem" onclick={() => choose(onexportwav)}>Export WAV…</button>
            {/if}
            {#if onexportsfz}
                <button type="button" role="menuitem" onclick={() => choose(onexportsfz)}>Export SFZ…</button>
            {/if}
            {#if onexportmidi}
                <button type="button" role="menuitem" onclick={() => choose(onexportmidi)}>Export MIDI…</button>
            {/if}
        {/if}
    </div>
{/if}

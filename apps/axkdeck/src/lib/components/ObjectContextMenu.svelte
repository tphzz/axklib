<script lang="ts">
    import { flushSync, onDestroy, onMount } from 'svelte';
    import Icon from './Icon.svelte';

    interface Props {
        objectName: string;
        selectionCount?: number;
        left: number;
        top: number;
        onrename?: () => void;
        onassignsamplebank?: () => void;
        onexportpackage?: () => void;
        onexportwav?: () => void;
        onexportsfz?: () => void;
        onexportmidi?: () => void;
        ondelete?: () => void;
        onclose: () => void;
    }

    let {
        objectName,
        selectionCount = 1,
        left,
        top,
        onrename,
        onassignsamplebank,
        onexportpackage,
        onexportwav,
        onexportsfz,
        onexportmidi,
        ondelete,
        onclose,
    }: Props = $props();
    let rootMenu = $state<HTMLDivElement>();
    let submenuMenu = $state<HTMLDivElement>();
    let exportParent = $state<HTMLButtonElement>();
    let rootLeft = $state(0);
    let rootTop = $state(0);
    let submenuLeft = $state(0);
    let submenuTop = $state(0);
    let rootPositioned = $state(false);
    let submenuPositioned = $state(false);
    let exportOpen = $state(false);
    const invoker =
        typeof document !== 'undefined' && document.activeElement instanceof HTMLElement
            ? document.activeElement
            : null;
    const hasMutations = $derived(Boolean(onrename || onassignsamplebank || ondelete));
    const hasExports = $derived(Boolean(onexportpackage || onexportwav || onexportsfz || onexportmidi));

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
        if (!submenuMenu || !exportParent) return;
        const parentRect = exportParent.getBoundingClientRect();
        const preferredLeft = parentRect.right + 2;
        submenuLeft =
            preferredLeft + submenuMenu.offsetWidth <= window.innerWidth - 8
                ? preferredLeft
                : Math.max(8, parentRect.left - submenuMenu.offsetWidth - 2);
        submenuTop = clamp(parentRect.top, 8, Math.max(8, window.innerHeight - submenuMenu.offsetHeight - 8));
    }

    function openExport(focusFirst: boolean): void {
        exportOpen = true;
        submenuPositioned = false;
        flushSync();
        positionSubmenu();
        submenuPositioned = true;
        flushSync();
        if (focusFirst) focusItem(submenuMenu, 0);
    }

    function closeExport(restoreParent: boolean): void {
        exportOpen = false;
        submenuPositioned = false;
        if (restoreParent) queueMicrotask(() => exportParent?.focus());
    }

    function handleRootKey(event: KeyboardEvent): void {
        const items = directMenuItems(rootMenu);
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'Escape') {
            event.preventDefault();
            onclose();
        } else if (event.key === 'ArrowDown') {
            event.preventDefault();
            closeExport(false);
            focusItem(rootMenu, current + 1);
        } else if (event.key === 'ArrowUp') {
            event.preventDefault();
            closeExport(false);
            focusItem(rootMenu, current - 1);
        } else if (event.key === 'Home') {
            event.preventDefault();
            closeExport(false);
            focusItem(rootMenu, 0);
        } else if (event.key === 'End') {
            event.preventDefault();
            closeExport(false);
            focusItem(rootMenu, items.length - 1);
        } else if (event.key === 'ArrowRight' && document.activeElement === exportParent) {
            event.preventDefault();
            openExport(true);
        }
        event.stopPropagation();
    }

    function handleSubmenuKey(event: KeyboardEvent): void {
        const items = directMenuItems(submenuMenu);
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'Escape' || event.key === 'ArrowLeft') {
            event.preventDefault();
            closeExport(true);
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
            if (exportOpen) positionSubmenu();
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
    {#if onrename}
        <button type="button" role="menuitem" onmouseenter={() => closeExport(false)} onclick={() => choose(onrename)}
            >Rename…</button
        >
    {/if}
    {#if onassignsamplebank}
        <button
            type="button"
            role="menuitem"
            onmouseenter={() => closeExport(false)}
            onclick={() => choose(onassignsamplebank)}>Assign to Sample Bank…</button
        >
    {/if}
    {#if ondelete}
        <button
            class="danger-menu-item"
            type="button"
            role="menuitem"
            onmouseenter={() => closeExport(false)}
            onclick={() => choose(ondelete)}
            >{selectionCount === 1 ? 'Delete…' : `Delete ${selectionCount} objects…`}</button
        >
    {/if}
    {#if hasMutations && hasExports}
        <div class="context-menu-separator" role="separator"></div>
    {/if}
    {#if hasExports}
        <button
            bind:this={exportParent}
            class="context-submenu-trigger"
            type="button"
            role="menuitem"
            aria-haspopup="menu"
            aria-expanded={exportOpen}
            onmouseenter={() => openExport(false)}
            onclick={() => openExport(true)}
        >
            <span>Export</span><Icon name="chevron" size={13} />
        </button>
    {/if}
</div>

{#if exportOpen}
    <div
        bind:this={submenuMenu}
        class="tree-context-menu tree-context-submenu"
        role="menu"
        aria-label="Export actions"
        tabindex="-1"
        style={`left: ${submenuLeft}px; top: ${submenuTop}px; visibility: ${submenuPositioned ? 'visible' : 'hidden'}; pointer-events: ${submenuPositioned ? 'auto' : 'none'};`}
        onclick={(event) => event.stopPropagation()}
        onkeydown={handleSubmenuKey}
    >
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
    </div>
{/if}

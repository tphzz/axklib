<script lang="ts">
    import { onDestroy, onMount } from 'svelte';

    interface Props {
        objectName: string;
        selectionCount?: number;
        left: number;
        top: number;
        onrename?: () => void;
        oncreatesamplebank?: () => void;
        onassignsamplebank?: () => void;
        onexportpackage?: () => void;
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
        oncreatesamplebank,
        onassignsamplebank,
        onexportpackage,
        onexportsfz,
        onexportmidi,
        ondelete,
        onclose,
    }: Props = $props();
    let menu: HTMLDivElement;
    const invoker =
        typeof document !== 'undefined' && document.activeElement instanceof HTMLElement
            ? document.activeElement
            : null;

    function menuItems(): HTMLButtonElement[] {
        return menu ? Array.from(menu.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')) : [];
    }

    function focusItem(index: number): void {
        const items = menuItems();
        if (items.length === 0) return;
        const normalized = (index + items.length) % items.length;
        items.forEach((item, itemIndex) => (item.tabIndex = itemIndex === normalized ? 0 : -1));
        items[normalized].focus();
    }

    function handleMenuKey(event: KeyboardEvent): void {
        const items = menuItems();
        const current = items.indexOf(document.activeElement as HTMLButtonElement);
        if (event.key === 'Escape') {
            event.preventDefault();
            onclose();
        } else if (event.key === 'ArrowDown') {
            event.preventDefault();
            focusItem(current + 1);
        } else if (event.key === 'ArrowUp') {
            event.preventDefault();
            focusItem(current - 1);
        } else if (event.key === 'Home') {
            event.preventDefault();
            focusItem(0);
        } else if (event.key === 'End') {
            event.preventDefault();
            focusItem(items.length - 1);
        }
        event.stopPropagation();
    }

    $effect(() => {
        queueMicrotask(() => focusItem(0));
    });

    onMount(() => {
        const dismissFromOutsidePointer = (event: PointerEvent): void => {
            if (!menu || event.composedPath().includes(menu)) return;
            onclose();
        };
        window.addEventListener('pointerdown', dismissFromOutsidePointer, true);
        return () => window.removeEventListener('pointerdown', dismissFromOutsidePointer, true);
    });

    onDestroy(() => {
        if (invoker?.isConnected) invoker.focus();
    });
</script>

<div
    bind:this={menu}
    class="tree-context-menu"
    role="menu"
    aria-label={`${objectName} actions`}
    tabindex="-1"
    style={`left: ${left}px; top: ${top}px;`}
    onclick={(event) => event.stopPropagation()}
    onkeydown={handleMenuKey}
>
    {#if onrename}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onrename?.();
                onclose();
            }}>Rename</button
        >
    {/if}
    {#if oncreatesamplebank}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                oncreatesamplebank?.();
                onclose();
            }}>Create Sample Bank from selection…</button
        >
    {/if}
    {#if onassignsamplebank}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onassignsamplebank?.();
                onclose();
            }}>Assign to Sample Bank…</button
        >
    {/if}
    {#if onexportpackage}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onexportpackage?.();
                onclose();
            }}>Export package…</button
        >
    {/if}
    {#if onexportsfz}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onexportsfz?.();
                onclose();
            }}>Export SFZ…</button
        >
    {/if}
    {#if onexportmidi}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onexportmidi?.();
                onclose();
            }}>Export MIDI…</button
        >
    {/if}
    {#if (onrename || oncreatesamplebank || onassignsamplebank || onexportpackage || onexportsfz || onexportmidi) && ondelete}
        <div class="context-menu-separator" role="separator"></div>
    {/if}
    {#if ondelete}
        <button
            class="danger-menu-item"
            type="button"
            role="menuitem"
            onclick={() => {
                ondelete?.();
                onclose();
            }}>Delete {selectionCount === 1 ? '' : `${selectionCount} objects`}</button
        >
    {/if}
</div>

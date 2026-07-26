<script lang="ts">
    import { onMount } from 'svelte';

    interface Props {
        objectName: string;
        selectionCount?: number;
        left: number;
        top: number;
        onrename?: () => void;
        onexport?: () => void;
        ondelete?: () => void;
        onclose: () => void;
    }

    let { objectName, selectionCount = 1, left, top, onrename, onexport, ondelete, onclose }: Props = $props();
    let menu: HTMLDivElement;

    $effect(() => {
        queueMicrotask(() => menu?.querySelector<HTMLButtonElement>('[role="menuitem"]')?.focus());
    });

    onMount(() => {
        const dismissFromOutsidePointer = (event: PointerEvent): void => {
            if (!menu || event.composedPath().includes(menu)) return;
            onclose();
        };
        window.addEventListener('pointerdown', dismissFromOutsidePointer, true);
        return () => window.removeEventListener('pointerdown', dismissFromOutsidePointer, true);
    });
</script>

<svelte:window onkeydown={(event) => event.key === 'Escape' && onclose()} />

<div
    bind:this={menu}
    class="tree-context-menu"
    role="menu"
    aria-label={`${objectName} actions`}
    tabindex="-1"
    style={`left: ${left}px; top: ${top}px;`}
    onclick={(event) => event.stopPropagation()}
    onkeydown={(event) => event.stopPropagation()}
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
    {#if onexport}
        <button
            type="button"
            role="menuitem"
            onclick={() => {
                onexport?.();
                onclose();
            }}>Export {selectionCount === 1 ? 'package' : `${selectionCount} objects`}</button
        >
    {/if}
    {#if (onrename || onexport) && ondelete}<div class="context-menu-separator" role="separator"></div>{/if}
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

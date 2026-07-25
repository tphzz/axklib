<script lang="ts">
    interface Props {
        objectName: string;
        selectionCount?: number;
        left: number;
        top: number;
        onexport?: () => void;
        ondelete?: () => void;
        onclose: () => void;
    }

    let { objectName, selectionCount = 1, left, top, onexport, ondelete, onclose }: Props = $props();
    let menu: HTMLDivElement;

    $effect(() => {
        queueMicrotask(() => menu?.querySelector<HTMLButtonElement>('[role="menuitem"]')?.focus());
    });
</script>

<svelte:window onclick={onclose} onkeydown={(event) => event.key === 'Escape' && onclose()} />

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
    {#if onexport && ondelete}<div class="context-menu-separator" role="separator"></div>{/if}
    {#if ondelete}
        <button
            class="danger-menu-item"
            type="button"
            role="menuitem"
            onclick={() => {
                ondelete?.();
                onclose();
            }}>Delete</button
        >
    {/if}
</div>

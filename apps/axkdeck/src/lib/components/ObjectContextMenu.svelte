<script lang="ts">
    interface Props {
        objectName: string;
        left: number;
        top: number;
        ondelete: () => void;
        onclose: () => void;
    }

    let { objectName, left, top, ondelete, onclose }: Props = $props();
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
    <button
        class="danger-menu-item"
        type="button"
        role="menuitem"
        onclick={() => {
            ondelete();
            onclose();
        }}>Delete</button
    >
</div>

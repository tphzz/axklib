<script lang="ts">
    import { onMount } from 'svelte';
    import { on } from 'svelte/events';
    let {
        note,
        x,
        y,
        formatNote,
        onchange,
        onclose,
    }: {
        note: number;
        x: number;
        y: number;
        formatNote: (note: number) => string;
        onchange: (note: number) => void;
        onclose: () => void;
    } = $props();
    let menu: HTMLDivElement;
    let action: HTMLButtonElement;
    let left = $state(0),
        top = $state(0);
    onMount(() => {
        const previous =
            document.activeElement instanceof HTMLElement || document.activeElement instanceof SVGElement
                ? document.activeElement
                : null;
        function position() {
            const box = menu.getBoundingClientRect();
            const scale = menu.offsetWidth ? box.width / menu.offsetWidth || 1 : 1;
            left = Math.max(8, Math.min(x, window.innerWidth - box.width - 8)) / scale;
            top = Math.max(8, Math.min(y, window.innerHeight - box.height - 8)) / scale;
        }
        position();
        action.focus();
        const stop = on(
            window,
            'pointerdown',
            (event) => {
                if (event.target instanceof Node && !menu.contains(event.target)) onclose();
            },
            { capture: true },
        );
        const resize = on(window, 'resize', onclose);
        const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(position);
        observer?.observe(menu);
        return () => {
            stop();
            resize();
            observer?.disconnect();
            previous?.focus();
        };
    });
    function key(event: KeyboardEvent) {
        if (event.key === 'Escape' || event.key === 'Tab') {
            event.preventDefault();
            onclose();
        } else if (['ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) {
            event.preventDefault();
            action.focus();
        }
    }
</script>

<div
    bind:this={menu}
    class="tree-context-menu"
    role="menu"
    tabindex="-1"
    aria-label="Root key"
    style:left={`${left}px`}
    style:top={`${top}px`}
    onkeydown={key}
>
    <button
        bind:this={action}
        role="menuitem"
        onclick={() => {
            onchange(note);
            onclose();
        }}>Set root to {formatNote(note)}</button
    >
</div>

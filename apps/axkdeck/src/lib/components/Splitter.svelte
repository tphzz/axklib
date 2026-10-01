<script lang="ts">
    let {
        orientation,
        label,
        value,
        min = 0,
        max = 100,
        onresize,
        onstep,
        onreset,
    }: {
        orientation: 'horizontal' | 'vertical';
        label: string;
        value: number;
        min?: number;
        max?: number;
        onresize: (event: PointerEvent) => void;
        onstep: (key: string) => void;
        onreset?: () => void;
    } = $props();
    let pointer = $state<number | null>(null);
</script>

<!-- Svelte does not model the interactive ARIA separator pattern. -->
<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<div
    class="pane-splitter"
    class:dragging={pointer !== null}
    role="separator"
    tabindex="0"
    aria-label={label}
    aria-orientation={orientation}
    aria-valuenow={Math.round(value)}
    aria-valuemin={Math.round(min)}
    aria-valuemax={Math.round(max)}
    onpointerdown={(event) => {
        if (event.button !== 0 || pointer !== null) return;
        event.preventDefault();
        event.currentTarget.focus();
        pointer = event.pointerId;
        event.currentTarget.setPointerCapture(event.pointerId);
        onresize(event);
    }}
    onpointermove={(event) => {
        if (pointer === event.pointerId) onresize(event);
    }}
    onpointerup={(event) => {
        if (pointer !== event.pointerId) return;
        onresize(event);
        pointer = null;
        event.currentTarget.releasePointerCapture(event.pointerId);
    }}
    onpointercancel={() => (pointer = null)}
    onlostpointercapture={() => (pointer = null)}
    ondblclick={onreset}
    onkeydown={(event) => {
        const arrows = orientation === 'vertical' ? ['ArrowLeft', 'ArrowRight'] : ['ArrowUp', 'ArrowDown'];
        if (![...arrows, 'Home', 'End'].includes(event.key)) return;
        event.preventDefault();
        onstep(event.key);
    }}
></div>

<style>
    .pane-splitter {
        position: relative;
        min-width: 0;
        min-height: 0;
        touch-action: none;
        user-select: none;
        outline: none;
    }
    .pane-splitter::before {
        content: '';
        position: absolute;
        background: var(--color-border);
        pointer-events: none;
    }
    [aria-orientation='horizontal'] {
        height: 8px;
        cursor: row-resize;
    }
    [aria-orientation='horizontal']::before {
        inset-inline: 0;
        top: 50%;
        height: 1px;
    }
    [aria-orientation='vertical'] {
        width: 8px;
        cursor: col-resize;
    }
    [aria-orientation='vertical']::before {
        inset-block: 0;
        left: 50%;
        width: 1px;
    }
    .pane-splitter:is(:hover, :focus-visible, .dragging) {
        background: var(--color-selection);
    }
    .pane-splitter:is(:hover, :focus-visible, .dragging)::before {
        background: var(--color-accent);
    }
</style>

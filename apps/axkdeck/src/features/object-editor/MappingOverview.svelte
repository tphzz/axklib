<script lang="ts">
    import { onDestroy } from 'svelte';
    import { graphDrag } from './graphDrag';
    import { mappingFill, type MappingGeometry } from './keyboardGeometry';
    import { clampViewport, viewportKey, type MappingViewport } from './mappingViewport';
    let {
        start,
        span,
        geometry,
        selected,
        disabled = false,
        formatNote,
        onchange,
        controls,
    }: {
        start: number;
        span: number;
        geometry: MappingGeometry;
        selected: Set<string>;
        disabled?: boolean;
        formatNote: (note: number) => string;
        onchange: (view: MappingViewport) => void;
        controls: string;
    } = $props();
    const notes = Array.from({ length: 128 }, (_, note) => note);
    let rail: HTMLDivElement;
    let cancel: (() => void) | undefined;
    const colors = $derived(
        geometry.coverage.map((coverage) => {
            const zone = coverage.find((zone) => selected.has(zone.id)) ?? coverage[0];
            return zone ? mappingFill(geometry.colors.get(zone.id), selected.has(zone.id)) : 'transparent';
        }),
    );
    function pointer(event: PointerEvent) {
        if (disabled || event.button !== 0) return;
        event.preventDefault();
        cancel?.();
        rail.focus();
        const original = { start, span },
            box = rail.getBoundingClientRect();
        const initial = (event.target as Element).closest('.viewport')
            ? original
            : clampViewport({ start: ((event.clientX - box.left) / box.width) * 128 - span / 2, span });
        onchange(initial);
        cancel = graphDrag(
            event,
            { x: initial.start / 128, y: 0 },
            { width: box.width, height: 1, unboundedX: true },
            (x) => onchange(clampViewport({ start: x * 128, span: original.span })),
            (cancelled) => {
                cancel = undefined;
                if (cancelled) onchange(original);
            },
        );
    }
    function key(event: KeyboardEvent) {
        if (disabled || cancel) return;
        const next = viewportKey({ start, span }, event.key, event.shiftKey);
        if (!next) return;
        event.preventDefault();
        onchange(next);
    }
    $effect(() => {
        if (disabled) cancel?.();
    });
    $effect(() => {
        if (!rail || typeof ResizeObserver === 'undefined') return;
        const observer = new ResizeObserver(() => cancel?.());
        observer.observe(rail);
        return () => observer.disconnect();
    });
    onDestroy(() => cancel?.());
</script>

<svelte:window onblur={() => cancel?.()} />
<div
    class="overview"
    bind:this={rail}
    role="scrollbar"
    tabindex={disabled ? -1 : 0}
    aria-label="Keyboard viewport"
    aria-controls={controls}
    aria-orientation="horizontal"
    aria-valuemin={0}
    aria-valuemax={128 - span}
    aria-valuenow={start}
    aria-valuetext={`${formatNote(start)} to ${formatNote(start + span - 1)}`}
    aria-disabled={disabled}
    onpointerdown={pointer}
    onkeydown={key}
    onblur={() => cancel?.()}
>
    <svg viewBox="0 0 128 20" preserveAspectRatio="none" aria-hidden="true">
        {#each notes as note}<rect x={note} width="1" height="20" fill={colors[note]} />{/each}
    </svg>
    <div class="viewport" style:left={`${(start / 128) * 100}%`} style:width={`${(span / 128) * 100}%`}></div>
</div>

<style>
    .overview {
        position: relative;
        height: 20px;
        margin-top: 2px;
        background: var(--color-panel-deep);
        border: 1px solid var(--color-border);
        touch-action: none;
        cursor: pointer;
        overflow: hidden;
    }
    svg {
        display: block;
        width: 100%;
        height: 100%;
    }
    .viewport {
        position: absolute;
        top: 0;
        bottom: 0;
        border: 1px solid var(--color-accent);
        background: color-mix(in srgb, var(--color-accent) 12%, transparent);
        cursor: grab;
    }
    .viewport:active {
        cursor: grabbing;
    }
    .overview[aria-disabled='true'] {
        cursor: default;
    }
    .overview:focus-visible {
        outline: 2px solid var(--color-text);
        outline-offset: -2px;
    }
</style>

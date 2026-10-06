<script lang="ts">
    import { onDestroy } from 'svelte';
    import { isBlackKey, keyFace, MappingGeometryCache, mappingFill, type MappingGeometry } from './keyboardGeometry';
    import type { KeyboardZone } from './keyboardMapping';
    let {
        marked = [],
        formatNote,
        start = 0,
        end = 127,
        ranges = [],
        roots = [],
        zones = [],
        geometry,
        interactive = false,
        onpress,
        onrelease,
        onrootmenu,
    }: {
        marked?: number[];
        formatNote: (note: number) => string;
        start?: number;
        end?: number;
        ranges?: { low: number; high: number }[];
        roots?: number[];
        zones?: KeyboardZone[];
        geometry?: MappingGeometry;
        interactive?: boolean;
        onpress?: (note: number) => void;
        onrelease?: () => void;
        onrootmenu?: (note: number, event: MouseEvent | KeyboardEvent) => void;
    } = $props();
    const notes = Array.from({ length: 128 }, (_, i) => i);
    const patternId = $props.id();
    const black = isBlackKey;
    const cache = new MappingGeometryCache();
    const indexed = $derived(geometry ?? cache.read(zones));
    const selectedIds = $derived(new Set(zones.filter((zone) => zone.selected).map((zone) => zone.id)));
    const highlighted = (note: number) =>
        marked.includes(note) ||
        ranges.some((range) => note >= range.low && note <= range.high) ||
        indexed.coverage[note]!.length > 0;
    const span = $derived(end - start + 1);
    const enabled = $derived(interactive || !!onpress || !!onrootmenu);
    let focused = $state<number | null>(null);
    let held: number | null = null;
    const tabStop = $derived(Math.max(start, Math.min(end, focused ?? roots[0] ?? start)));
    function release() {
        if (held === null) return;
        held = null;
        onrelease?.();
    }
    function press(note: number) {
        if (!onpress || held === note) return;
        release();
        held = note;
        onpress(note);
    }
    function pointer(event: PointerEvent, note: number) {
        if (event.button !== 0 || !onpress) return;
        event.preventDefault();
        const target = event.currentTarget as SVGGElement;
        target.focus();
        target.setPointerCapture(event.pointerId);
        press(note);
    }
    function keydown(event: KeyboardEvent, note: number) {
        if (['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) {
            event.preventDefault();
            release();
            focused =
                event.key === 'Home'
                    ? start
                    : event.key === 'End'
                      ? end
                      : Math.max(start, Math.min(end, note + (event.key === 'ArrowLeft' ? -1 : 1)));
            (event.currentTarget as Element)
                .closest('svg')
                ?.querySelector<SVGGElement>(`[data-key="${focused}"]`)
                ?.focus();
            return;
        }
        if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            if (!event.repeat) press(note);
        } else if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) {
            onrootmenu?.(note, event);
        }
    }
    function fill(note: number) {
        const coverage = indexed.coverage[note]!;
        const zone = coverage.find((candidate) => selectedIds.has(candidate.id)) ?? coverage[0];
        return zone ? mappingFill(indexed.colors.get(zone.id), selectedIds.has(zone.id)) : undefined;
    }
    onDestroy(release);
</script>

<svelte:window onblur={release} />

{#snippet shape(note: number)}
    {@const geometry = keyFace(note)}
    {@const coverage = indexed.coverage[note]!}
    {@const color = fill(note)}
    <rect
        data-note={note}
        class={black(note) ? 'black' : 'white'}
        class:marked={highlighted(note)}
        class:selected={coverage.some((zone) => selectedIds.has(zone.id))}
        x={geometry.x}
        width={geometry.width}
        height={geometry.height}
        fill={color}
        style:fill={color}
        ><title
            >{formatNote(note)}{roots.includes(note) ? ' (root)' : ''}{coverage.length
                ? `: ${coverage.map((zone) => zone.label).join(', ')}`
                : ''}</title
        ></rect
    >
    {#if coverage.length > 1}<rect
            class="overlap"
            x={geometry.x}
            width={geometry.width}
            height={geometry.height}
            fill={`url(#${patternId})`}
        />{/if}
    {#if roots.includes(note)}<rect
            class="root-marker"
            x={geometry.x + geometry.width * 0.2}
            y={geometry.height - 6}
            width={geometry.width * 0.6}
            height="5"
        />{/if}
{/snippet}
{#snippet face(note: number)}
    {#if enabled}
        {@const visible = note >= start && note <= end}
        <g
            role="button"
            tabindex={visible && tabStop === note ? 0 : -1}
            data-key={note}
            aria-label={formatNote(note)}
            aria-hidden={!visible}
            onfocus={() => (focused = note)}
            onpointerdown={(event) => visible && pointer(event, note)}
            onpointerup={release}
            onpointercancel={release}
            onlostpointercapture={release}
            onblur={release}
            onkeydown={(event) => visible && keydown(event, note)}
            onkeyup={(event) => {
                if (event.key === 'Enter' || event.key === ' ') release();
            }}
            oncontextmenu={(event) => visible && onrootmenu?.(note, event)}>{@render shape(note)}</g
        >
    {:else}{@render shape(note)}{/if}
{/snippet}

<div
    class="keyboard"
    role={enabled ? 'group' : 'img'}
    aria-label={`Keyboard ${formatNote(start)} to ${formatNote(end)}${roots.length ? `, root ${roots.map(formatNote).join(', ')}` : ''}`}
>
    <svg class="keys" viewBox={`${start} 0 ${span} 28`} preserveAspectRatio="none">
        <defs>
            {#if indexed.coverage.some((coverage) => coverage.length > 1)}
                <pattern
                    id={patternId}
                    width="0.4"
                    height="2"
                    patternUnits="userSpaceOnUse"
                    patternTransform="rotate(15)"
                    viewBox="0 0 10 5"
                    preserveAspectRatio="none"
                >
                    <rect width="3" height="5" fill="var(--editor-loop)" opacity="0.1" stroke="none" />
                </pattern>
            {/if}
        </defs>
        {#each notes as note}
            {#if !black(note)}{@render face(note)}{/if}
        {/each}
        {#each notes.filter(black) as note}{@render face(note)}{/each}
        {#each marked.filter((note) => note >= start && note <= end) as note}<path
                class="mark"
                d={`M${keyFace(note).center} 0V28`}
            />{/each}
    </svg>
    <div class="key-labels" aria-hidden="true">
        {#each notes.filter((note) => note >= start && note <= end && note % 12 === 0) as note}<span
                data-note={note}
                style:left={`${((keyFace(note).center - start) / span) * 100}%`}>{formatNote(note)}</span
            >{/each}
    </div>
</div>

<style>
    .keyboard {
        container: keyboard-axis / inline-size;
    }
    .keys {
        display: block;
        width: 100%;
        height: 28px;
        overflow: hidden;
        shape-rendering: crispEdges;
    }
    .white {
        fill: var(--key-fill, #b5c0c2);
    }
    .black {
        fill: #253034;
    }
    .marked {
        fill: var(--editor-loop);
    }
    rect {
        stroke: var(--color-panel-deep);
        stroke-width: 1px;
        vector-effect: non-scaling-stroke;
    }
    pattern rect {
        stroke: none;
    }
    .root-marker {
        fill: var(--color-warning, #e4b15b);
        stroke-width: 1px;
        pointer-events: none;
    }
    .overlap {
        stroke: none;
        pointer-events: none;
    }
    g[role='button'] {
        cursor: pointer;
    }
    g:focus-visible {
        outline: none;
    }
    g:focus-visible rect {
        stroke: var(--color-text);
        stroke-width: 2px;
    }
    .mark {
        stroke: var(--color-accent);
        stroke-width: 1;
        vector-effect: non-scaling-stroke;
    }
    .key-labels {
        position: relative;
        height: 28px;
        color: var(--color-text-muted);
        font: 10px var(--font-mono, monospace);
    }
    .key-labels span {
        position: absolute;
        top: 2px;
        line-height: 10px;
        white-space: nowrap;
        writing-mode: vertical-rl;
        transform: translateX(-50%);
    }
</style>

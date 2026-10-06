<script lang="ts">
    import { onDestroy, type Snippet } from 'svelte';
    import KeyboardAxis from './KeyboardAxis.svelte';
    import MappingRegions from './MappingRegions.svelte';
    import MappingRootMenu from './MappingRootMenu.svelte';
    import EditorNumber from './EditorNumber.svelte';
    import { mappingRectangle, MappingGeometryCache } from './keyboardGeometry';
    import { velocityGrid } from './mappingPresentation';
    import Icon from '../../lib/components/Icon.svelte';
    import { graphDrag } from './graphDrag';
    import {
        editKeyboardRange,
        rangeBoundaries,
        type EditableMappingAxes,
        type RangeBoundary,
        type KeyboardRange,
        type KeyboardZone,
        type RangeHandle,
    } from './keyboardMapping';
    let {
        zones,
        limits,
        formatNote,
        disabled = false,
        mode = 'ranges',
        editableAxes = { keys: true, velocity: true },
        rangeLabel = 'Range',
        tools,
        onselect,
        onchange,
        onbegin = () => {},
        onend = () => {},
        rootEditable = false,
        rootIdentity = '',
        onroot = () => {},
        onpress,
        onrelease,
        targets = [],
        velocity = 100,
        onvelocity = () => {},
    }: {
        zones: KeyboardZone[];
        limits: KeyboardRange;
        formatNote: (note: number) => string;
        disabled?: boolean;
        mode?: 'ranges' | 'mapping';
        editableAxes?: EditableMappingAxes;
        rangeLabel?: string;
        tools?: Snippet;
        onselect: (id: string) => void;
        onchange: (range: KeyboardRange, boundaries: RangeBoundary[]) => void;
        onbegin?: (id?: string) => void;
        onend?: (cancelled?: boolean, move?: boolean) => void;
        rootEditable?: boolean;
        rootIdentity?: string;
        onroot?: (note: number) => void;
        onpress?: (note: number) => void;
        onrelease?: () => void;
        targets?: { id: string; limits: KeyboardRange; axes: EditableMappingAxes; editable: boolean }[];
        velocity?: number;
        onvelocity?: (velocity: number) => void;
    } = $props();
    const mapping = $derived(mode === 'mapping' ? 1 : 0);
    const axes = $derived({ keys: editableAxes.keys, velocity: !!mapping && editableAxes.velocity });
    let start = $state(0);
    let span = $state(128);
    let plot = $state<HTMLDivElement>();
    let plotHeight = $state(128);
    let plotWidth = $state(128);
    let rootMenu = $state<{ note: number; x: number; y: number; identity: string } | null>(null);
    let cancel: (() => void) | undefined;
    const x = (note: number) => ((note - start) / span) * 100;
    const selected = $derived(zones.filter((zone) => zone.selected));
    const rectangle = $derived(mappingRectangle(limits, start, span));
    const velocityOffset = $derived(Math.min(12, Math.max(0, ((rectangle.width / 100) * plotWidth) / 2 - 1)));
    const grid = velocityGrid();
    const geometryCache = new MappingGeometryCache();
    const geometry = $derived(geometryCache.read(zones));
    const handles = $derived<{ id: RangeHandle; label: string; x: number; y: number }[]>([
        ...(axes.keys
            ? ([
                  { id: 'low', label: 'Low key limit', x: rectangle.left, y: rectangle.centerY },
                  { id: 'high', label: 'High key limit', x: rectangle.left + rectangle.width, y: rectangle.centerY },
              ] as const)
            : []),
        ...(axes.velocity
            ? [
                  {
                      id: 'velocityLow' as const,
                      label: 'Low velocity limit',
                      x: x((limits.low + limits.high + 1) / 2),
                      y: 100 - (limits.velocityLow / 128) * 100,
                  },
                  {
                      id: 'velocityHigh' as const,
                      label: 'High velocity limit',
                      x: x((limits.low + limits.high + 1) / 2),
                      y: 100 - ((limits.velocityHigh + 1) / 128) * 100,
                  },
              ]
            : []),
    ]);
    $effect(() => {
        if (!plot || typeof ResizeObserver === 'undefined') return;
        const observer = new ResizeObserver(([entry]) => {
            plotHeight = entry.contentRect.height;
            plotWidth = entry.contentRect.width;
        });
        observer.observe(plot);
        return () => observer.disconnect();
    });
    $effect(() => {
        if (disabled || !rootEditable || !mapping || (rootMenu && rootMenu.identity !== rootIdentity)) rootMenu = null;
    });
    function openRoot(note: number, event: MouseEvent | KeyboardEvent) {
        if (disabled || !rootEditable || !mapping) return;
        event.preventDefault();
        event.stopPropagation();
        (event.currentTarget as HTMLElement | SVGElement).focus();
        const box = (event.currentTarget as Element).getBoundingClientRect();
        rootMenu = {
            note,
            x: event instanceof MouseEvent ? event.clientX : box.left + box.width / 2,
            y: event instanceof MouseEvent ? event.clientY : box.bottom,
            identity: rootIdentity,
        };
    }
    function plotRoot(event: MouseEvent) {
        if (!plot) return;
        const box = plot.getBoundingClientRect();
        openRoot(
            Math.max(0, Math.min(127, Math.floor(start + ((event.clientX - box.left) / box.width) * span))),
            event,
        );
    }
    function zoom(factor: number) {
        const next = Math.max(12, Math.min(128, Math.round(span * factor)));
        start = Math.max(0, Math.min(128 - next, Math.round(start + (span - next) / 2)));
        span = next;
    }
    function fit() {
        const shown = selected.length ? selected : zones;
        const low = Math.min(limits.low, ...shown.map((zone) => zone.low));
        const high = Math.max(limits.high, ...shown.map((zone) => zone.high));
        span = Math.max(12, Math.min(128, high - low + 13));
        start = Math.max(0, Math.min(128 - span, low - 6));
    }
    function drag(event: PointerEvent, handle: RangeHandle) {
        if (disabled || !plot || !rangeBoundaries(handle, axes).length) return;
        event.preventDefault();
        cancel?.();
        const original = { ...limits },
            box = plot.getBoundingClientRect();
        const changed = new Set<RangeBoundary>();
        let previous = original;
        onbegin();
        cancel = graphDrag(
            event,
            { x: 0.5, y: 0.5 },
            { width: box.width, height: box.height, unboundedX: true, unboundedY: true },
            (nx, ny) => {
                const dx = axes.keys ? (nx - 0.5) * span : 0;
                const dy = axes.velocity ? (ny - 0.5) * 128 : 0;
                const range = editKeyboardRange(original, handle, dx, dy);
                if (Object.keys(range).every((key) => range[key as RangeBoundary] === previous[key as RangeBoundary]))
                    return;
                previous = range;
                for (const boundary of rangeBoundaries(handle, axes))
                    if (range[boundary] !== original[boundary]) changed.add(boundary);
                onchange(range, [...changed]);
            },
            (cancelled) => {
                onend(cancelled, handle === 'move');
                cancel = undefined;
            },
        );
    }
    function dragBlock(event: PointerEvent, ids: string[]) {
        if (disabled || !plot || event.button !== 0) return;
        const id = ids.find((id) => selected.some((zone) => zone.id === id)) ?? ids[0]!;
        const target =
            targets.find((row) => row.id === id) ??
            (selected.some((zone) => zone.id === id) ? { limits, axes, editable: true } : null);
        if (!target?.editable) return;
        event.preventDefault();
        cancel?.();
        const original = { ...target.limits },
            box = plot.getBoundingClientRect(),
            element = event.currentTarget as HTMLElement;
        let begun = false,
            previous = original;
        const changed = new Set<RangeBoundary>();
        cancel = graphDrag(
            event,
            { x: 0.5, y: 0.5 },
            { width: box.width, height: box.height, unboundedX: true, unboundedY: true },
            (nx, ny) => {
                if (!begun && Math.hypot((nx - 0.5) * box.width, (ny - 0.5) * box.height) < 3) return;
                if (!begun) {
                    begun = true;
                    onbegin(id);
                }
                const range = editKeyboardRange(
                    original,
                    'move',
                    target.axes.keys ? (nx - 0.5) * span : 0,
                    target.axes.velocity ? (ny - 0.5) * 128 : 0,
                );
                if (Object.keys(range).every((key) => range[key as RangeBoundary] === previous[key as RangeBoundary]))
                    return;
                previous = range;
                for (const boundary of rangeBoundaries('move', target.axes))
                    if (range[boundary] !== original[boundary]) changed.add(boundary);
                onchange(range, [...changed]);
            },
            (cancelled) => {
                cancel = undefined;
                if (begun) {
                    if (!cancelled)
                        element.addEventListener(
                            'click',
                            (click) => {
                                click.preventDefault();
                                click.stopImmediatePropagation();
                            },
                            { capture: true, once: true },
                        );
                    onend(cancelled, true);
                }
            },
        );
    }
    function key(event: KeyboardEvent, handle: RangeHandle) {
        if (disabled || !['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return;
        event.preventDefault();
        onbegin();
        const step = event.shiftKey ? 12 : 1;
        const delta =
            event.key === 'Home'
                ? -127
                : event.key === 'End'
                  ? 127
                  : event.key === 'ArrowLeft' || event.key === 'ArrowDown'
                    ? -step
                    : step;
        const vertical =
            handle.startsWith('velocity') || (handle === 'move' && ['ArrowUp', 'ArrowDown'].includes(event.key));
        const permitted = vertical ? axes.velocity : axes.keys;
        if (permitted)
            onchange(
                editKeyboardRange(limits, handle, vertical ? 0 : delta, vertical ? delta : 0),
                rangeBoundaries(handle, { keys: !vertical && axes.keys, velocity: vertical && axes.velocity }),
            );
        onend();
    }
    onDestroy(() => cancel?.());
</script>

{#snippet keyboard()}
    <KeyboardAxis
        {start}
        end={start + span - 1}
        {formatNote}
        {zones}
        {geometry}
        interactive
        {onpress}
        {onrelease}
        onrootmenu={rootEditable && mapping ? openRoot : undefined}
        roots={selected.flatMap((zone) => (zone.root === undefined ? [] : [zone.root]))}
    />
{/snippet}
{#snippet boundaries()}
    {#each handles as handle (handle.id)}
        <button
            class="limit-handle"
            class:velocity={handle.id.startsWith('velocity')}
            {disabled}
            aria-label={handle.label}
            title={handle.label}
            style:left={`${handle.x}%`}
            style:margin-left={mapping && (rectangle.height / 100) * plotHeight < 18 && handle.id.startsWith('velocity')
                ? `${handle.id === 'velocityLow' ? -velocityOffset : velocityOffset}px`
                : '0px'}
            style:top={mapping ? `${handle.y}%` : '14px'}
            hidden={handle.x < 0 || handle.x > 100}
            onpointerdown={(event) => drag(event, handle.id)}
            onkeydown={(event) => key(event, handle.id)}
        ></button>
    {/each}
{/snippet}
<section class="keyboard-mapping" class:full={mapping === 1} aria-label="Key and velocity mapping">
    <div class="toolbar">
        {#if tools}{@render tools()}{/if}
        <span class="summary"
            >{formatNote(limits.low)}–{formatNote(limits.high)}{#if mapping}
                · Velocity {limits.velocityLow}–{limits.velocityHigh}{/if}</span
        >
        <div class="view-tools">
            {#if onpress}<div class="audition-velocity">
                    <span>Velocity</span><EditorNumber
                        label="Audition velocity"
                        value={velocity}
                        min={1}
                        max={127}
                        onchange={onvelocity}
                    />
                </div>{/if}
            <div class="viewport-tools">
                <button
                    class="editor-icon pan-left"
                    aria-label="Pan keyboard left"
                    title="Pan left"
                    disabled={start === 0}
                    onclick={() => (start = Math.max(0, start - 12))}><Icon name="chevron" size={14} /></button
                >
                <button
                    class="editor-icon pan-right"
                    aria-label="Pan keyboard right"
                    title="Pan right"
                    disabled={start + span >= 128}
                    onclick={() => (start = Math.min(128 - span, start + 12))}><Icon name="chevron" size={14} /></button
                >
                <button
                    class="editor-icon"
                    aria-label="Zoom keyboard out"
                    title="Zoom out"
                    disabled={span === 128}
                    onclick={() => zoom(2)}><Icon name="zoom-out" size={14} /></button
                >
                <button
                    class="editor-icon"
                    aria-label="Zoom keyboard in"
                    title="Zoom in"
                    disabled={span === 12}
                    onclick={() => zoom(0.5)}><Icon name="zoom-in" size={14} /></button
                >
                <button class="editor-icon" aria-label="Fit keyboard ranges" title="Fit ranges" onclick={fit}
                    ><Icon name="fit-width" size={14} /></button
                >
            </div>
        </div>
    </div>
    {#if mapping}
        <div class="mapping-layout">
            <div class="velocity-axis" aria-hidden="true">
                {#each [...grid.major, ...grid.minor].sort((a, b) => a.value - b.value) as tick}<span
                        data-velocity={tick.value}
                        style:top={`${tick.position}%`}>{tick.value}</span
                    >{/each}
            </div>
            <div
                class="plot"
                class:extended={mapping === 1}
                bind:this={plot}
                role="group"
                aria-label="Mapping ranges"
                oncontextmenu={plotRoot}
            >
                <div class="plot-content">
                    <MappingRegions
                        {zones}
                        {geometry}
                        {start}
                        {span}
                        width={plotWidth}
                        height={plotHeight}
                        {grid}
                        {formatNote}
                        {onselect}
                        ondrag={dragBlock}
                    />
                    <button
                        class="limits"
                        disabled={disabled || (!axes.keys && !axes.velocity)}
                        aria-label={`Move ${rangeLabel}`}
                        title={`${rangeLabel}; drag to move`}
                        onpointerdown={(event) => drag(event, 'move')}
                        onkeydown={(event) => key(event, 'move')}
                        style:left={`${rectangle.left}%`}
                        style:width={`${rectangle.width}%`}
                        style:top={`${rectangle.top}%`}
                        style:height={`${rectangle.height}%`}
                    ></button>
                </div>
                {@render boundaries()}
            </div>
            <div></div>
            {@render keyboard()}
        </div>
        <div class="legend">
            <span><i class="source-key"></i>Source</span>
            <span><i class="effective-key"></i>Effective</span>
            <span><i class="limit-key"></i>{rangeLabel}</span>
            <span><i class="root-key"></i>Root</span>
        </div>
    {:else}
        <div class="keyboard-surface" bind:this={plot}>
            {@render keyboard()}
            <div class="compact-outline">
                <div class="compact-limits" style:left={`${rectangle.left}%`} style:width={`${rectangle.width}%`}></div>
            </div>
            {@render boundaries()}
        </div>
    {/if}
</section>
{#if rootMenu}<MappingRootMenu {...rootMenu} {formatNote} onchange={onroot} onclose={() => (rootMenu = null)} />{/if}

<style>
    .keyboard-mapping {
        min-width: 0;
        margin-top: 8px;
    }
    .toolbar {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 4px;
        min-height: 28px;
        margin-bottom: 4px;
    }
    .full {
        display: flex;
        flex-direction: column;
        height: 100%;
        min-height: 410px;
    }
    .full .mapping-layout {
        flex: 1;
        min-height: 376px;
        grid-template-rows: minmax(320px, 1fr) auto;
    }
    .pan-left :global(svg) {
        transform: rotate(90deg);
    }
    .view-tools {
        display: flex;
        flex-wrap: wrap;
        justify-content: flex-end;
        gap: 4px;
        margin-left: auto;
        max-width: 100%;
        flex: none;
    }
    .viewport-tools {
        display: flex;
        gap: 4px;
        flex: none;
    }
    .audition-velocity {
        display: flex;
        align-items: center;
        gap: 6px;
        width: 190px;
        max-width: 100%;
        font-size: 10px;
    }
    .pan-right :global(svg) {
        transform: rotate(-90deg);
    }
    .summary {
        flex: 1;
        min-width: 72px;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .mapping-layout {
        display: grid;
        grid-template-columns: 26px minmax(0, 1fr);
    }
    .keyboard-surface {
        position: relative;
        min-width: 0;
    }
    .plot {
        position: relative;
        height: 52px;
        background: var(--color-panel-deep);
        border: 1px solid var(--color-border);
    }
    .plot-content {
        position: absolute;
        inset: 0;
        overflow: hidden;
    }
    .compact-limits {
        position: absolute;
        top: 0;
        height: 28px;
        box-shadow: inset 0 0 0 2px var(--color-accent);
        pointer-events: none;
    }
    .compact-outline {
        position: absolute;
        inset: 0;
        overflow: hidden;
        pointer-events: none;
    }
    .plot.extended {
        height: 100%;
    }
    .limits {
        position: absolute;
        padding: 0;
        border: 0;
        box-shadow: inset 0 0 0 2px var(--color-accent);
        background: transparent;
        border-radius: 0;
        pointer-events: none;
    }
    .limit-handle {
        position: absolute;
        width: 8px;
        height: 18px;
        padding: 0;
        background: var(--color-accent);
        border: 1px solid var(--color-panel-deep);
        border-radius: 2px;
        transform: translate(-50%, -50%);
        cursor: ew-resize;
        touch-action: none;
    }
    .limit-handle.velocity {
        width: 18px;
        height: 8px;
        cursor: ns-resize;
    }
    button:focus-visible {
        outline: 2px solid var(--color-text);
        outline-offset: -2px;
    }
    .velocity-axis {
        position: relative;
        color: var(--color-text-muted);
        font-size: 10px;
        text-align: right;
    }
    .velocity-axis span {
        position: absolute;
        right: 5px;
        transform: translateY(-50%);
        line-height: 10px;
    }
    .legend {
        display: flex;
        flex-wrap: wrap;
        align-items: center;
        gap: 6px;
        padding-left: 26px;
        color: var(--color-text-muted);
        font-size: 10px;
        min-height: 20px;
    }
    .legend span {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        white-space: nowrap;
    }
    .legend i {
        width: 12px;
        height: 8px;
    }
    .source-key {
        border: 1px dashed var(--color-text-muted);
    }
    .effective-key {
        background: var(--editor-loop);
    }
    .limit-key {
        border: 2px solid var(--color-accent);
    }
    .root-key {
        background: var(--color-warning, #e4b15b);
    }
</style>

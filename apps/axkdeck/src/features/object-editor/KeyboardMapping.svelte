<script lang="ts">
    import { onDestroy, type Snippet } from 'svelte';
    import KeyboardAxis from './KeyboardAxis.svelte';
    import MappingRegions from './MappingRegions.svelte';
    import MappingRootMenu from './MappingRootMenu.svelte';
    import MappingOverview from './MappingOverview.svelte';
    import MappingViewTools from './MappingViewTools.svelte';
    import MappingDragReadout from './MappingDragReadout.svelte';
    import { mappingDragText, type MappingFeedback } from './mappingFeedback';
    import {
        minimumKeyboardSpan,
        resizeViewport,
        zoomViewport,
        fitViewport,
        type MappingViewport,
    } from './mappingViewport';
    import { mappingRectangle, MappingGeometryCache } from './keyboardGeometry';
    import { MappingPreviewGeometryCache } from './mappingPreviewGeometry';
    import { velocityGrid } from './mappingPresentation';
    import { graphDrag } from './graphDrag';
    import {
        editKeyboardRange,
        rangeBoundaries,
        type EditableMappingAxes,
        type RangeBoundary,
        type KeyboardRange,
        type KeyboardZone,
        type RangeHandle,
        type KeyboardMappingPreview,
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
        preview,
        coverageLabel = 'Effective',
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
        preview?: KeyboardMappingPreview;
        coverageLabel?: string;
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
    let feedback = $state<MappingFeedback | null>(null);
    const plotId = $props.id();
    const minimum = $derived(minimumKeyboardSpan(plotWidth, !mapping));
    const selectionIdentity = $derived(
        [...new Set(zones.filter((zone) => zone.selected).map((zone) => zone.selectionId ?? zone.id))]
            .sort()
            .join('\n'),
    );
    const selectedIds = $derived(new Set(zones.filter((zone) => zone.selected).map((zone) => zone.id)));
    const busy = $derived(feedback !== null);
    const x = (note: number) => ((note - start) / span) * 100;
    const previewById = $derived(new Map(preview?.zones.map((zone) => [zone.id, zone])));
    const shownZones = $derived(preview ? zones.map((zone) => previewById.get(zone.id) ?? zone) : zones);
    const selected = $derived(shownZones.filter((zone) => zone.selected));
    const rectangle = $derived(mappingRectangle(limits, start, span));
    const velocityOffset = $derived(Math.min(12, Math.max(0, ((rectangle.width / 100) * plotWidth) / 2 - 1)));
    const grid = velocityGrid();
    const geometryCache = new MappingGeometryCache();
    const previewCache = new MappingPreviewGeometryCache();
    const staticGeometry = $derived(geometryCache.read(zones));
    const presentation = $derived(previewCache.read(staticGeometry, preview));
    const geometry = $derived(presentation.geometry);
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
            if (plotHeight !== entry.contentRect.height || plotWidth !== entry.contentRect.width) {
                cancel?.();
                feedback = null;
            }
            plotHeight = entry.contentRect.height;
            plotWidth = entry.contentRect.width;
            view({ start, span });
        });
        observer.observe(plot);
        return () => observer.disconnect();
    });
    $effect(() => {
        if (disabled || !rootEditable || !mapping || (rootMenu && rootMenu.identity !== rootIdentity)) rootMenu = null;
    });
    $effect(() => {
        if (feedback && (disabled || feedback.identity !== selectionIdentity)) {
            cancel?.();
            feedback = null;
        }
    });
    function clearFeedback() {
        if (!feedback?.pointer) feedback = null;
    }
    function blurFeedback() {
        cancel?.();
        feedback = null;
    }
    function view(next: MappingViewport) {
        ({ start, span } = resizeViewport(next, minimum));
    }
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
        if (!busy) view(zoomViewport({ start, span }, factor, minimum));
    }
    function fit() {
        if (busy) return;
        const shown = selected.length ? selected : zones;
        const low = Math.min(limits.low, ...shown.map((zone) => zone.low));
        const high = Math.max(limits.high, ...shown.map((zone) => zone.high));
        view(fitViewport(low, high, minimum));
    }
    function drag(event: PointerEvent, handle: RangeHandle) {
        if (disabled || !plot || event.button !== 0 || !rangeBoundaries(handle, axes).length) return;
        event.preventDefault();
        cancel?.();
        const original = { ...limits },
            box = plot.getBoundingClientRect();
        const changed = new Set<RangeBoundary>();
        let previous = original;
        onbegin();
        feedback = { range: original, handle, axes, identity: selectionIdentity, pointer: true };
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
                if (feedback) feedback.range = range;
                for (const boundary of rangeBoundaries(handle, axes))
                    if (range[boundary] !== original[boundary]) changed.add(boundary);
                onchange(range, [...changed]);
            },
            (cancelled) => {
                onend(cancelled, handle === 'move');
                cancel = undefined;
                feedback = null;
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
        const identity = selected.some((zone) => zone.id === id)
            ? selectionIdentity
            : String(zones.find((zone) => zone.id === id)?.selectionId ?? id);
        cancel = graphDrag(
            event,
            { x: 0.5, y: 0.5 },
            { width: box.width, height: box.height, unboundedX: true, unboundedY: true },
            (nx, ny) => {
                if (!begun && Math.hypot((nx - 0.5) * box.width, (ny - 0.5) * box.height) < 3) return;
                if (!begun) {
                    begun = true;
                    onbegin(id);
                    feedback = {
                        range: original,
                        handle: 'move',
                        axes: target.axes,
                        identity,
                        pointer: true,
                    };
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
                if (feedback) feedback.range = range;
                for (const boundary of rangeBoundaries('move', target.axes))
                    if (range[boundary] !== original[boundary]) changed.add(boundary);
                onchange(range, [...changed]);
            },
            (cancelled) => {
                cancel = undefined;
                feedback = null;
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
        if (permitted) {
            const range = editKeyboardRange(limits, handle, vertical ? 0 : delta, vertical ? delta : 0);
            feedback = { range, handle, axes, identity: selectionIdentity, pointer: false };
            onchange(
                range,
                rangeBoundaries(handle, { keys: !vertical && axes.keys, velocity: vertical && axes.velocity }),
            );
        }
        onend();
    }
    onDestroy(() => cancel?.());
</script>

<svelte:window
    onblur={() => {
        cancel?.();
        feedback = null;
    }}
/>

{#snippet keyboard()}
    <KeyboardAxis
        {start}
        end={start + span - 1}
        {formatNote}
        zones={shownZones}
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
            style:left={`${handle.x}%`}
            style:margin-left={mapping && (rectangle.height / 100) * plotHeight < 18 && handle.id.startsWith('velocity')
                ? `${handle.id === 'velocityLow' ? -velocityOffset : velocityOffset}px`
                : '0px'}
            style:top={mapping ? `${handle.y}%` : '14px'}
            hidden={handle.x < 0 || handle.x > 100}
            onpointerdown={(event) => drag(event, handle.id)}
            onkeydown={(event) => key(event, handle.id)}
            onkeyup={clearFeedback}
            onblur={blurFeedback}
        ></button>
    {/each}
{/snippet}
<section
    class="keyboard-mapping"
    class:full={mapping === 1}
    class:zoomed={span < 128}
    aria-label="Key and velocity mapping"
>
    <div class="toolbar">
        {#if tools}{@render tools()}{/if}
        <span class="summary" data-drag-readout={!mapping && feedback ? '' : undefined}
            >{#if !mapping && feedback}{mappingDragText(feedback, formatNote)}{:else}{formatNote(
                    limits.low,
                )}–{formatNote(limits.high)}{#if mapping}
                    · Velocity {limits.velocityLow}–{limits.velocityHigh}{/if}{/if}</span
        >
        <MappingViewTools
            {start}
            {span}
            {minimum}
            {busy}
            audition={!!onpress}
            {velocity}
            {onvelocity}
            onpan={(delta) => !busy && view({ start: start + delta, span })}
            onzoom={zoom}
            onfit={fit}
        />
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
                id={plotId}
                class:extended={mapping === 1}
                bind:this={plot}
                role="group"
                aria-label="Mapping ranges"
                oncontextmenu={plotRoot}
            >
                <div class="plot-content">
                    <MappingRegions
                        zones={shownZones}
                        {geometry}
                        overlapPath={presentation.overlapPath}
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
                        onkeyup={clearFeedback}
                        onblur={blurFeedback}
                        style:left={`${rectangle.left}%`}
                        style:width={`${rectangle.width}%`}
                        style:top={`${rectangle.top}%`}
                        style:height={`${rectangle.height}%`}
                    ></button>
                </div>
                {@render boundaries()}
                {#if feedback}<MappingDragReadout
                        {feedback}
                        {formatNote}
                        {start}
                        {span}
                        width={plotWidth}
                        height={plotHeight}
                    />{/if}
            </div>
            <div></div>
            {@render keyboard()}
            {#if span < 128}<div></div>
                <MappingOverview
                    {start}
                    {span}
                    {geometry}
                    selected={selectedIds}
                    disabled={busy}
                    {formatNote}
                    controls={plotId}
                    onchange={view}
                />{/if}
        </div>
        <div class="legend">
            {#if geometry.sources.length}<span><i class="source-key"></i>Source</span>{/if}
            <span><i class="effective-key"></i>{coverageLabel}</span>
            <span><i class="limit-key"></i>{rangeLabel}</span>
            <span><i class="root-key"></i>Root</span>
        </div>
    {:else}
        <div class="keyboard-surface" id={plotId} bind:this={plot}>
            {@render keyboard()}
            <div class="compact-outline">
                <div class="compact-limits" style:left={`${rectangle.left}%`} style:width={`${rectangle.width}%`}></div>
            </div>
            {@render boundaries()}
        </div>
        {#if span < 128}<MappingOverview
                {start}
                {span}
                {geometry}
                selected={selectedIds}
                disabled={busy}
                {formatNote}
                controls={plotId}
                onchange={view}
            />{/if}
    {/if}
</section>
{#if rootMenu}<MappingRootMenu {...rootMenu} {formatNote} onchange={onroot} onclose={() => (rootMenu = null)} />{/if}

<style>
    .keyboard-mapping {
        min-width: 0;
        margin-top: 8px;
    }
    .keyboard-mapping:not(.full) {
        max-width: 1280px;
    }
    .toolbar {
        display: flex;
        flex: none;
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
        grid-template-rows: minmax(320px, 1fr) auto auto;
    }
    .full.zoomed .mapping-layout {
        min-height: 398px;
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
        flex: none;
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

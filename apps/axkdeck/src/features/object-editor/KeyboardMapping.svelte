<script lang="ts">
    import { onDestroy, type Snippet } from 'svelte';
    import KeyboardAxis from './KeyboardAxis.svelte';
    import Icon from '../../lib/components/Icon.svelte';
    import { graphDrag } from './graphDrag';
    import { editKeyboardRange, type KeyboardRange, type KeyboardZone, type RangeHandle } from './keyboardMapping';
    let {
        zones,
        limits,
        formatNote,
        disabled = false,
        mode = 'ranges',
        tools,
        onselect,
        onchange,
        onbegin = () => {},
        onend = () => {},
    }: {
        zones: KeyboardZone[];
        limits: KeyboardRange;
        formatNote: (note: number) => string;
        disabled?: boolean;
        mode?: 'ranges' | 'mapping';
        tools?: Snippet;
        onselect: (id: string) => void;
        onchange: (range: KeyboardRange) => void;
        onbegin?: () => void;
        onend?: () => void;
    } = $props();
    const mapping = $derived(mode === 'mapping' ? 1 : 0);
    let start = $state(0);
    let span = $state(128);
    let plot: HTMLDivElement;
    let cancel: (() => void) | undefined;
    const x = (note: number) => ((note - start) / span) * 100;
    const selected = $derived(zones.filter((zone) => zone.selected));
    const handles = $derived<{ id: RangeHandle; label: string; x: number; y: number }[]>([
        { id: 'low', label: 'Low key limit', x: x(limits.low), y: 50 },
        { id: 'high', label: 'High key limit', x: x(limits.high + 1), y: 50 },
        ...(mapping
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
        if (disabled) return;
        event.preventDefault();
        cancel?.();
        const original = { ...limits },
            box = plot.getBoundingClientRect();
        onbegin();
        cancel = graphDrag(
            event,
            { x: 0.5, y: 0.5 },
            { width: box.width, height: box.height, unboundedX: true, unboundedY: true },
            (nx, ny) => {
                onchange(editKeyboardRange(original, handle, (nx - 0.5) * span, mapping ? (ny - 0.5) * 128 : 0));
            },
            () => {
                onend();
                cancel = undefined;
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
        onchange(editKeyboardRange(limits, handle, vertical ? 0 : delta, vertical && mapping ? delta : 0));
        onend();
    }
    onDestroy(() => cancel?.());
</script>

<section class="keyboard-mapping" class:full={mapping === 1} aria-label="Key and velocity mapping">
    <div class="toolbar">
        {#if tools}{@render tools()}{/if}
        <span class="summary"
            >{formatNote(limits.low)}–{formatNote(limits.high)} · Velocity {limits.velocityLow}–{limits.velocityHigh}</span
        >
        <div class="view-tools">
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
    <div class="mapping-layout" class:extended={mapping === 1}>
        <div class="velocity-axis" aria-hidden="true">
            {#if mapping}<span>127</span><span>64</span><span>0</span>{/if}
        </div>
        <div class="plot" class:extended={mapping === 1} bind:this={plot}>
            <svg viewBox={`${start} 0 ${span} 128`} preserveAspectRatio="none" aria-hidden="true">
                {#each Array.from({ length: 129 }, (_, i) => i) as note}<path
                        class="grid"
                        class:octave={note % 12 === 0}
                        d={`M${note} 0V128`}
                    />{/each}
                {#if mapping}{#each [0, 32, 64, 96, 128] as velocity}<path
                            class="grid"
                            d={`M0 ${velocity}H128`}
                        />{/each}{/if}
                {#each zones as zone}
                    {#if zone.source && zone.source.low <= zone.source.high}<rect
                            class="source"
                            x={zone.source.low}
                            width={zone.source.high - zone.source.low + 1}
                            y={mapping ? 127 - zone.source.velocityHigh : 12}
                            height={mapping ? zone.source.velocityHigh - zone.source.velocityLow + 1 : 104}
                        />{/if}
                {/each}
            </svg>
            {#each zones as zone (zone.id)}
                {#if !zone.empty}
                    <button
                        class="zone"
                        class:chosen={zone.selected}
                        title={`${zone.label}: ${formatNote(zone.low)}–${formatNote(zone.high)}, velocity ${zone.velocityLow}–${zone.velocityHigh}`}
                        aria-label={`Select mapping ${zone.label}`}
                        aria-pressed={!!zone.selected}
                        style:left={`${x(zone.low)}%`}
                        style:width={`${((zone.high - zone.low + 1) / span) * 100}%`}
                        style:top={`${mapping ? ((127 - zone.velocityHigh) / 128) * 100 : 22}%`}
                        style:height={`${mapping ? ((zone.velocityHigh - zone.velocityLow + 1) / 128) * 100 : 56}%`}
                        onclick={() => onselect(zone.id)}
                        >{#if mapping}<span>{zone.label}</span>{/if}</button
                    >
                {/if}
            {/each}
            <button
                class="limits"
                {disabled}
                aria-label="Move Program limits"
                title="Program limits; drag to move"
                onpointerdown={(event) => drag(event, 'move')}
                onkeydown={(event) => key(event, 'move')}
                style:left={`${x(limits.low)}%`}
                style:width={`${((limits.high - limits.low + 1) / span) * 100}%`}
                style:top={`${mapping ? ((127 - limits.velocityHigh) / 128) * 100 : 8}%`}
                style:height={`${mapping ? ((limits.velocityHigh - limits.velocityLow + 1) / 128) * 100 : 84}%`}
            ></button>
            {#each handles as handle (handle.id)}
                <button
                    class="limit-handle"
                    class:velocity={handle.id.startsWith('velocity')}
                    {disabled}
                    aria-label={handle.label}
                    title={handle.label}
                    style:left={`${Math.max(0.5, Math.min(99.5, handle.x))}%`}
                    style:top={`${Math.max(3, Math.min(97, handle.y))}%`}
                    hidden={handle.x < 0 || handle.x > 100}
                    onpointerdown={(event) => drag(event, handle.id)}
                    onkeydown={(event) => key(event, handle.id)}
                ></button>
            {/each}
        </div>
        <div></div>
        <KeyboardAxis
            {start}
            end={start + span - 1}
            {formatNote}
            ranges={selected.filter((zone) => !zone.empty)}
            roots={selected.flatMap((zone) => (zone.root === undefined ? [] : [zone.root]))}
        />
    </div>
    <div class="legend">
        <span><i class="source-key"></i>Source</span>
        <span><i class="effective-key"></i>Effective</span>
        <span><i class="limit-key"></i>Program limits</span>
        <span><i class="root-key"></i>Root</span>
    </div>
</section>

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
    }
    .full {
        display: flex;
        flex-direction: column;
        height: 100%;
        min-height: 260px;
    }
    .full .mapping-layout {
        flex: 1;
        min-height: 0;
        grid-template-rows: minmax(0, 1fr) auto;
    }
    .pan-left :global(svg) {
        transform: rotate(90deg);
    }
    .view-tools {
        display: flex;
        gap: 4px;
        margin-left: auto;
        flex: none;
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
    .plot {
        position: relative;
        height: 52px;
        overflow: hidden;
        background: var(--color-panel-deep);
        border: 1px solid var(--color-border);
    }
    .plot.extended {
        height: 100%;
    }
    svg {
        position: absolute;
        width: 100%;
        height: 100%;
    }
    .grid {
        stroke: var(--color-border);
        stroke-width: 0.5;
        vector-effect: non-scaling-stroke;
        fill: none;
        opacity: 0.35;
    }
    .grid.octave {
        opacity: 1;
    }
    .source {
        fill: none;
        stroke: var(--color-text-muted);
        stroke-dasharray: 3 3;
        vector-effect: non-scaling-stroke;
    }
    .zone {
        position: absolute;
        padding: 0;
        border: 1px solid var(--color-accent);
        border-radius: 0;
        background: color-mix(in srgb, var(--color-accent) 18%, transparent);
        overflow: hidden;
        color: var(--color-text);
        font: 10px var(--font-ui, sans-serif);
        text-align: left;
    }
    .zone.chosen {
        background: color-mix(in srgb, var(--editor-loop) 30%, transparent);
        border-color: var(--editor-loop);
    }
    .zone span {
        position: absolute;
        top: 3px;
        left: 4px;
        white-space: nowrap;
    }
    .limits {
        position: absolute;
        padding: 0;
        border: 2px solid var(--color-accent);
        background: transparent;
        border-radius: 0;
        pointer-events: none;
    }
    .limits::after {
        content: '';
        position: absolute;
        top: 0;
        left: 0;
        right: 0;
        height: 7px;
        pointer-events: auto;
        cursor: move;
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
        display: flex;
        flex-direction: column;
        justify-content: space-between;
        padding-right: 5px;
        color: var(--color-text-muted);
        font-size: 10px;
        text-align: right;
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

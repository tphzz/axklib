<script lang="ts" module>
    export interface PlotPoint {
        x: number;
        y: number;
    }
    export interface PlotHandle extends PlotPoint {
        id: string;
        label: string;
        readout: string;
        disabled?: boolean;
        horizontal?: boolean;
        vertical?: boolean;
        unboundedHorizontal?: boolean;
        help?: string;
        trace?: string;
        alternate?: boolean;
    }
    export interface PlotTrace {
        id: string;
        points: PlotPoint[];
        color?: string;
        dashed?: boolean;
    }
</script>

<script lang="ts">
    import { onDestroy, type Snippet } from 'svelte';
    import { on } from 'svelte/events';
    import GraphFrame from './GraphFrame.svelte';
    import GraphReadout from './GraphReadout.svelte';
    import { graphDrag } from './graphDrag';
    import { hoverHelp } from '../../lib/components/hoverHelp.svelte';
    import { tracePosition } from './tracePosition';
    import Icon from '../../lib/components/Icon.svelte';
    let {
        label,
        traces,
        handles = [],
        ticks,
        axis,
        disabled = false,
        onchange,
        onkey,
        onbegin = () => {},
        onend = () => {},
        onselect = () => {},
        tools,
        title,
        onaltdrag,
        onwheel,
    }: {
        label: string;
        traces: PlotTrace[];
        handles?: PlotHandle[];
        ticks: (string | number)[];
        axis: { x: number; label: string }[];
        disabled?: boolean;
        onchange?: (id: string, x: number, y: number) => void;
        onkey?: (id: string, key: string, shift: boolean, alt: boolean) => void;
        onbegin?: (id: string) => void;
        onend?: () => void;
        onselect?: (id: string) => void;
        tools?: Snippet;
        title?: Snippet;
        onaltdrag?: (id: string, delta: number) => void;
        onwheel?: (id: string, event: WheelEvent) => void;
    } = $props();
    let host: HTMLDivElement;
    let selected = $state('');
    let focused = $state('');
    let readout = $state('');
    let dragging = $state(false);
    let cancel: (() => void) | undefined;
    let traceX = $state(0.15);
    let nearTrace = $state('');
    const controls = $derived(
        handles.map((handle) => {
            const point = handle.trace
                ? tracePosition(traces.find((trace) => trace.id === handle.trace)?.points ?? [], traceX)
                : undefined;
            return point ? { ...handle, ...point } : handle;
        }),
    );
    function hoverTrace(event: PointerEvent) {
        if (dragging || disabled) return;
        const bounds = host.getBoundingClientRect();
        const x = (event.clientX - bounds.left) / bounds.width,
            y = 1 - (event.clientY - bounds.top) / bounds.height;
        if (
            controls.some(
                (handle) =>
                    !handle.trace && Math.hypot((handle.x - x) * bounds.width, (handle.y - y) * bounds.height) < 22,
            )
        ) {
            nearTrace = '';
            return;
        }
        const handle = handles.find((item) => item.trace && !item.disabled);
        const point = handle
            ? tracePosition(traces.find((trace) => trace.id === handle.trace)?.points ?? [], x)
            : undefined;
        nearTrace = point && Math.abs((point.y - y) * bounds.height) < 14 ? handle!.id : '';
        if (nearTrace) traceX = x;
    }
    const path = (points: PlotPoint[]) =>
        points.map((p, i) => `${i ? 'L' : 'M'}${p.x * 1000},${(1 - p.y) * 200}`).join(' ');
    const chosen = $derived(controls.find((item) => item.id === readout));
    function wheelHandle(node: HTMLButtonElement, id: string) {
        return {
            destroy: on(
                node,
                'wheel',
                (event) => {
                    if (!disabled && !node.disabled) onwheel?.(id, event);
                },
                { passive: false },
            ),
        };
    }
    function drag(event: PointerEvent, handle: PlotHandle) {
        if (disabled || handle.disabled || (event.altKey && handle.alternate === false)) return;
        event.preventDefault();
        cancel?.();
        selected = handle.id;
        const target = event.currentTarget as HTMLButtonElement;
        target.focus();
        onbegin(handle.id);
        dragging = true;
        readout = handle.id;
        const alternate = event.altKey && handle.alternate !== false && onaltdrag;
        const bounds = host.getBoundingClientRect();
        cancel = graphDrag(
            event,
            alternate ? { x: 0.5, y: 0.5 } : handle,
            { width: bounds.width, height: bounds.height, unboundedX: handle.unboundedHorizontal },
            (x, y) => {
                if (alternate) alternate(handle.id, x + y - 1);
                else onchange?.(handle.id, handle.horizontal ? x : handle.x, handle.vertical ? y : handle.y);
            },
            () => {
                dragging = false;
                readout = '';
                onend();
                cancel = undefined;
            },
        );
    }
    onDestroy(() => cancel?.());
</script>

{#snippet footer()}
    <div class="plot-axis">
        {#each axis as tick}<span style:left={`${tick.x * 100}%`}>{tick.label}</span>{/each}
    </div>
{/snippet}
<GraphReadout text={chosen ? `${chosen.label}: ${chosen.readout}` : ''} {title} {tools} />
<GraphFrame {label} {ticks} {footer}>
    <div
        class="parameter-plot"
        bind:this={host}
        role="presentation"
        onpointermove={hoverTrace}
        onpointerleave={() => {
            if (!dragging) nearTrace = '';
        }}
    >
        <svg viewBox="0 0 1000 200" preserveAspectRatio="none" aria-hidden="true">
            {#each [0, 0.25, 0.5, 0.75, 1] as fraction}
                <path class="grid" d={`M0 ${fraction * 200}H1000M${fraction * 1000} 0V200`} />
            {/each}
            {#each traces as trace (trace.id)}
                <path
                    class="response-trace"
                    class:dashed={trace.dashed}
                    data-trace={trace.id}
                    style:stroke={trace.color ?? 'var(--editor-loop)'}
                    d={path(trace.points)}
                />
            {/each}
        </svg>
        {#each controls as handle (handle.id)}
            <button
                type="button"
                class="plot-handle"
                class:selected={selected === handle.id}
                class:trace-handle={!!handle.trace}
                class:trace-visible={nearTrace === handle.id ||
                    focused === handle.id ||
                    (dragging && selected === handle.id)}
                disabled={disabled || handle.disabled}
                aria-label={`${handle.label}: ${handle.readout}`}
                data-handle={handle.id}
                hidden={handle.x < 0 || handle.x > 1 || handle.y < 0 || handle.y > 1}
                use:wheelHandle={handle.id}
                use:hoverHelp={`${handle.label}: ${handle.readout}\n${handle.help ?? 'Drag to adjust. Shift-drag for fine movement. Arrow keys adjust values; Home/End move to the limits.'}`}
                style:left={`${handle.x * 100}%`}
                style:top={`${(1 - handle.y) * 100}%`}
                onpointerenter={() => (readout = handle.id)}
                onpointerleave={() => {
                    if (!dragging) readout = '';
                }}
                onfocus={(event) => {
                    selected = handle.id;
                    focused = handle.id;
                    if (event.currentTarget.matches(':focus-visible')) readout = handle.id;
                    onselect(handle.id);
                }}
                onpointerdown={(event) => drag(event, handle)}
                onkeydown={(event) => {
                    if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return;
                    event.preventDefault();
                    readout = handle.id;
                    onbegin(handle.id);
                    onkey?.(handle.id, event.key, event.shiftKey, event.altKey);
                }}
                onkeyup={onend}
                onblur={() => {
                    focused = '';
                    readout = '';
                    if (!dragging) onend();
                }}
                >{#if handle.trace}<span class="gain-icon"><Icon name="fit-width" size={18} /></span>{/if}</button
            >
        {/each}
    </div>
</GraphFrame>

<style>
    .parameter-plot {
        position: absolute;
        inset: 10px;
        min-width: 0;
        user-select: none;
    }
    svg {
        position: absolute;
        inset: 0;
        width: 100%;
        height: 100%;
        overflow: hidden;
    }
    path {
        fill: none;
        vector-effect: non-scaling-stroke;
    }
    .grid {
        stroke: var(--color-border);
        stroke-width: 1;
    }
    .response-trace {
        stroke-width: 2;
    }
    .dashed {
        stroke-dasharray: 4 4;
        opacity: 0.6;
    }
    .plot-handle {
        position: absolute;
        width: 13px;
        height: 13px;
        padding: 0;
        border: 2px solid var(--editor-loop);
        background: var(--color-panel-deep);
        border-radius: 50%;
        transform: translate(-50%, -50%);
        cursor: move;
        touch-action: none;
    }
    .plot-handle::before {
        content: '';
        position: absolute;
        inset: -5px;
    }
    .plot-handle[hidden] {
        display: none;
    }
    .plot-handle.selected {
        background: var(--editor-loop);
        z-index: 1;
    }
    .trace-handle {
        opacity: 0;
        pointer-events: none;
        border: 0;
        border-radius: 2px;
        width: 20px;
        height: 24px;
        cursor: ns-resize;
        color: var(--editor-loop);
        font-size: 22px;
    }
    .trace-handle.trace-visible,
    .trace-handle:focus {
        opacity: 1;
        pointer-events: auto;
    }
    .trace-handle.selected {
        color: var(--color-panel-deep);
    }
    .gain-icon {
        display: inline-flex;
        transform: rotate(90deg);
    }
    .plot-axis {
        position: relative;
        height: 20px;
        font-size: 10px;
        color: var(--color-text-muted);
    }
    .plot-axis span {
        position: absolute;
        transform: translateX(-50%);
        white-space: nowrap;
    }
    .plot-axis span:first-child {
        transform: none;
    }
    .plot-axis span:last-child {
        transform: translateX(-100%);
    }
</style>

<script lang="ts">
    import { onMount, onDestroy, type Snippet } from 'svelte';
    import GraphFrame from './GraphFrame.svelte';
    import KeyboardAxis from './KeyboardAxis.svelte';
    import type { GraphPoint } from './graphTypes';
    import GraphReadout from './GraphReadout.svelte';
    import { graphDrag } from './graphDrag';
    import { hoverHelp } from '../../lib/components/hoverHelp.svelte';
    let {
        points,
        minY = 0,
        maxY = 127,
        disabled = false,
        onchange,
        onbegin,
        onend,
        label,
        keyboard = false,
        extend = false,
        formatX = (x: number) => String(x),
        onselect = () => {},
        title,
        tools,
    }: {
        points: GraphPoint[];
        minY?: number;
        maxY?: number;
        disabled?: boolean;
        label: string;
        keyboard?: boolean;
        extend?: boolean;
        formatX?: (x: number) => string;
        onchange: (index: number, x: number, y: number) => void;
        onbegin: () => void;
        onend: () => void;
        onselect?: (index: number) => void;
        title?: Snippet;
        tools?: Snippet;
    } = $props();
    let host: HTMLDivElement;
    let canvas: HTMLCanvasElement;
    let width = $state(400);
    let height = $state(120);
    let selected = $state(0);
    let readout = $state<number | null>(null);
    let dragging = $state(false);
    let cancelDrag: (() => void) | undefined;
    let palette = $state({ curve: '#9ce1ba', grid: '#344044' });
    const px = (x: number) => 10 + (x / 127) * (width - 20);
    const py = (y: number) => 10 + ((maxY - y) / (maxY - minY)) * (height - 20);
    const point = $derived(readout === null ? undefined : points[readout]);
    const ticks = $derived([maxY, Math.round((minY + maxY) / 2), minY]);
    onMount(() => {
        const measure = () => {
            width = host.clientWidth;
            height = host.clientHeight;
            const style = getComputedStyle(host);
            palette = {
                curve: style.getPropertyValue('--editor-loop').trim() || '#9ce1ba',
                grid: style.getPropertyValue('--color-border').trim() || '#344044',
            };
        };
        const observer = new ResizeObserver(measure);
        observer.observe(host);
        measure();
        const theme = new MutationObserver(measure);
        theme.observe(document.documentElement, {
            attributes: true,
            attributeFilter: ['class', 'style', 'data-theme'],
        });
        theme.observe(document.body, { attributes: true, attributeFilter: ['class', 'style', 'data-theme'] });
        return () => {
            observer.disconnect();
            theme.disconnect();
        };
    });
    onDestroy(() => cancelDrag?.());
    $effect(() => {
        if (!canvas) return;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        const dpr = window.devicePixelRatio || 1;
        const pixelWidth = Math.max(1, Math.round(width * dpr)),
            pixelHeight = Math.max(1, Math.round(height * dpr));
        if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
        if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
        ctx.clearRect(0, 0, width, height);
        ctx.globalAlpha = 1;
        const color = palette.curve;
        ctx.strokeStyle = palette.grid;
        ctx.lineWidth = 1;
        for (let i = 0; i <= 4; i++) {
            ctx.beginPath();
            ctx.moveTo(10, 10 + (i / 4) * (height - 20));
            ctx.lineTo(width - 10, 10 + (i / 4) * (height - 20));
            ctx.stroke();
        }
        for (const x of keyboard ? [0, 24, 48, 72, 96, 120, 127] : points.map((p) => p.x)) {
            ctx.beginPath();
            ctx.moveTo(px(x), 10);
            ctx.lineTo(px(x), height - 10);
            ctx.stroke();
        }
        if (!points.length) return;
        const path = extend ? [{ ...points[0]!, x: 0 }, ...points, { ...points.at(-1)!, x: 127 }] : points;
        ctx.beginPath();
        path.forEach((p, i) => (i ? ctx.lineTo(px(p.x), py(p.y)) : ctx.moveTo(px(p.x), py(p.y))));
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.lineTo(px(path.at(-1)!.x), py(Math.max(minY, 0)));
        ctx.lineTo(px(path[0]!.x), py(Math.max(minY, 0)));
        ctx.closePath();
        ctx.globalAlpha = 0.1;
        ctx.fillStyle = color;
        ctx.fill();
    });
    function drag(event: PointerEvent, index: number) {
        if (disabled || points[index]?.disabled || points[index]?.fixed) return;
        cancelDrag?.();
        event.preventDefault();
        selected = index;
        const target = event.currentTarget as HTMLButtonElement;
        target.focus();
        onbegin();
        dragging = true;
        readout = index;
        const rect = host.getBoundingClientRect();
        const origin = points[index]!;
        cancelDrag = graphDrag(
            event,
            { x: origin.x / 127, y: (origin.y - minY) / (maxY - minY) },
            { width: (rect.width * (width - 20)) / width, height: (rect.height * (height - 20)) / height },
            (x, y) =>
                onchange(index, origin.movableX ? Math.round(x * 127) : origin.x, Math.round(minY + y * (maxY - minY))),
            () => {
                dragging = false;
                readout = null;
                onend();
                cancelDrag = undefined;
            },
        );
    }
    function key(event: KeyboardEvent, index: number) {
        const p = points[index]!;
        if (disabled || p.disabled || p.fixed) return;
        let { x, y } = p;
        const step = event.shiftKey ? 8 : 1;
        if (event.key === 'ArrowUp') y += step;
        else if (event.key === 'ArrowDown') y -= step;
        else if (event.key === 'ArrowLeft' && p.movableX) x -= step;
        else if (event.key === 'ArrowRight' && p.movableX) x += step;
        else if (event.key === 'Home') y = minY;
        else if (event.key === 'End') y = maxY;
        else return;
        event.preventDefault();
        readout = index;
        onbegin();
        onchange(index, Math.max(0, Math.min(127, x)), Math.max(minY, Math.min(maxY, y)));
    }
</script>

{#snippet footer()}
    {#if keyboard}
        <KeyboardAxis marked={points.map((p) => p.x)} formatNote={formatX} />
    {:else}
        <div class="stage-labels">
            {#each points as p}<span style:left={`${(p.x / 127) * 100}%`}>{p.label}</span>{/each}
        </div>
    {/if}
{/snippet}
<GraphReadout text={point ? `${keyboard ? formatX(point.x) : point.label}: ${point.y}` : ''} {title} {tools} />
<GraphFrame {ticks} {label} {footer}>
    <div class="breakpoint-graph" bind:this={host}>
        <canvas bind:this={canvas} aria-hidden="true"></canvas>
        {#each points as p, index}
            {#if !p.fixed && Number.isFinite(p.x) && Number.isFinite(p.y)}
                <button
                    disabled={disabled || p.disabled}
                    class:selected={selected === index}
                    aria-label={`${p.label}: ${formatX(p.x)}, ${p.y}`}
                    use:hoverHelp={`${p.label}: ${formatX(p.x)}, ${p.y}\n${p.movableX ? 'Drag horizontally for the breakpoint and vertically for its value. Left/Right moves the breakpoint; Up/Down adjusts its value.' : 'Drag vertically or use Up/Down to adjust the value.'} Shift-drag is finer; Shift+arrows moves by 8. Home/End selects the value limits.`}
                    style:left={`${px(p.x)}px`}
                    style:top={`${py(p.y)}px`}
                    onpointerenter={() => (readout = index)}
                    onpointerleave={() => {
                        if (!dragging) readout = null;
                    }}
                    onfocus={(event) => {
                        selected = index;
                        if (event.currentTarget.matches(':focus-visible')) readout = index;
                        onselect(index);
                    }}
                    onpointerdown={(event) => drag(event, index)}
                    onkeydown={(event) => key(event, index)}
                    onkeyup={onend}
                    onblur={() => {
                        readout = null;
                        if (!dragging) onend();
                    }}
                ></button>
            {/if}
        {/each}
    </div>
</GraphFrame>

<style>
    .breakpoint-graph {
        position: relative;
        flex: 1;
        min-width: 0;
        min-height: 0;
        background: var(--color-panel-deep);
    }
    canvas {
        position: absolute;
        inset: 0;
        width: 100%;
        height: 100%;
    }
    button {
        position: absolute;
        width: 13px;
        height: 13px;
        padding: 0;
        border: 2px solid var(--editor-loop);
        background: var(--color-panel-deep);
        border-radius: 50%;
        transform: translate(-50%, -50%);
        touch-action: none;
        cursor: move;
    }
    button.selected {
        background: var(--editor-loop);
        z-index: 1;
    }
    button::before {
        content: '';
        position: absolute;
        inset: -6px;
    }
    button:focus-visible {
        outline: 2px solid var(--color-text);
        outline-offset: 2px;
    }
    .stage-labels {
        position: relative;
        height: 20px;
        color: var(--color-text-muted);
        font-size: 10px;
    }
    .stage-labels span {
        position: absolute;
        transform: translateX(-50%);
        white-space: nowrap;
    }
    .stage-labels span:first-child {
        transform: none;
    }
    .stage-labels span:last-child {
        transform: translateX(-100%);
    }
</style>

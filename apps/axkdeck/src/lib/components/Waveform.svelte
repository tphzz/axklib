<script lang="ts">
    import { onMount } from 'svelte';
    import type { WaveformBin } from '../types';
    import { canvasPixelSize, waveformPixelColumns } from '../waveformCanvas';
    import { waveformContentRatio, waveformFrameWindow } from '../waveformTimeline';

    interface Props {
        values: readonly WaveformBin[];
        large?: boolean;
        playheadRatio?: number;
        sourceFrameCount?: number;
        timelineFrameCount?: number;
        windowStartFrame?: number;
        windowLengthFrames?: number;
        loopStartFrame?: number;
        loopLengthFrames?: number;
    }

    let {
        values,
        large = false,
        playheadRatio = 0,
        sourceFrameCount = 0,
        timelineFrameCount = 0,
        windowStartFrame = 0,
        windowLengthFrames = 0,
        loopStartFrame = 0,
        loopLengthFrames = 0,
    }: Props = $props();
    let canvas: HTMLCanvasElement;
    const contentRatio = $derived(waveformContentRatio(sourceFrameCount, timelineFrameCount));
    const normalizedPlayheadRatio = $derived(Math.max(0, Math.min(1, playheadRatio)));
    const waveWindow = $derived(waveformFrameWindow(windowStartFrame, windowLengthFrames, timelineFrameCount));
    const loopWindow = $derived(waveformFrameWindow(loopStartFrame, loopLengthFrames, timelineFrameCount));

    function draw(): void {
        if (!canvas || typeof CanvasRenderingContext2D === 'undefined') return;
        const bounds = canvas.getBoundingClientRect();
        const { width, height } = canvasPixelSize(bounds.width, bounds.height, window.devicePixelRatio || 1);
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }
        const context = canvas.getContext('2d');
        if (!context) return;
        context.clearRect(0, 0, width, height);
        context.fillStyle = getComputedStyle(canvas).color;
        context.globalAlpha = 0.75;
        context.fillRect(0, Math.min(height - 1, Math.floor(height / 2)), width, 1);
        context.globalAlpha = 1;
        for (const column of waveformPixelColumns(values, contentRatio, bounds.width, width, height)) {
            context.fillRect(column.x, column.y, column.width, column.height);
        }
    }

    $effect(() => {
        values;
        contentRatio;
        draw();
    });

    onMount(() => {
        const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(draw);
        observer?.observe(canvas);
        draw();
        return () => observer?.disconnect();
    });
</script>

<div
    class="waveform-frame"
    aria-hidden="true"
    data-content-ratio={contentRatio}
    data-playhead-ratio={normalizedPlayheadRatio}
    data-window-start-ratio={waveWindow?.startRatio}
    data-window-end-ratio={waveWindow?.endRatio}
>
    <canvas bind:this={canvas} class:large class="waveform"></canvas>
    {#if waveWindow}
        {#if waveWindow.startRatio > 0}
            <span class="waveform-outside-window" style:width={`${waveWindow.startRatio * 100}%`}></span>
        {/if}
        {#if waveWindow.endRatio < 1}
            <span
                class="waveform-outside-window waveform-outside-window-end"
                style:left={`${waveWindow.endRatio * 100}%`}
            ></span>
        {/if}
        <span class="waveform-window-boundary" style:left={`${waveWindow.startRatio * 100}%`}></span>
        <span class="waveform-window-boundary" style:left={`${waveWindow.endRatio * 100}%`}></span>
    {/if}
    {#if loopWindow}
        <span class="waveform-loop-boundary" style:left={`${loopWindow.startRatio * 100}%`}></span>
        <span class="waveform-loop-boundary" style:left={`${loopWindow.endRatio * 100}%`}></span>
    {/if}
    {#if normalizedPlayheadRatio > 0}
        <span
            class="waveform-playhead"
            style:left={normalizedPlayheadRatio === 1 ? 'calc(100% - 1px)' : `${normalizedPlayheadRatio * 100}%`}
        ></span>
    {/if}
</div>

<script lang="ts">
    import { onMount } from 'svelte';
    import type { WaveformBin } from '../types';
    import { waveformContentRatio } from '../waveformTimeline';

    interface Props {
        values: readonly WaveformBin[];
        large?: boolean;
        playheadRatio?: number;
        sourceFrameCount?: number;
        timelineFrameCount?: number;
    }

    let { values, large = false, playheadRatio = 0, sourceFrameCount = 0, timelineFrameCount = 0 }: Props = $props();
    let canvas: HTMLCanvasElement;
    const contentRatio = $derived(waveformContentRatio(sourceFrameCount, timelineFrameCount));
    const normalizedPlayheadRatio = $derived(Math.max(0, Math.min(1, playheadRatio)));

    function draw(): void {
        if (!canvas || typeof CanvasRenderingContext2D === 'undefined') return;
        const bounds = canvas.getBoundingClientRect();
        const scale = Math.max(1, window.devicePixelRatio || 1);
        const width = Math.max(1, Math.round(bounds.width * scale));
        const height = Math.max(1, Math.round(bounds.height * scale));
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
        }
        const context = canvas.getContext('2d');
        if (!context) return;
        context.clearRect(0, 0, width, height);
        context.strokeStyle = getComputedStyle(canvas).color;
        context.lineWidth = Math.max(1, scale);
        const center = height / 2;
        context.globalAlpha = 0.75;
        context.beginPath();
        context.moveTo(0, center);
        context.lineTo(width, center);
        context.stroke();
        if (values.length > 0 && contentRatio > 0) {
            const contentWidth = Math.max(1, Math.round(width * contentRatio));
            const peak = Math.max(1, ...values.flatMap((value) => [Math.abs(value.minimum), Math.abs(value.maximum)]));
            context.globalAlpha = 1;
            context.beginPath();
            for (let pixel = 0; pixel < contentWidth; pixel += Math.max(1, Math.round(scale))) {
                const first = Math.floor((pixel / contentWidth) * values.length);
                const last = Math.max(first + 1, Math.ceil(((pixel + scale) / contentWidth) * values.length));
                let minimum = 0;
                let maximum = 0;
                for (let index = first; index < Math.min(last, values.length); index += 1) {
                    minimum = Math.min(minimum, values[index]?.minimum ?? 0);
                    maximum = Math.max(maximum, values[index]?.maximum ?? 0);
                }
                context.moveTo(pixel, center - (maximum / peak) * center);
                context.lineTo(pixel, center - (minimum / peak) * center);
            }
            context.stroke();
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
>
    <canvas bind:this={canvas} class:large class="waveform"></canvas>
    {#if normalizedPlayheadRatio > 0}
        <span
            class="waveform-playhead"
            style:left={normalizedPlayheadRatio === 1 ? 'calc(100% - 1px)' : `${normalizedPlayheadRatio * 100}%`}
        ></span>
    {/if}
</div>

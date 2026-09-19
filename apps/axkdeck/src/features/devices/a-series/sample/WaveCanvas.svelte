<script lang="ts">
    import { onMount } from 'svelte';
    import type { WaveformBin } from '../../../../lib/types';
    import { waveformPixelColumns } from '../../../../lib/waveformCanvas';
    let {
        bins,
        pcm,
        frames,
        start = 0,
        end = frames,
        markers = [],
        overview = false,
    }: {
        bins: readonly WaveformBin[];
        pcm?: Float32Array;
        frames: number;
        start?: number;
        end?: number;
        markers?: number[];
        overview?: boolean;
    } = $props();
    let canvas: HTMLCanvasElement;
    let size = $state({ width: 1, height: 1 });
    const columns = $derived.by(() => {
        const count = Math.max(1, Math.round(size.width));
        if (!pcm)
            return bins.slice(
                Math.floor((start / frames) * bins.length),
                Math.max(1, Math.ceil((end / frames) * bins.length)),
            );
        return Array.from({ length: count }, (_, index) => {
            const first = Math.floor(start + ((end - start) * index) / count);
            const last = Math.min(
                pcm.length,
                Math.max(first + 1, Math.ceil(start + ((end - start) * (index + 1)) / count)),
            );
            let minimum = 0;
            let maximum = 0;
            for (let i = first; i < last; i++) {
                minimum = Math.min(minimum, pcm[i]!);
                maximum = Math.max(maximum, pcm[i]!);
            }
            return { minimum, maximum };
        });
    });
    function draw() {
        if (!canvas) return;
        const context = canvas.getContext('2d');
        if (!context) return;
        const ratio = window.devicePixelRatio || 1;
        canvas.width = Math.max(1, Math.round(size.width * ratio));
        canvas.height = Math.max(1, Math.round(size.height * ratio));
        const styles = getComputedStyle(canvas);
        const wave = styles.getPropertyValue('--editor-wave').trim() || '#7eafc8';
        const loop = styles.getPropertyValue('--editor-loop').trim() || '#9ce1ba';
        const [waveStart = 0, waveEnd = frames, loopStart = 0, loopEnd = 0] = markers;
        const pixelColumns = waveformPixelColumns(
            columns,
            1,
            size.width,
            canvas.width,
            canvas.height - (overview ? 0 : 18 * ratio),
        );
        for (const column of pixelColumns) {
            const frame = start + (column.x / canvas.width) * (end - start);
            context.globalAlpha = !overview && (frame < waveStart || frame >= waveEnd) ? 0.23 : 0.85;
            context.fillStyle = !overview && frame >= loopStart && frame < loopEnd ? loop : wave;
            context.fillRect(column.x, column.y + (overview ? 0 : 9 * ratio), column.width, column.height);
        }
        context.globalAlpha = 0.3;
        context.fillStyle = wave;
        context.fillRect(0, Math.floor(canvas.height / 2), canvas.width, 1);
    }
    $effect(() => {
        columns;
        markers;
        size;
        draw();
    });
    onMount(() => {
        const measure = () => {
            const box = canvas.getBoundingClientRect();
            size = { width: box.width, height: box.height };
        };
        const observer = new ResizeObserver(measure);
        observer.observe(canvas);
        measure();
        return () => observer.disconnect();
    });
</script>

<canvas bind:this={canvas} aria-hidden="true"></canvas>

<style>
    canvas {
        display: block;
        width: 100%;
        height: 100%;
    }
</style>

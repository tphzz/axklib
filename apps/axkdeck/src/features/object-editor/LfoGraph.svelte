<script lang="ts">
    import GraphFrame from './GraphFrame.svelte';
    import { lfoTimeline, lfoBuildup, type LfoWave } from './lfoShape';
    let {
        wave,
        speed,
        delay,
        keyOnSync,
        traces,
        label,
    }: {
        wave: LfoWave;
        speed: number;
        delay: number;
        keyOnSync: boolean;
        traces: { id: string; depth: number; inverted: boolean; color: string }[];
        label: string;
    } = $props();
    const points = $derived(lfoTimeline(wave, speed, delay, keyOnSync));
    const buildup = $derived(lfoBuildup(delay));
    const guide = (sign: number) =>
        buildup.map((p, i) => `${i ? 'L' : 'M'}${p.x * 1000},${100 - p.y * sign * 90}`).join(' ');
    const path = (amount: number) =>
        points.map((p, i) => `${i ? 'L' : 'M'}${p.x * 1000},${100 - p.y * amount * 90}`).join(' ');
</script>

{#snippet footer()}<div class="phase-axis"><span>Note on</span><span>Relative time</span></div>{/snippet}
<GraphFrame ticks={['+', '0', '-']} {footer} {label}>
    <svg class="lfo-plot" viewBox="0 0 1000 200" preserveAspectRatio="none" aria-hidden="true">
        {#each [0, 250, 500, 750, 1000] as x}<path class="grid" d={`M${x} 0V200`} />{/each}
        {#each [10, 100, 190] as y}<path class="grid" d={`M0 ${y}H1000`} />{/each}
        <path class="reference" d={path(1)} />
        {#if delay > 0}
            <path class="buildup" data-trace="buildup" d={guide(1)} />
            <path class="buildup" d={guide(-1)} />
        {/if}
        {#if delay > 0}<path class="delay-marker" d={`M${(delay / 127) * 350} 0V200`} />{/if}
        {#each traces as trace (trace.id)}
            <path
                class="modulation"
                data-trace={trace.id}
                style:stroke={trace.color}
                d={path(Math.max(0, Math.min(1, trace.depth)) * (trace.inverted ? -1 : 1))}
            />
        {/each}
    </svg>
</GraphFrame>

<style>
    .lfo-plot {
        position: absolute;
        top: 10px;
        left: 10px;
        width: calc(100% - 20px);
        height: calc(100% - 20px);
        overflow: visible;
    }
    path {
        fill: none;
        vector-effect: non-scaling-stroke;
    }
    .grid {
        stroke: var(--color-border);
        stroke-width: 1;
    }
    .reference {
        stroke: var(--color-text-muted);
        opacity: 0.45;
        stroke-width: 1;
        stroke-dasharray: 3 4;
    }
    .modulation {
        stroke: var(--editor-loop);
        stroke-width: 2;
    }
    .buildup {
        stroke: var(--color-text-muted);
        opacity: 0.35;
        stroke-width: 1;
        stroke-dasharray: 1 3;
    }
    .delay-marker {
        stroke: var(--color-text-muted);
        stroke-width: 1;
        stroke-dasharray: 3 4;
    }
    .phase-axis {
        display: flex;
        justify-content: space-between;
        height: 20px;
        color: var(--color-text-muted);
        font-size: 10px;
    }
</style>

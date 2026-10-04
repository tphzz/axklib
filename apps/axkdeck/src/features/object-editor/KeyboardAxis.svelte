<script lang="ts">
    import { measureWidth } from './measureWidth';
    let {
        marked = [],
        formatNote,
        start = 0,
        end = 127,
        ranges = [],
        roots = [],
    }: {
        marked?: number[];
        formatNote: (note: number) => string;
        start?: number;
        end?: number;
        ranges?: { low: number; high: number }[];
        roots?: number[];
    } = $props();
    const notes = Array.from({ length: 128 }, (_, i) => i);
    const black = (note: number) => [1, 3, 6, 8, 10].includes(note % 12);
    const highlighted = (note: number) =>
        marked.includes(note) || ranges.some((range) => note >= range.low && note <= range.high);
    const span = $derived(end - start + 1);
</script>

<div
    class="keyboard"
    use:measureWidth={{ scope: 'keyboard' }}
    role="img"
    aria-label={`Keyboard ${formatNote(start)} to ${formatNote(end)}${roots.length ? `, root ${roots.map(formatNote).join(', ')}` : ''}`}
>
    <svg class="keys" viewBox={`${start} 0 ${span} 28`} preserveAspectRatio="none" aria-hidden="true">
        {#each notes as note}
            {#if !black(note)}<rect
                    class="white"
                    class:marked={highlighted(note)}
                    x={note - (black(note - 1) ? 0.5 : 0)}
                    width={1 + (black(note - 1) ? 0.5 : 0) + (black(note + 1) ? 0.5 : 0)}
                    height="28"
                />{/if}
        {/each}
        {#each notes.filter(black) as note}<rect
                class="black"
                class:marked={highlighted(note)}
                x={note + 0.08}
                width="0.84"
                height="18"
            />{/each}
        {#each roots.filter((note) => note >= start && note <= end) as note}<rect
                class="root"
                x={note + 0.25}
                y="21"
                width="0.5"
                height="5"
            />{/each}
        {#each marked.filter((note) => note >= start && note <= end) as note}<path
                class="mark"
                d={`M${note + 0.5} 0V28`}
            />{/each}
    </svg>
    <div class="key-labels" aria-hidden="true">
        {#each notes.filter((note) => note >= start && note <= end && note % (span > 72 ? 24 : 12) === 0) as note}<span
                style:left={`${((note + 0.5 - start) / span) * 100}%`}>{formatNote(note)}</span
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
        fill: #b5c0c2;
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
    .root {
        fill: var(--color-warning, #e4b15b);
        stroke: none;
    }
    .mark {
        stroke: var(--color-accent);
        stroke-width: 1;
        vector-effect: non-scaling-stroke;
    }
    .key-labels {
        position: relative;
        height: 16px;
        color: var(--color-text-muted);
        font: 10px var(--font-mono, monospace);
    }
    .key-labels span {
        position: absolute;
    }
    :global([data-keyboard-under~='420']) .key-labels span:nth-child(even) {
        display: none;
    }
</style>

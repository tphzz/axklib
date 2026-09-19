<script lang="ts">
    import { measureWidth } from './measureWidth';
    let { marked = [], formatNote }: { marked?: number[]; formatNote: (note: number) => string } = $props();
    const notes = Array.from({ length: 128 }, (_, i) => i);
    const black = (note: number) => [1, 3, 6, 8, 10].includes(note % 12);
</script>

<div
    class="keyboard"
    use:measureWidth={{ scope: 'keyboard' }}
    aria-label={`Keyboard ${formatNote(0)} to ${formatNote(127)}`}
>
    <svg class="keys" viewBox="0 0 127 21" preserveAspectRatio="none" aria-hidden="true">
        <rect width="127" height="21" class="keybed" />
        {#each notes as note}
            {#if marked.includes(note)}<rect class="marked" x={note - 0.5} width="1" height="21" />{/if}
            {#if black(note)}<rect
                    class="black"
                    class:marked={marked.includes(note)}
                    x={note - 0.5}
                    width="1"
                    height="14"
                />{/if}
            <path class="key-edge" d={`M${note - 0.5} 0V21`} />
        {/each}
    </svg>
    <div class="key-labels" aria-hidden="true">
        {#each [0, 24, 48, 72, 96, 120, 127] as note}<span
                style:left={`${(note / 127) * 100}%`}
                class:end={note === 127}>{formatNote(note)}</span
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
        height: 21px;
        overflow: hidden;
        shape-rendering: crispEdges;
    }
    .keybed {
        fill: #8a9fa3;
    }
    .black {
        fill: #25383c;
    }
    .marked {
        fill: var(--editor-loop);
    }
    .key-edge {
        stroke: var(--color-panel-deep);
        stroke-width: 1px;
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
    .key-labels .end {
        transform: translateX(-100%);
    }
    :global([data-keyboard-under~='420']) .key-labels span:nth-child(even) {
        display: none;
    }
</style>

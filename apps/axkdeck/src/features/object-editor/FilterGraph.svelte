<script lang="ts">
    import type { Snippet } from 'svelte';
    import ParameterGraph, { type PlotHandle } from './ParameterGraph.svelte';
    import { filterResponse, type FilterStage } from './filterResponse';
    let {
        stages,
        gain,
        handles,
        disabled,
        onchange,
        onkey,
        onbegin,
        onend,
        title,
    }: {
        stages: FilterStage[];
        gain: number;
        handles: PlotHandle[];
        disabled: boolean;
        onchange: (id: string, x: number, y: number) => void;
        onkey: (id: string, key: string, shift: boolean) => void;
        onbegin: () => void;
        onend: () => void;
        title?: Snippet;
    } = $props();
    const points = $derived(filterResponse(stages, gain));
</script>

<ParameterGraph
    {title}
    label="Filter response"
    ticks={['+', 'Relative', '-']}
    traces={[{ id: 'filter', points }]}
    {handles}
    {disabled}
    axis={[
        { x: 0, label: 'Cutoff 0' },
        { x: 0.5, label: '64' },
        { x: 1, label: '127' },
    ]}
    {onchange}
    {onkey}
    {onbegin}
    {onend}
/>

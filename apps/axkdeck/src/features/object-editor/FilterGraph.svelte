<script lang="ts">
    import type { Snippet } from 'svelte';
    import ParameterGraph, { type PlotHandle } from './ParameterGraph.svelte';
    import { filterResponse, filterResponseAt, type FilterStage } from './filterResponse';
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
        onaltdrag,
        onwheel,
        ongainchange,
    }: {
        stages: FilterStage[];
        gain: number;
        handles: Omit<PlotHandle, 'y'>[];
        disabled: boolean;
        onchange: (id: string, x: number, y: number) => void;
        onkey: (id: string, key: string, shift: boolean, alt: boolean) => void;
        onbegin: (id: string) => void;
        onend: () => void;
        title?: Snippet;
        onaltdrag?: (id: string, delta: number) => void;
        onwheel?: (id: string, event: WheelEvent) => void;
        ongainchange?: (gain: number) => void;
    } = $props();
    const response = $derived(filterResponse(stages, gain));
    const anchored = $derived(handles.map((handle) => ({ ...handle, y: filterResponseAt(stages, gain, handle.x) })));
    let gainBounds = $state<[number, number]>();
    const minimum = $derived(
        gainBounds?.[0] ?? Math.min(0, ...response.map((point) => point.y), ...anchored.map((handle) => handle.y)),
    );
    const maximum = $derived(
        gainBounds?.[1] ?? Math.max(1, ...response.map((point) => point.y), ...anchored.map((handle) => handle.y)),
    );
    const project = (y: number) => (y - minimum) / (maximum - minimum);
    const points = $derived(response.map((point) => ({ ...point, y: project(point.y) })));
    const controls = $derived(anchored.map((handle) => ({ ...handle, y: project(handle.y) })));
</script>

<ParameterGraph
    {title}
    label="Filter response"
    ticks={['+', 'Relative', '-']}
    traces={[{ id: 'filter', points }]}
    handles={controls}
    {disabled}
    axis={[
        { x: 0, label: 'Cutoff 0' },
        { x: 0.5, label: '64' },
        { x: 1, label: '127' },
    ]}
    onchange={(id, x, y) => {
        if (id === 'gain') ongainchange?.((minimum + y * (maximum - minimum) - filterResponseAt(stages, 0, x)) / 0.2);
        else onchange(id, x, y);
    }}
    {onkey}
    onbegin={(id) => {
        if (id === 'gain') gainBounds = [minimum, maximum];
        onbegin(id);
    }}
    onend={() => {
        onend();
        gainBounds = undefined;
    }}
    {onaltdrag}
    {onwheel}
/>

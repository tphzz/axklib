<script lang="ts">
    import type { Snippet } from 'svelte';
    import ParameterGraph, { type PlotPoint, type PlotHandle } from './ParameterGraph.svelte';
    let {
        points,
        coefficientPoints = [],
        frequency,
        gain,
        readout,
        disabled = false,
        blocked = [],
        onbegin,
        onend,
        onchange,
        onkey,
        title,
        tools,
        onaltdrag,
        onwheel,
    }: {
        points: PlotPoint[];
        coefficientPoints?: PlotPoint[];
        frequency: number;
        gain: number;
        readout: string;
        disabled?: boolean;
        blocked?: string[];
        onbegin: () => void;
        onend: () => void;
        onchange: (control: string, x: number, y: number) => void;
        onkey: (control: string, key: string, shift: boolean, alt: boolean) => void;
        title?: Snippet;
        tools?: Snippet;
        onaltdrag?: (control: string, delta: number) => void;
        onwheel?: (control: string, event: WheelEvent) => void;
    } = $props();
    const handles = $derived<PlotHandle[]>([
        {
            id: 'frequency-gain',
            label: 'EQ frequency / gain',
            readout,
            x: frequency,
            y: (gain + 18) / 36,
            horizontal: !blocked.includes('frequency'),
            vertical: !blocked.includes('gain'),
            disabled: blocked.includes('frequency') && blocked.includes('gain') && blocked.includes('width'),
        },
    ]);
</script>

<ParameterGraph
    {title}
    {tools}
    {onaltdrag}
    {onwheel}
    label="Sample EQ response"
    ticks={['+18 dB', '0', '-18 dB']}
    traces={[
        { id: 'eq', points },
        ...(coefficientPoints.length
            ? [{ id: 'coefficients', points: coefficientPoints, dashed: true, color: 'var(--editor-wave)' }]
            : []),
    ]}
    {handles}
    {disabled}
    axis={[
        { x: 0, label: '32 Hz' },
        { x: Math.log(1000 / 32) / Math.log(500), label: '1 kHz' },
        { x: 1, label: '16 kHz' },
    ]}
    {onbegin}
    {onend}
    {onchange}
    {onkey}
/>

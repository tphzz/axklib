<script lang="ts">
    import { BankDraft } from '../bank/draft.svelte';
    import type { Snippet } from 'svelte';
    import ParameterGraph, { type PlotHandle } from '../../../object-editor/ParameterGraph.svelte';
    import type { EditorDraftState } from '../../../object-editor/draft.svelte';
    import { sampleEnvelope, envelopeDurations, envelopeRate, type SampleEnvelope } from './envelope';
    import { envelopeViewport } from './envelopeViewport';
    import Icon from '../../../../lib/components/Icon.svelte';
    let {
        draft,
        kind,
        disabled,
        blocked,
        onselect,
        title,
        values = draft.values,
        baseValues,
        rateOffsets,
    }: {
        draft: EditorDraftState;
        kind: SampleEnvelope;
        disabled: boolean;
        blocked: string[];
        onselect: (key: string) => void;
        title?: Snippet;
        values?: EditorDraftState['values'];
        baseValues?: EditorDraftState['values'];
        rateOffsets?: Record<string, { key: string; base: number }>;
    } = $props();
    const min = $derived(kind === 'aeg' ? 0 : -127);
    let selected = $state('');
    const points = $derived(sampleEnvelope(kind, values));
    const total = $derived(envelopeDurations(kind, values).reduce((sum, duration) => sum + duration, 32));
    const baseTotal = $derived(
        baseValues ? envelopeDurations(kind, baseValues).reduce((sum, duration) => sum + duration, 32) : 0,
    );
    const extent = $derived(Math.max(total, baseTotal));
    const viewport = $derived(envelopeViewport(draft, kind, extent));
    const canEdit = (key?: string) =>
        !!key && !disabled && !blocked.includes(key) && Number.isFinite(draft.values[rateOffsets?.[key]?.key ?? key]);
    const readRate = (key: string) =>
        rateOffsets?.[key]
            ? `offset ${draft.values[rateOffsets[key]!.key]}, base ${rateOffsets[key]!.base}, effective ${values[key]}`
            : String(values[key] ?? 'Unavailable');
    const x = (index: number) => ((points[index]!.x / 127) * total) / viewport.span;
    const y = (value: number) => (value - min) / (127 - min);
    const handles = $derived<PlotHandle[]>(
        points
            .flatMap((point, index) =>
                point.fixed
                    ? []
                    : [
                          {
                              id: String(index),
                              label: index === 4 ? 'Release' : point.label,
                              readout: `${point.y}${point.rateParameter ? `, ${point.rateParameter.split('.')[1]!.replace('_', ' ')} ${readRate(point.rateParameter)}` : ''}${draft instanceof BankDraft ? `. ${draft.sourceDescription([point.parameter, point.rateParameter].filter((key): key is string => !!key))}` : ''}`,
                              help: `${canEdit(point.rateParameter) ? 'Drag horizontally to adjust the rate. Left shortens the stage; Right lengthens it. ' : ''}${canEdit(point.parameter) ? 'Drag vertically or use Up/Down to adjust the level. ' : ''}Shift-drag is finer; Shift+arrows moves by 8. Home/End selects the limits.`,
                              x: x(index),
                              y: y(point.y),
                              horizontal: canEdit(point.rateParameter),
                              unboundedHorizontal: true,
                              vertical: canEdit(point.parameter),
                              disabled: !canEdit(point.rateParameter) && !canEdit(point.parameter),
                          },
                      ],
            )
            .filter((handle) => handle.horizontal || handle.vertical || handle.disabled),
    );
    $effect(() => {
        const current = handles.find((handle) => handle.id === selected);
        const available = handles.find((handle) => !handle.disabled);
        if (!current || (current.disabled && available)) selected = available?.id ?? handles[0]?.id ?? '';
    });
    function select(id: string) {
        const point = points[Number(id)]!;
        onselect(rateOffsets?.[point.rateParameter ?? '']?.key ?? point.parameter ?? point.rateParameter!);
    }
    function selectStage(event: Event) {
        const id = (event.currentTarget as HTMLSelectElement).value;
        const handle = handles.find((handle) => handle.id === id && !handle.disabled);
        if (!handle) return;
        selected = id;
        if (handle.x > 1) viewport.fit(extent);
        select(id);
    }
    function begin() {
        draft.beginGesture();
    }
    function end() {
        draft.endGesture();
    }
    function change(id: string, nextX: number, nextY: number) {
        const index = Number(id),
            point = points[index]!;
        const patch: Record<string, number> = {};
        if (canEdit(point.parameter)) patch[point.parameter!] = Math.round(min + nextY * (127 - min));
        if (canEdit(point.rateParameter)) {
            const previous = index - 1;
            const before = (points[previous]!.x / 127) * total;
            const binding = rateOffsets?.[point.rateParameter!];
            const rate = envelopeRate(nextX * viewport.span - before);
            patch[binding?.key ?? point.rateParameter!] = binding
                ? Math.max(-127, Math.min(127, rate - binding.base))
                : rate;
        }
        draft.patch(patch);
    }
</script>

{#snippet tools()}
    <select
        class="envelope-stage"
        aria-label="Envelope stage"
        title="Envelope stage"
        value={selected}
        disabled={disabled || !handles.some((handle) => !handle.disabled)}
        onchange={selectStage}
    >
        {#each handles as handle (handle.id)}
            <option value={handle.id} disabled={handle.disabled}>{handle.label}</option>
        {/each}
    </select>
    <button
        class="editor-icon"
        aria-label="Zoom envelope out"
        title="Zoom out"
        disabled={viewport.span >= viewport.maximum}
        onclick={() => viewport.zoom(1.5)}><Icon name="zoom-out" size={14} /></button
    >
    <button
        class="editor-icon"
        aria-label="Zoom envelope in"
        title="Zoom in"
        disabled={viewport.span <= viewport.minimum}
        onclick={() => viewport.zoom(1 / 1.5)}><Icon name="zoom-in" size={14} /></button
    >
    <button
        class="editor-icon"
        aria-label="Fit envelope to width"
        title="Fit to width"
        onclick={() => viewport.fit(extent)}><Icon name="fit-width" size={14} /></button
    >
{/snippet}
<ParameterGraph
    {title}
    {tools}
    label="Envelope levels by stage"
    ticks={[127, Math.round((127 + min) / 2), min]}
    traces={[
        ...(baseValues
            ? [
                  {
                      id: 'base-envelope',
                      color: 'var(--color-text-muted)',
                      dashed: true,
                      points: sampleEnvelope(kind, baseValues).map((point) => ({
                          x: ((point.x / 127) * baseTotal) / viewport.span,
                          y: y(point.y),
                      })),
                  },
              ]
            : []),
        { id: 'envelope', points: points.map((point, index) => ({ x: x(index), y: y(point.y) })) },
    ]}
    {handles}
    {disabled}
    bind:selected
    retainReadout
    axis={[
        { x: 0, label: 'Note on' },
        { x: 1, label: 'Relative time' },
    ]}
    onbegin={begin}
    onend={end}
    onchange={change}
    onselect={select}
    onkey={(id, key, shift) => {
        const handle = handles.find((item) => item.id === id)!;
        const point = points[Number(id)]!;
        const horizontal =
            ['ArrowLeft', 'ArrowRight'].includes(key) || (['Home', 'End'].includes(key) && !handle.vertical);
        if (horizontal ? !handle.horizontal : !handle.vertical) return;
        const parameter = horizontal ? point.rateParameter : point.parameter;
        if (!canEdit(parameter)) return;
        const binding = rateOffsets?.[parameter!];
        const storedKey = binding?.key ?? parameter!;
        const current = Number(draft.values[storedKey]);
        const low = binding ? -127 : horizontal ? 0 : min;
        const delta = (['ArrowLeft', 'ArrowUp'].includes(key) ? 1 : -1) * (shift ? 8 : 1);
        draft.set(
            storedKey,
            key === 'Home' ? low : key === 'End' ? 127 : Math.max(low, Math.min(127, current + delta)),
        );
    }}
/>

<style>
    .envelope-stage {
        width: 88px;
        height: 22px;
        padding: 0 4px;
        border: 1px solid var(--color-border);
        border-radius: 3px;
        background: var(--color-panel-deep);
        color: var(--color-text);
        font: inherit;
        font-size: 11px;
        color-scheme: dark;
    }
    .envelope-stage:focus-visible {
        outline: 1px solid var(--color-accent);
        outline-offset: 2px;
    }
</style>

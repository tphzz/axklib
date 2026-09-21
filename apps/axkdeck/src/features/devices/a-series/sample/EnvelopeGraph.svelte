<script lang="ts">
    import { BankDraft } from '../bank/draft.svelte';
    import type { Snippet } from 'svelte';
    import ParameterGraph, { type PlotHandle } from '../../../object-editor/ParameterGraph.svelte';
    import type { EditorDraft } from '../../../object-editor/draft.svelte';
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
    }: {
        draft: EditorDraft;
        kind: SampleEnvelope;
        disabled: boolean;
        blocked: string[];
        onselect: (key: string) => void;
        title?: Snippet;
    } = $props();
    const min = $derived(kind === 'aeg' ? 0 : -127);
    const points = $derived(sampleEnvelope(kind, draft.values));
    const total = $derived(envelopeDurations(kind, draft.values).reduce((sum, duration) => sum + duration, 32));
    const viewport = $derived(envelopeViewport(draft, kind, total));
    const canEdit = (key?: string) =>
        !!key && !disabled && !blocked.includes(key) && Number.isFinite(draft.values[key]);
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
                              readout: `${point.y}${point.rateParameter ? `, ${point.rateParameter.split('.')[1]!.replace('_', ' ')} ${draft.values[point.rateParameter] ?? 'Unavailable'}` : ''}${draft instanceof BankDraft ? `. ${draft.sourceDescription([point.parameter, point.rateParameter].filter((key): key is string => !!key))}` : ''}`,
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
            patch[point.rateParameter!] = envelopeRate(nextX * viewport.span - before);
        }
        draft.patch(patch);
    }
</script>

{#snippet tools()}
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
        onclick={() => viewport.fit(total)}><Icon name="fit-width" size={14} /></button
    >
{/snippet}
<ParameterGraph
    {title}
    {tools}
    label="Envelope levels by stage"
    ticks={[127, Math.round((127 + min) / 2), min]}
    traces={[{ id: 'envelope', points: points.map((point, index) => ({ x: x(index), y: y(point.y) })) }]}
    {handles}
    {disabled}
    axis={[
        { x: 0, label: 'Note on' },
        { x: 1, label: 'Relative time' },
    ]}
    onbegin={begin}
    onend={end}
    onchange={change}
    onselect={(id) => {
        const point = points[Number(id)]!;
        onselect(point.parameter ?? point.rateParameter!);
    }}
    onkey={(id, key, shift) => {
        const handle = handles.find((item) => item.id === id)!;
        const point = points[Number(id)]!;
        const horizontal =
            ['ArrowLeft', 'ArrowRight'].includes(key) || (['Home', 'End'].includes(key) && !handle.vertical);
        if (horizontal ? !handle.horizontal : !handle.vertical) return;
        const parameter = horizontal ? point.rateParameter : point.parameter;
        if (!canEdit(parameter)) return;
        const current = Number(draft.values[parameter!]);
        const low = horizontal ? 0 : min;
        const delta = (['ArrowLeft', 'ArrowUp'].includes(key) ? 1 : -1) * (shift ? 8 : 1);
        draft.set(
            parameter!,
            key === 'Home' ? low : key === 'End' ? 127 : Math.max(low, Math.min(127, current + delta)),
        );
    }}
/>

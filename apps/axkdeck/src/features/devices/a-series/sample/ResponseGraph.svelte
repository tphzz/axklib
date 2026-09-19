<script lang="ts">
    import type { Snippet } from 'svelte';
    import BreakpointGraph from '../../../object-editor/BreakpointGraph.svelte';
    import EnvelopeGraph from './EnvelopeGraph.svelte';
    import type { EditorDraft } from '../../../object-editor/draft.svelte';
    import type { SampleEnvelope } from './envelope';
    import type { GraphPoint } from '../../../object-editor/graphTypes';
    import { noteName } from './geometry';
    let {
        draft,
        page,
        disabled,
        blocked = [],
        onselect = () => {},
        title,
        tools,
    }: {
        draft: EditorDraft;
        page: string;
        disabled: boolean;
        blocked?: string[];
        onselect?: (key: string) => void;
        title?: Snippet;
        tools?: Snippet;
    } = $props();
    const scaling = $derived(page.endsWith('-scaling'));
    const filter = $derived(page === 'filter-scaling');
    const prefix = $derived(filter ? 'filter' : 'level');
    const levelKeys = $derived([1, 2].map((i) => `${filter ? 'filter_scaling_cutoff' : 'level_scaling_level'}${i}`));
    const points = $derived<(GraphPoint & { parameter: string })[]>(
        levelKeys.map((parameter, i) => ({
            x: Number(draft.values[`${prefix}_scaling_break${i + 1}`] ?? 0),
            y: Number(draft.values[parameter] ?? 0),
            label: `Point ${i + 1}`,
            parameter,
            movableX: true,
            disabled:
                [parameter, `${prefix}_scaling_break${i + 1}`].some((key) => blocked.includes(key)) ||
                [parameter, `${prefix}_scaling_break1`, `${prefix}_scaling_break2`].some(
                    (key) => draft.values[key] === undefined,
                ),
        })),
    );
</script>

{#if !scaling}
    <EnvelopeGraph {draft} kind={page as SampleEnvelope} {disabled} {blocked} {onselect} {title} />
{:else}
    <BreakpointGraph
        {title}
        {tools}
        {points}
        keyboard
        extend
        formatX={noteName}
        label="Keyboard scaling"
        minY={filter ? -127 : 0}
        maxY={127}
        {disabled}
        onselect={(index) => {
            const key = points[index]?.parameter;
            if (key) onselect(key);
        }}
        onbegin={() => draft.beginGesture()}
        onend={() => draft.endGesture()}
        onchange={(index, x, y) => {
            const point = points[index];
            if (!point?.parameter || point.disabled || point.fixed || disabled) return;
            const changes = { [point.parameter]: y };
            const prefix = `${filter ? 'filter' : 'level'}_scaling_break`;
            changes[`${prefix}${index + 1}`] =
                index === 0
                    ? Math.min(x, Number(draft.values[`${prefix}2`]))
                    : Math.max(x, Number(draft.values[`${prefix}1`]));
            draft.patch(changes);
        }}
    />
{/if}

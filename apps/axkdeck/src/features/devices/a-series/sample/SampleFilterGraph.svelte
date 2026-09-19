<script lang="ts">
    import FilterGraph from '../../../object-editor/FilterGraph.svelte';
    import type { PlotHandle } from '../../../object-editor/ParameterGraph.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import { sampleFilterStages } from './filterModel';
    let { document, disabled }: { document: ObjectEditorDocument; disabled: boolean } = $props();
    const values = $derived(document.draft.values);
    const type = $derived(Number(values.filter_type));
    const cutoff = $derived(Number(values.filter_cutoff));
    const q = $derived(Number(values.filter_q_width));
    const gain = $derived(Number(values.filter_gain));
    const distance = $derived(Number(values.filter_cutoff_distance));
    const available = $derived(
        [
            'filter_type',
            'filter_cutoff',
            'filter_q_width',
            'filter_gain',
            ...(type >= 10 ? ['filter_cutoff_distance'] : []),
        ].every((key) => Number.isFinite(values[key])),
    );
    const stages = $derived(available ? sampleFilterStages(type, cutoff, q, distance) : []);
    const canEdit = (key: string) =>
        !disabled && type !== 0 && available && !document.detail!.editing!.blockedParameters.includes(key);
    const handles = $derived<PlotHandle[]>(
        type === 0 || !available
            ? []
            : [
                  {
                      id: 'cutoff-q',
                      label: 'Filter cutoff / Q',
                      readout: `${cutoff}, ${q}`,
                      x: cutoff / 127,
                      y: 0.65 + (q / 31) * 0.28,
                      horizontal: canEdit('filter_cutoff'),
                      vertical: canEdit('filter_q_width'),
                      disabled: !canEdit('filter_cutoff') && !canEdit('filter_q_width'),
                  },
                  {
                      id: 'gain',
                      label: 'Filter gain',
                      readout: String(gain),
                      x: 0.08,
                      y: 0.65 + (gain / 31) * 0.2,
                      vertical: true,
                      disabled: !canEdit('filter_gain'),
                  },
                  ...(type >= 10
                      ? [
                            {
                                id: 'distance',
                                label: 'Cutoff distance',
                                readout: String(distance),
                                x: Math.max(0, Math.min(1, (cutoff + distance) / 127)),
                                y: 0.35,
                                horizontal: true,
                                disabled: !canEdit('filter_cutoff_distance'),
                            },
                        ]
                      : []),
              ],
    );
    function patch(next: Record<string, number>) {
        document.draft.patch(Object.fromEntries(Object.entries(next).filter(([key]) => canEdit(key))));
    }
</script>

{#snippet title()}
    <AttributeHelp
        label="Filter"
        description="Schematic filter response in native parameter values, not a measured frequency response. Q controls resonance, except for band-pass and elimination filters where it controls width. Compound types add a second cutoff. Bypass preserves inactive settings. The graph does not add filtering to audition playback."
    />
{/snippet}
{#if available}<FilterGraph
        {title}
        {stages}
        gain={gain / 31}
        {handles}
        {disabled}
        onbegin={() => document.draft.beginGesture()}
        onend={() => document.draft.endGesture()}
        onchange={(id, x, y) => {
            if (id === 'cutoff-q')
                patch({
                    filter_cutoff: Math.round(x * 127),
                    filter_q_width: Math.max(0, Math.min(31, Math.round(((y - 0.65) / 0.28) * 31))),
                });
            else if (id === 'gain')
                patch({ filter_gain: Math.max(-31, Math.min(31, Math.round(((y - 0.65) / 0.2) * 31))) });
            else patch({ filter_cutoff_distance: Math.max(-63, Math.min(63, Math.round(x * 127 - cutoff))) });
        }}
        onkey={(id, key, shift) => {
            const parameter =
                id === 'cutoff-q'
                    ? ['ArrowLeft', 'ArrowRight'].includes(key)
                        ? 'filter_cutoff'
                        : 'filter_q_width'
                    : id === 'gain'
                      ? 'filter_gain'
                      : 'filter_cutoff_distance';
            const min = parameter === 'filter_gain' ? -31 : parameter === 'filter_cutoff_distance' ? -63 : 0;
            const max =
                parameter === 'filter_gain' || parameter === 'filter_q_width'
                    ? 31
                    : parameter === 'filter_cutoff_distance'
                      ? 63
                      : 127;
            const delta = (['ArrowRight', 'ArrowUp'].includes(key) ? 1 : -1) * (shift ? 8 : 1);
            patch({
                [parameter]:
                    key === 'Home'
                        ? min
                        : key === 'End'
                          ? max
                          : Math.max(min, Math.min(max, Number(values[parameter]) + delta)),
            });
        }}
    />{:else}<p class="editor-meta">Filter response is unavailable for these stored parameters.</p>{/if}

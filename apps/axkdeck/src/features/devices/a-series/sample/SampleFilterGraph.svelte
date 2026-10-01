<script lang="ts">
    import { BankDraft } from '../bank/draft.svelte';
    import { blockedGraphParameters } from './formatCapabilities';
    import { onDestroy } from 'svelte';
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
    const graphBlocked = $derived(blockedGraphParameters(document.detail!.editing!));
    const canEdit = (key: string) => !disabled && type !== 0 && available && !graphBlocked.includes(key);
    const origin = (keys: string[]) =>
        document.draft instanceof BankDraft ? `. ${document.draft.sourceDescription(keys)}` : '';
    const handles = $derived<Omit<PlotHandle, 'y'>[]>(
        type === 0 || !available
            ? []
            : [
                  {
                      id: 'cutoff-q',
                      label: 'Filter cutoff / Q',
                      readout: `${cutoff}, Q / Width ${q}${origin(['filter_cutoff', 'filter_q_width'])}`,
                      help: 'Drag horizontally for cutoff. Mouse wheel or Alt-drag adjusts Q / Width. Shift gives finer movement. Left/Right adjusts cutoff; Up/Down adjusts Q / Width.',
                      x: cutoff / 127,
                      horizontal: canEdit('filter_cutoff'),
                      disabled: !canEdit('filter_cutoff') && !canEdit('filter_q_width'),
                  },
                  ...(type >= 10
                      ? [
                            {
                                id: 'distance',
                                label: 'Cutoff distance',
                                readout: `${distance}, Q / Width ${q}${origin(['filter_cutoff_distance', 'filter_q_width'])}`,
                                help: 'Drag horizontally for cutoff distance. Mouse wheel or Alt-drag adjusts Q / Width. Shift gives finer movement. Left/Right adjusts distance; Up/Down adjusts Q / Width.',
                                x: (cutoff + distance) / 127,
                                horizontal: canEdit('filter_cutoff_distance'),
                                disabled: !canEdit('filter_cutoff_distance') && !canEdit('filter_q_width'),
                            },
                        ]
                      : []),
                  {
                      id: 'gain',
                      label: 'Filter gain',
                      readout: `${gain}${origin(['filter_gain'])}`,
                      x: 0.15,
                      trace: 'filter',
                      vertical: true,
                      alternate: false,
                      disabled: !canEdit('filter_gain'),
                      help: 'Drag up or down to adjust gain. Shift-drag is four times finer. Up/Down adjusts gain; Home/End selects its limits.',
                  },
              ],
    );
    function patch(next: Record<string, number>) {
        document.draft.patch(Object.fromEntries(Object.entries(next).filter(([key]) => canEdit(key))));
    }
    let startQ = 0;
    let wheelTimer: ReturnType<typeof setTimeout> | undefined;
    function end() {
        clearTimeout(wheelTimer);
        wheelTimer = undefined;
        document.draft.endGesture();
    }
    function begin() {
        if (wheelTimer !== undefined) end();
        startQ = q;
        document.draft.beginGesture();
    }
    function setQ(value: number) {
        patch({ filter_q_width: Math.max(0, Math.min(31, Math.round(value))) });
    }
    function wheel(id: string, event: WheelEvent) {
        if (id === 'gain') return;
        const delta = event.deltaY || (event.shiftKey ? event.deltaX : 0);
        if (event.ctrlKey || !delta || !canEdit('filter_q_width')) return;
        event.preventDefault();
        event.stopPropagation();
        if (wheelTimer === undefined) document.draft.beginGesture();
        clearTimeout(wheelTimer);
        setQ(q - Math.sign(delta) * (event.shiftKey ? 1 : 3));
        wheelTimer = setTimeout(end, 180);
    }
    onDestroy(end);
</script>

{#snippet title()}
    <AttributeHelp
        label="Filter"
        description="Schematic filter response in native parameter values, not a measured frequency response. Cutoff handles also adjust Q / Width. Hover near the response away from the cutoff handles to adjust gain vertically. Compound types add a second cutoff. Bypass preserves inactive settings. The graph does not add filtering to audition playback."
    />
{/snippet}
{#if available}<FilterGraph
        {title}
        {stages}
        gain={gain / 31}
        {handles}
        {disabled}
        onbegin={begin}
        onend={end}
        onaltdrag={(_id, delta) => setQ(startQ + delta * 31)}
        onwheel={wheel}
        ongainchange={(value) => patch({ filter_gain: Math.max(-31, Math.min(31, Math.round(value * 31))) })}
        onchange={(id, x) => {
            if (id === 'cutoff-q')
                patch({
                    filter_cutoff: Math.round(x * 127),
                });
            else patch({ filter_cutoff_distance: Math.max(-63, Math.min(63, Math.round(x * 127 - cutoff))) });
        }}
        onkey={(id, key, _shift, alt) => {
            if (id === 'gain') {
                if (key === 'Home' || key === 'End' || key === 'ArrowUp' || key === 'ArrowDown')
                    patch({
                        filter_gain:
                            key === 'Home'
                                ? -31
                                : key === 'End'
                                  ? 31
                                  : Math.max(-31, Math.min(31, gain + (key === 'ArrowUp' ? 1 : -1))),
                    });
                return;
            }
            const parameter =
                alt || ['ArrowUp', 'ArrowDown'].includes(key)
                    ? 'filter_q_width'
                    : id === 'cutoff-q'
                      ? 'filter_cutoff'
                      : 'filter_cutoff_distance';
            const min = parameter === 'filter_cutoff_distance' ? -63 : 0;
            const max = parameter === 'filter_q_width' ? 31 : parameter === 'filter_cutoff_distance' ? 63 : 127;
            const delta = ['ArrowRight', 'ArrowUp'].includes(key) ? 1 : -1;
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

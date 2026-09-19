<script lang="ts">
    import LfoGraph from '../../../object-editor/LfoGraph.svelte';
    import type { LfoWave } from '../../../object-editor/lfoShape';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import { sampleView } from './view.svelte';
    let { document }: { document: ObjectEditorDocument } = $props();
    const view = $derived(sampleView(document));
    const destinations = [
        { id: 'pitch', label: 'Pitch', color: '#9ce1ba' },
        { id: 'cutoff', label: 'Cutoff', color: '#f1d49c' },
        { id: 'amp', label: 'Amplitude', color: '#7eafc8' },
    ];
    const wave = $derived(
        (['saw', 'triangle', 'square', 'sample-hold'] as LfoWave[])[Number(document.draft.values['lfo.wave'])] ?? 'saw',
    );
</script>

<div class="editor-toolbar">
    <AttributeHelp
        label="Modulation shape"
        description="Relative modulation, not calibrated time or audio gain. Speed changes cycle spacing; delay shifts onset. Colored traces show the clean oscillator at each destination's depth and inversion. The faint envelope guide shows gradual buildup separately; it is not multiplied into the displayed oscillator. The dashed trace shows full depth. Unsynchronized phase and Sample & Hold patterns are illustrative; Sample & Hold speed belongs to the Program."
    />
    <span class="editor-spacer"></span>
</div>
<LfoGraph
    {wave}
    speed={Number(document.draft.values['lfo.speed'] ?? 1)}
    delay={Number(document.draft.values['lfo.delay_time'] ?? 0)}
    keyOnSync={document.draft.values['lfo.key_on_sync'] === true}
    traces={destinations
        .filter((item) => view.lfoTraces.includes(item.id))
        .map((item) => ({
            ...item,
            depth: Number(document.draft.values[`lfo.${item.id}_mod_depth`] ?? 0) / 127,
            inverted: document.draft.values[`lfo.${item.id}_mod_phase_invert`] === true,
        }))}
    label="LFO modulation"
/>
<div class="destination">
    <div class="editor-choices" role="group" aria-label="Modulation traces">
        {#each destinations as item}<button
                type="button"
                class="editor-choice"
                aria-label={`Show ${item.label} modulation`}
                aria-pressed={view.lfoTraces.includes(item.id)}
                onclick={() =>
                    (view.lfoTraces = view.lfoTraces.includes(item.id)
                        ? view.lfoTraces.filter((id) => id !== item.id)
                        : [...view.lfoTraces, item.id])}
            >
                <span class="swatch" style:background={item.color}></span>{item.label}</button
            >{/each}
    </div>
</div>

<style>
    .destination {
        display: flex;
        justify-content: center;
    }
    .destination :global(.editor-choices) {
        width: 100%;
        max-width: 360px;
    }
    .swatch {
        display: inline-block;
        width: 9px;
        height: 2px;
        margin-right: 5px;
        vertical-align: middle;
    }
</style>

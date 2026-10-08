<script lang="ts">
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import ProgramField from './ProgramField.svelte';
    import { programField } from './fields';
    import ParameterGraph, { type PlotPoint, type PlotHandle } from '../../../object-editor/ParameterGraph.svelte';
    let {
        document,
        step = false,
        disabled = false,
    }: { document: ObjectEditorDocument; step?: boolean; disabled?: boolean } = $props();
    const values = $derived(document.draft.values);
    const count = $derived(Number(values['step_wave.step_count']) || 16);
    const wave = $derived(step ? 6 : Number(values['lfo.wave']));
    const slope = $derived(Number(values['step_wave.slope']));
    let selected = $state('1');
    const handles = $derived<PlotHandle[]>(
        step
            ? Array.from({ length: count }, (_, index) => ({
                  id: String(index + 1),
                  label: `Step ${index + 1}`,
                  readout: String(values[`step_wave.values.${index + 1}`] ?? 'Unavailable'),
                  x: index / count,
                  y: Number(values[`step_wave.values.${index + 1}`] ?? 64) / 127,
                  vertical: true,
                  horizontal: false,
                  disabled: values[`step_wave.values.${index + 1}`] === undefined,
              }))
            : [],
    );
    const points = $derived.by(() => {
        const result: PlotPoint[] = [];
        const phase = Number(values['lfo.initial_phase'] ?? 0) / 4;
        for (let index = 0; index <= 256; index++) {
            const time = index / 256,
                t = (time + phase) % 1;
            let value = 0;
            if (wave === 1) value = Math.sin(t * Math.PI * 2);
            else if (wave === 2) value = 1 - 2 * t;
            else if (wave === 3) value = 1 - 4 * Math.abs(t - 0.5);
            else if (wave === 4) value = t < 0.5 ? 1 : -1;
            else if (wave === 5) value = [0.3, -0.6, 0.8, -0.1, 0.5, -0.9, 0.1, -0.4][Math.min(7, Math.floor(t * 8))]!;
            else if (wave === 6) {
                const pos = time * count,
                    index = Math.min(count - 1, Math.floor(pos));
                const a = Number(values[`step_wave.values.${index + 1}`] ?? 64),
                    b = Number(values[`step_wave.values.${((index + 1) % count) + 1}`] ?? 64);
                const interpolate = slope === 3 || (slope === 1 && b > a) || (slope === 2 && b < a);
                value = ((interpolate ? a + (b - a) * (pos - index) : a) / 127) * 2 - 1;
            }
            result.push({ x: time, y: (value + 1) / 2 });
        }
        return result;
    });
</script>

<div class="wave">
    <ParameterGraph
        label={step ? 'StepWave shape' : 'LFO cycle schematic'}
        ticks={step ? [127, 64, 0] : ['+1', '0', '-1']}
        traces={[{ id: 'wave', points }]}
        {handles}
        {disabled}
        bind:selected
        axis={[
            { x: 0, label: step ? 'Step 1' : 'Cycle start' },
            { x: 1, label: step ? `Step ${count}` : 'Cycle end' },
        ]}
        onbegin={() => document.draft.beginGesture()}
        onend={() => document.draft.endGesture()}
        onchange={(id, _x, y) => document.draft.set(`step_wave.values.${id}`, Math.round(y * 127))}
        onkey={(id, key, shift) => {
            const field = `step_wave.values.${id}`,
                value = Number(values[field]);
            const next =
                key === 'Home'
                    ? 0
                    : key === 'End'
                      ? 127
                      : key === 'ArrowUp'
                        ? value + (shift ? 8 : 1)
                        : key === 'ArrowDown'
                          ? value - (shift ? 8 : 1)
                          : value;
            document.draft.set(field, Math.max(0, Math.min(127, next)));
        }}
    />
</div>
{#if step}
    <div class="steps">
        {#each Array.from({ length: 16 }, (_, i) => i + 1) as index}
            {@const field = programField(`step_wave.values.${index}`, document.programFormat!, values)}
            {#if field}<ProgramField
                    {document}
                    field={{
                        ...field,
                        reason: index > count ? 'Outside the selected step count; saved value is retained' : '',
                    }}
                    {disabled}
                />{/if}
        {/each}
    </div>
{/if}

<style>
    .wave {
        display: flex;
        flex-direction: column;
        height: 154px;
    }
    .steps {
        display: grid;
        grid-template-columns: repeat(auto-fit, minmax(min(100%, 260px), 1fr));
        gap: 6px 18px;
        margin-top: 8px;
    }
</style>

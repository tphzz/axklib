<script lang="ts">
    import { onDestroy } from 'svelte';
    import { blockedGraphParameters } from './formatCapabilities';
    import EqualizerGraph from '../../../object-editor/EqualizerGraph.svelte';
    import AttributeHelp from '../../../../lib/components/AttributeHelp.svelte';
    import Icon from '../../../../lib/components/Icon.svelte';
    import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
    import {
        eqCoefficients,
        eqFrequencyPosition,
        eqFrequencySelection,
        eqFrequencyLabel,
        eqResponse,
        eqEffectiveGain,
        eqParameterCoefficients,
        eqParameterResponse,
    } from './eqModel';
    let { document, disabled }: { document: ObjectEditorDocument; disabled: boolean } = $props();
    const keys = ['sample_eq_type', 'sample_eq_frequency', 'sample_eq_gain_db', 'sample_eq_width_tenths'];
    const values = $derived(document.draft.values);
    const native = $derived(document.detail!.editing!.sampleFormat.format === 'A3000_188');
    const available = $derived(
        keys.every((key) => (native && key === 'sample_eq_type') || Number.isFinite(values[key])),
    );
    const type = $derived(native ? 0 : Number(values.sample_eq_type));
    const frequency = $derived(Number(values.sample_eq_frequency));
    const gain = $derived(Number(values.sample_eq_gain_db));
    const width = $derived(Number(values.sample_eq_width_tenths));
    const dirty = $derived(keys.some((key) => values[key] !== document.draft.baselineValue(key)));
    let showCoefficients = $state(false);
    const parameters = $derived(available ? eqParameterCoefficients(type, frequency, gain, width) : []);
    const calculated = $derived(available ? eqCoefficients(type, frequency, gain, width) : []);
    const stored = $derived(document.detail!.editing!.eqCoefficients);
    const coefficients = $derived(dirty ? calculated : stored);
    const points = $derived(
        available
            ? Array.from({ length: 401 }, (_, i) => ({
                  x: i / 400,
                  y: (eqParameterResponse(parameters, i / 400) + 18) / 36,
              }))
            : [],
    );
    const coefficientPoints = $derived(
        showCoefficients && available && coefficients?.length === 5
            ? Array.from({ length: 401 }, (_, i) => ({ x: i / 400, y: (eqResponse(coefficients, i / 400) + 18) / 36 }))
            : [],
    );
    const blocked = $derived(blockedGraphParameters(document.detail!.editing!));
    let startWidth = 10;
    let wheelTimer: ReturnType<typeof setTimeout> | undefined;
    const canEdit = (key: string) => available && !disabled && !blocked.includes(key);
    function patch(next: Record<string, number>) {
        document.draft.patch(Object.fromEntries(Object.entries(next).filter(([key]) => canEdit(key))));
    }
    function change(id: string, x: number, y: number) {
        if (id === 'frequency-gain')
            patch({
                sample_eq_frequency: eqFrequencySelection(x, frequency),
                sample_eq_gain_db: Math.max(-12, Math.min(12, Math.round(y * 36 - 18))),
            });
    }
    function end() {
        clearTimeout(wheelTimer);
        wheelTimer = undefined;
        document.draft.endGesture();
    }
    function begin() {
        if (wheelTimer !== undefined) end();
        startWidth = width;
        document.draft.beginGesture();
    }
    function setWidth(value: number) {
        if (type === 0) patch({ sample_eq_width_tenths: Math.max(10, Math.min(120, Math.round(value))) });
    }
    function wheel(_id: string, event: WheelEvent) {
        const delta = event.deltaY || (event.shiftKey ? event.deltaX : 0);
        if (event.ctrlKey || !delta || type !== 0 || !canEdit('sample_eq_width_tenths')) return;
        event.preventDefault();
        event.stopPropagation();
        if (wheelTimer === undefined) document.draft.beginGesture();
        clearTimeout(wheelTimer);
        setWidth(width - Math.sign(delta) * (event.shiftKey ? 1 : 5));
        wheelTimer = setTimeout(end, 180);
    }
    onDestroy(end);
</script>

{#snippet title()}
    <AttributeHelp
        label="Sample EQ"
        description="The solid curve shows the parameter response before coefficient rounding. Drag the point for frequency and gain; hold Shift before dragging for fine movement. Wheel over the point, Alt-drag or Alt+Arrow keys edit Peak/Dip width; add Shift for fine width adjustment. The optional dashed curve shows stored Q13 coefficients, or regenerated coefficients after an EQ edit; rounding can strongly distort low-frequency shapes. Neither curve is a measured hardware response or adds EQ to audition playback. Shelves have fixed width; high-shelf effective gain is limited at lower frequency selections."
    />
{/snippet}
{#snippet tools()}
    <button
        type="button"
        class="editor-icon"
        aria-label="Show coefficient response"
        aria-pressed={showCoefficients}
        style:color={showCoefficients ? 'var(--editor-wave)' : undefined}
        style:background={showCoefficients ? 'var(--color-panel-raised)' : undefined}
        title={`Coefficient response: ${dirty ? 'draft Q13' : 'stored Q13'} (dashed). Solid: pre-quantization parameter response. Neither is a hardware measurement.`}
        onclick={() => (showCoefficients = !showCoefficients)}><Icon name="layers" size={14} /></button
    >
{/snippet}
{#if available && points.length}
    <EqualizerGraph
        {points}
        {coefficientPoints}
        {title}
        {tools}
        frequency={eqFrequencyPosition(frequency)}
        {gain}
        readout={`${eqFrequencyLabel(frequency)}, ${gain} dB${type === 0 ? `, width ${width / 10}` : ''}${eqEffectiveGain(type, frequency, gain) !== gain ? ` (effective ${eqEffectiveGain(type, frequency, gain)} dB)` : ''}`}
        {disabled}
        blocked={[
            ...(!canEdit('sample_eq_frequency') ? ['frequency'] : []),
            ...(!canEdit('sample_eq_gain_db') ? ['gain'] : []),
            ...(type !== 0 || !canEdit('sample_eq_width_tenths') ? ['width'] : []),
        ]}
        onbegin={begin}
        onend={end}
        onaltdrag={(_id, delta) => setWidth(startWidth + delta * 220)}
        onwheel={wheel}
        onchange={change}
        onkey={(_id, key, shift, alt) => {
            const horizontal = ['ArrowLeft', 'ArrowRight'].includes(key);
            const parameter = !alt
                ? horizontal
                    ? 'sample_eq_frequency'
                    : 'sample_eq_gain_db'
                : 'sample_eq_width_tenths';
            if (parameter === 'sample_eq_width_tenths' && type !== 0) return;
            const min = parameter === 'sample_eq_frequency' ? 4 : parameter === 'sample_eq_gain_db' ? -12 : 10;
            const max = parameter === 'sample_eq_frequency' ? 58 : parameter === 'sample_eq_gain_db' ? 12 : 120;
            const step = alt ? (shift ? 1 : 5) : shift ? 8 : 1;
            const delta = (['ArrowRight', 'ArrowUp'].includes(key) ? 1 : -1) * step;
            patch({
                [parameter]:
                    key === 'Home'
                        ? min
                        : key === 'End'
                          ? max
                          : Math.max(min, Math.min(max, Number(values[parameter]) + delta)),
            });
        }}
    />
{:else}<p class="editor-meta">EQ response is unavailable for these stored parameters.</p>{/if}

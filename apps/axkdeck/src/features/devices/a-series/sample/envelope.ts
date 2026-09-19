import type { GraphPoint } from '../../../object-editor/graphTypes';
import type { EditorDraft } from '../../../object-editor/draft.svelte';

export type SampleEnvelope = 'aeg' | 'feg' | 'peg';
export interface SampleEnvelopePoint extends GraphPoint {
    parameter?: string;
    rateParameter?: string;
}

// Relative spacing only. The small floor preserves a draggable fastest attack,
// not a claim about its duration in sampler milliseconds.
export const envelopeDuration = (rate: number) => 0.25 + 127 - Math.max(0, Math.min(127, rate));
export const envelopeRate = (duration: number) => Math.max(0, Math.min(127, Math.round(127.25 - duration)));

export function envelopeDurations(kind: SampleEnvelope, values: EditorDraft['values']) {
    return ['attack', 'decay', 'release'].map((stage) => {
        const rate = Number(values[`${kind}.${stage}_rate`] ?? 127);
        // Yamaha owner's manual p137 explicitly defines maximum AEG release as immediate.
        return kind === 'aeg' && stage === 'release' && rate === 127 ? 0 : envelopeDuration(rate);
    });
}

export function sampleEnvelope(kind: SampleEnvelope, values: EditorDraft['values']): SampleEnvelopePoint[] {
    const amplitude = kind === 'aeg';
    const hold = amplitude && values['aeg.attack_mode'] === 1;
    const level = (name: string) => Number(values[`${kind}.${name}_level`] ?? 0);
    const durations = envelopeDurations(kind, values);
    const total = durations.reduce((a, b) => a + b, 0) + 32;
    const attack = (durations[0]! / total) * 127;
    const sustain = ((durations[0]! + durations[1]!) / total) * 127;
    const noteOff = ((total - durations[2]!) / total) * 127;
    const point = (x: number, name: string, label: string): SampleEnvelopePoint => ({
        x,
        y: level(name),
        label,
        parameter: `${kind}.${name}_level`,
        disabled: values[`${kind}.${name}_level`] === undefined,
    });
    return [
        amplitude ? { x: 0, y: hold ? 127 : 0, label: 'Start', fixed: true } : point(0, 'init', 'Initial'),
        {
            ...(amplitude ? { x: attack, y: 127, label: hold ? 'Hold' : 'Peak' } : point(attack, 'attack', 'Attack')),
            rateParameter: `${kind}.attack_rate`,
        },
        { ...point(sustain, 'sustain', 'Sustain'), rateParameter: `${kind}.decay_rate` },
        { x: noteOff, y: level('sustain'), label: 'Note off', fixed: true },
        {
            ...(amplitude ? { x: 127, y: 0, label: 'End' } : point(127, 'release', 'Release')),
            rateParameter: `${kind}.release_rate`,
        },
    ];
}

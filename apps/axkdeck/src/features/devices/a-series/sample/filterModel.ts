import type { FilterStage } from '../../../object-editor/filterResponse';

const families: FilterStage['kind'][][] = [
    [],
    ['lowpass'],
    ['lowpass'],
    ['highpass'],
    ['highpass'],
    ['bandpass'],
    ['notch'],
    ['lowpass'],
    ['peak'],
    ['peak'],
    ['peak', 'peak'],
    ['notch', 'notch'],
    ['lowpass', 'lowpass'],
    ['lowpass', 'peak'],
    ['highpass', 'highpass'],
    ['highpass', 'peak'],
    ['lowpass', 'highpass'],
];
export function sampleFilterStages(type: number, cutoff: number, q: number, distance: number): FilterStage[] {
    return (families[type] ?? []).map((kind, index) => ({
        kind,
        cutoff: (cutoff + (index ? distance : 0)) / 127,
        resonance: q / 31,
        width: kind === 'bandpass' ? 0.015 + (q / 31) * 0.15 : kind === 'notch' ? 0.015 + (1 - q / 31) * 0.15 : 0.055,
        order: [2, 4].includes(type) ? 4 : type === 7 ? 3 : 2,
        mix: type === 9 ? 0.5 : 1,
    }));
}

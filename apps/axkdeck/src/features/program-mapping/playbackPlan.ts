import type { EditorValues } from '../object-editor/draft.svelte';
import type { KeyboardRange } from '../object-editor/keyboardMapping';

export function matchesMapping(range: KeyboardRange & { empty?: boolean }, note: number, velocity: number): boolean {
    return (
        !range.empty &&
        note >= range.low &&
        note <= range.high &&
        velocity >= range.velocityLow &&
        velocity <= range.velocityHigh
    );
}
export function mappingVoiceValues(
    source: EditorValues,
    program?: EditorValues,
    id = 0,
    coarse = { minimum: -64, maximum: 63 },
): EditorValues {
    const values = { ...source };
    if (!program) return values;
    const offset = (name: string) => Number(program[`assignments.${id}.${name}`] ?? 0);
    const clamp = (value: number, low: number, high: number) => Math.max(low, Math.min(high, value));
    values.root_key = Number(values.root_key) + offset('key_shift');
    values.coarse_tune = clamp(
        Number(values.coarse_tune ?? 0) + offset('coarse_tune_offset'),
        coarse.minimum,
        coarse.maximum,
    );
    values.fine_tune_cents = clamp(Number(values.fine_tune_cents ?? 0) + offset('fine_tune_offset'), -63, 63);
    values.level = clamp(Number(values.level ?? 127) + offset('level_offset'), 0, 127);
    values.pan = clamp((values.pan === -64 ? 0 : Number(values.pan ?? 0)) + offset('pan_offset'), -63, 63);
    return values;
}

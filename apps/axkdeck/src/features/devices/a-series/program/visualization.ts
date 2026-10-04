import type { ProgramEditingSnapshot, ProgramEditorTarget } from '../../../../lib/objectEditing';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import type { DraftAssignment } from './draft.svelte';

export const clampMidi = (value: number) => Math.max(0, Math.min(127, value));
export function previewSamples(snapshot: ProgramEditingSnapshot, assignment: DraftAssignment) {
    const target = snapshot.targets.find((item) => item.objectId === assignment.targetObjectId);
    if (!target?.available) return [];
    if (target.kind === 'SBNK') return [target];
    return target.members.flatMap((member) => {
        const sample = snapshot.targets.find((item) => item.objectId === member.objectId);
        if (!sample?.available) return [];
        const values = { ...sample.values };
        for (const key of target.overrideKeys) {
            if (key in target.values) values[key] = target.values[key]!;
            else delete values[key];
        }
        return [{ ...sample, values }];
    });
}
export function programEnvelope(sample: ProgramEditorTarget, values: EditorValues, id: number) {
    const required = ['aeg.attack_rate', 'aeg.decay_rate', 'aeg.release_rate', 'aeg.sustain_level', 'aeg.attack_mode'];
    if (!required.every((key) => Number.isFinite(sample.values[key]))) return null;
    const effective = { ...sample.values };
    const bindings: Record<string, { key: string; base: number }> = {};
    for (const stage of ['attack', 'decay', 'release']) {
        const rate = `aeg.${stage}_rate`,
            key = `assignments.${id}.amp_${stage}_offset`;
        if (!Number.isFinite(values[key])) return null;
        const base = Number(sample.values[rate]);
        effective[rate] = clampMidi(base + Number(values[key]));
        bindings[rate] = { key, base };
    }
    return { effective, bindings };
}
export function programCoverage(sample: ProgramEditorTarget, values: EditorValues, id: number) {
    const prefix = `assignments.${id}.`;
    const required = ['key_low', 'key_high', 'velocity_low', 'velocity_high'];
    if (
        !required.every((key) => Number.isFinite(sample.values[key]) && Number.isFinite(values[prefix + key])) ||
        !Number.isFinite(values[prefix + 'key_shift'])
    )
        return null;
    const shift = Number(values[prefix + 'key_shift']);
    const low = Math.max(Number(values[prefix + 'key_low']), Number(sample.values.key_low) + shift, 0);
    const high = Math.min(Number(values[prefix + 'key_high']), Number(sample.values.key_high) + shift, 127);
    const velocityLow = Math.max(Number(values[prefix + 'velocity_low']), Number(sample.values.velocity_low));
    const velocityHigh = Math.min(Number(values[prefix + 'velocity_high']), Number(sample.values.velocity_high));
    return {
        low,
        high,
        velocityLow,
        velocityHigh,
        root: Number.isFinite(sample.values.root_key) ? Number(sample.values.root_key) + shift : undefined,
        empty: low > high || velocityLow > velocityHigh,
    };
}

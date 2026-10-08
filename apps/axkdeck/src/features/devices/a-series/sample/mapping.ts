import type { EditorValues } from '../../../object-editor/draft.svelte';
import type { KeyboardRange, RangeBoundary } from '../../../object-editor/keyboardMapping';
import { validRange } from '../../../program-mapping/protocol';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';
import { sampleSnapshot } from '../../../../lib/objectEditing';

export function sampleMappingAxes(document: ObjectEditorDocument) {
    const snapshot = sampleSnapshot(document.detail);
    const allowed = (key: string) => key in document.draft.values && !snapshot?.blockedParameters.includes(key);
    return {
        keys: allowed('key_low') && allowed('key_high'),
        velocity: allowed('velocity_low') && allowed('velocity_high'),
    };
}

export const rangeParameters = {
    low: 'key_low',
    high: 'key_high',
    velocityLow: 'velocity_low',
    velocityHigh: 'velocity_high',
} as const;
export function sampleMappingRange(values: EditorValues): KeyboardRange | null {
    const range = {
        low: Number(values.key_low === 255 ? values.root_key : values.key_low),
        high: Number(values.key_high === 128 ? values.root_key : values.key_high),
        velocityLow: Number(values.velocity_low),
        velocityHigh: Number(values.velocity_high),
    };
    return validRange(range) ? range : null;
}
export function sampleMappingPatch(
    range: KeyboardRange,
    boundaries: RangeBoundary[],
    original?: EditorValues,
): EditorValues {
    const baseline = original && sampleMappingRange(original);
    return Object.fromEntries(
        boundaries.map((boundary) => [
            rangeParameters[boundary],
            original && baseline?.[boundary] === range[boundary]
                ? original[rangeParameters[boundary]]!
                : range[boundary],
        ]),
    );
}

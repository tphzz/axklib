import type { ProgramEditingSnapshot } from '../../../../lib/objectEditing';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import type { KeyboardRange, KeyboardZone, RangeBoundary } from '../../../object-editor/keyboardMapping';
import { sampleMappingPatch } from '../sample/mapping';
import type { DraftAssignment } from './draft.svelte';
import { previewSamples, programCoverage } from './visualization';
import type { ObjectEditorDocument } from '../../../object-editor/workflow.svelte';

export function mappingLimits(values: EditorValues, id: number): KeyboardRange {
    const prefix = `assignments.${id}.`;
    return {
        low: Number(values[prefix + 'key_low']),
        high: Number(values[prefix + 'key_high']),
        velocityLow: Number(values[prefix + 'velocity_low']),
        velocityHigh: Number(values[prefix + 'velocity_high']),
    };
}
export function mappingPatch(id: number, range: KeyboardRange, boundaries: RangeBoundary[]) {
    const prefix = `assignments.${id}.`;
    return Object.fromEntries(
        Object.entries(sampleMappingPatch(range, boundaries)).map(([key, value]) => [prefix + key, value]),
    );
}
export function mappingZones(
    snapshot: ProgramEditingSnapshot,
    assignments: DraftAssignment[],
    values: EditorValues,
    selected: number,
    lookup?: (id: string) => ObjectEditorDocument | undefined,
): KeyboardZone[] {
    return assignments.flatMap((row) =>
        previewSamples(snapshot, row, lookup).flatMap((sample, index) => {
            const coverage = programCoverage(sample, values, row.id);
            if (!coverage) return [];
            const shift = Number(values[`assignments.${row.id}.key_shift`]);
            return [
                {
                    id: `${row.id}:${index}`,
                    selectionId: row.id,
                    label: row.kind === 'SBAC' ? `${row.name} / ${sample.name}` : row.name,
                    ...coverage,
                    selected: row.id === selected,
                    source: {
                        low: Math.max(0, Number(sample.values.key_low) + shift),
                        high: Math.min(127, Number(sample.values.key_high) + shift),
                        velocityLow: Number(sample.values.velocity_low),
                        velocityHigh: Number(sample.values.velocity_high),
                    },
                },
            ];
        }),
    );
}

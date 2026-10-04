import { describe, expect, it } from 'vitest';
import type { ObjectDetail } from '../../../../lib/transport';
import type { ProgramEditingSnapshot, ProgramEditorFormat } from '../../../../lib/objectEditing';
import { programEdit, validateProgram } from './adapter';
import { ProgramDraft } from './draft.svelte';

const snapshot: ProgramEditingSnapshot = {
    profile: 'a-series/program',
    editable: true,
    reason: '',
    payloadSha256: 'a'.repeat(64),
    partitionIndex: 0,
    volumeName: 'Programs',
    programNumber: 33,
    programName: 'Test',
    storageRevision: 4,
    model: 'A5000',
    values: { 'assignments.0.level_offset': 0, 'assignments.1.level_offset': 0, 'effects.1.type': 0 },
    assignments: [
        { ordinal: 0, kind: 'SBNK', name: 'Duplicate', targetObjectId: 'sample' },
        { ordinal: 1, kind: 'SBNK', name: 'Duplicate', targetObjectId: 'sample' },
    ],
    targets: [],
};
const detail = { editing: snapshot, image: { revision: 8 } } as ObjectDetail;
const format: ProgramEditorFormat = {
    model: 'A5000',
    fields: [{ key: 'assignments.*.level_offset', min: -127, max: 127 }],
    effects: [
        {
            id: 0,
            label: 'Test',
            printedNumber: 1,
            resetWords: Array(16).fill(0),
            parameters: Array.from({ length: 16 }, (_, index) => ({
                index,
                label: 'Word',
                min: 0,
                max: 100,
                editable: index < 2,
            })),
        },
    ],
};
describe('Program edit contract', () => {
    it('saves membership with stable retained ordinals, new defaults and global parameters in one operation', () => {
        const draft = new ProgramDraft(snapshot.values, snapshot.assignments);
        draft.removeAssignment(0);
        draft.set('assignments.1.level_offset', 30);
        const id = draft.addAssignment(
            { objectId: 'new', kind: 'SBAC', name: 'New' },
            { level_offset: 0, receive: -1 },
        );
        draft.set('level', 70);
        expect(id).toBe(2);
        const result = programEdit(detail, draft.changes, draft.values, format, draft);
        expect(result.operation.type).toBe('replace_program_assignments');
        expect(result.operation.parameters).toEqual({ level: 70 });
        expect(result.operation.assignments).toEqual([
            { retain_ordinal: 1, sample: 'Duplicate', parameters: { level_offset: 30 } },
            { sample_bank: 'New', parameters: { level_offset: 0, receive: 'inherit' } },
        ]);
    });
    it('uses stored row ordinal and payload precondition, not deduplicated targets', () => {
        const changes = { 'assignments.1.level_offset': 12 };
        const result = programEdit(detail, changes, { ...snapshot.values, ...changes }, format);
        expect(result.operation.expected_payload_sha256).toBe(snapshot.payloadSha256);
        expect(result.expectedRevision).toBe(8);
        expect(result.operation.assignments).toEqual([
            {
                ordinal: 1,
                expected_target_kind: 'SBNK',
                expected_target_name: 'Duplicate',
                parameters: { level_offset: 12 },
            },
        ]);
    });
    it('carries reset intent even after returning to the original effect type', () => {
        const values = { ...snapshot.values, 'effects.1.words.0': 50, 'effects.1.words.1': 0, 'effects.1.reset': true };
        const result = programEdit(detail, { 'effects.1.reset': true }, values, format);
        expect(result.operation.parameters).toEqual({
            effects: { '1': { type: 0, reset_parameters: true, parameters: { '1': 50, '2': 0 } } },
        });
    });
    it('rejects unsupported fields, fractional values and inverted touched limits', () => {
        expect(validateProgram(snapshot.values, { bogus: 1 }, snapshot, format)).not.toBe('');
        expect(validateProgram(snapshot.values, { 'assignments.1.level_offset': 1.2 }, snapshot, format)).not.toBe('');
        expect(validateProgram(snapshot.values, { 'assignments.1.level_offset': 5 }, snapshot, format)).toBe('');
        expect(
            validateProgram(snapshot.values, {}, { ...snapshot, editable: false, reason: 'Read-only' }, format),
        ).toBe('Read-only');
    });
});

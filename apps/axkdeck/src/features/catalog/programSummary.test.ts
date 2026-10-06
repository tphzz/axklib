import { describe, expect, it } from 'vitest';
import type { SamplerObject, SamplerRelationship } from '../../lib/transport';
import { programSummaries } from './programSummary';

const objects = [
    ['p', 'PROG'],
    ['b', 'SBAC'],
    ['c', 'SBAC'],
    ['s', 'SBNK'],
    ['t', 'SBNK'],
].map(([key, objectType]) => ({ key, objectType }) as SamplerObject);
function row(source: string, target: string, type: string, extra = {}): SamplerRelationship {
    return {
        sourceObjectId: source,
        targetObjectId: target,
        relationshipType: type,
        quality: 'KNOWN',
        assignmentState: 'stored-assignment',
        ...extra,
    } as SamplerRelationship;
}
describe('Program subtitle context', () => {
    it('counts assignment rows separately from distinct targets and shared bank members', () => {
        const result = programSummaries(objects, [
            row('p', 'b', 'PROG_ASSIGNMENT_TO_SBAC'),
            row('p', 'b', 'PROG_ASSIGNMENT_TO_SBAC'),
            row('p', 'c', 'PROG_ASSIGNMENT_TO_SBAC', { assignmentState: 'source-load-assignment' }),
            row('p', 's', 'PROG_ASSIGNMENT_TO_SBNK'),
            row('b', 's', 'SBAC_SLOT_TO_SBNK'),
            row('b', 't', 'SBAC_SLOT_TO_SBNK'),
            row('c', 's', 'SBAC_SLOT_TO_SBNK'),
        ]);
        expect(result.get('p')).toBe('4 assignments · 2 banks (2 samples) · 1 direct sample');
    });
    it('does not project unknown relationships onto counts', () => {
        const result = programSummaries(objects, [
            row('p', 'b', 'PROG_ASSIGNMENT_TO_SBAC'),
            row('p', 'missing', 'PROG_ASSIGNMENT_TO_SBNK'),
            row('b', 's', 'SBAC_SLOT_TO_SBNK', { quality: 'LIKELY' }),
        ]);
        expect(result.get('p')).toContain('1 assignment · 1 bank (0 samples) · 0 direct samples');
        expect(result.get('p')).toContain('1 unresolved assignment · 1 unresolved bank member');
        expect(programSummaries(objects, []).get('p')).toBe('0 assignments · 0 banks (0 samples) · 0 direct samples');
    });
});

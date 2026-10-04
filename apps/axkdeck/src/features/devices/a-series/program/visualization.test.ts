import { describe, expect, it } from 'vitest';
import { programEditorFixture } from '../../../../test/programEditorFixture';
import { previewSamples, programEnvelope, programCoverage } from './visualization';
import { ProgramDraft } from './draft.svelte';
describe('Program visualization context', () => {
    it('clamps envelope rate offsets and keeps source levels unchanged', () => {
        const { editing, document } = programEditorFixture();
        document.draft.set('assignments.0.amp_attack_offset', 127);
        const curve = programEnvelope(editing.targets[0]!, document.draft.values, 0)!;
        expect(curve.effective['aeg.attack_rate']).toBe(127);
        expect(curve.effective['aeg.sustain_level']).toBe(90);
        expect(curve.bindings['aeg.attack_rate']).toEqual({ key: 'assignments.0.amp_attack_offset', base: 64 });
        expect(editing.targets[0]!.values['aeg.attack_rate']).toBe(64);
    });
    it('shifts source keys before intersecting Program limits, without clamping an out-of-range source to a playable note', () => {
        const { editing, document } = programEditorFixture();
        document.draft.patch({
            'assignments.0.key_shift': 12,
            'assignments.0.key_low': 50,
            'assignments.0.key_high': 70,
        });
        expect(programCoverage(editing.targets[0]!, document.draft.values, 0)).toMatchObject({
            low: 50,
            high: 70,
            root: 72,
            empty: false,
        });
        document.draft.set('assignments.0.key_shift', 127);
        expect(programCoverage(editing.targets[0]!, document.draft.values, 0)?.empty).toBe(true);
    });
    it('uses only active bank overrides and never substitutes a missing member with bank defaults', () => {
        const { editing, document } = programEditorFixture();
        editing.targets.push({
            objectId: 'bank',
            kind: 'SBAC',
            name: 'Bank',
            assignable: true,
            available: true,
            reason: '',
            values: { 'aeg.attack_rate': 20, 'aeg.sustain_level': 0 },
            overrideKeys: ['aeg.attack_rate'],
            members: [
                { name: 'Duplicate', objectId: 'sample' },
                { name: 'Missing', objectId: null },
            ],
        });
        const draft = document.draft as ProgramDraft;
        const id = draft.addAssignment(editing.targets.at(-1)!, {});
        const samples = previewSamples(
            editing,
            draft.assignments.find((row) => row.id === id)!,
        );
        expect(samples).toHaveLength(1);
        expect(samples[0]!.values).toMatchObject({ 'aeg.attack_rate': 20, 'aeg.sustain_level': 90 });
        delete samples[0]!.values['aeg.attack_rate'];
        expect(programEnvelope(samples[0]!, document.draft.values, 0)).toBeNull();
    });
});

import { describe, expect, it } from 'vitest';
import { BankDraft } from './draft.svelte';

const units = [
    { id: 33, keys: ['level'], selectors: [33], activeSelectors: [] },
    { id: 65, keys: ['aeg.attack_rate', 'aeg.decay_rate', 'aeg.release_rate'], selectors: [65], activeSelectors: [] },
];
const stored = { level: 127, 'aeg.attack_rate': 1, 'aeg.decay_rate': 2, 'aeg.release_rate': 3 };

describe('bank override draft', () => {
    it('seeds a complete group from the preview member and undoes activation atomically', () => {
        const draft = new BankDraft(stored, units);
        draft.member = { level: 80, 'aeg.attack_rate': 90, 'aeg.decay_rate': 91, 'aeg.release_rate': 92 };
        expect(draft.values.level).toBe(80);
        expect(draft.dirty).toBe(false);
        draft.beginGesture();
        draft.set('aeg.attack_rate', 100);
        draft.set('aeg.attack_rate', 110);
        draft.endGesture();
        expect(draft.values['aeg.decay_rate']).toBe(91);
        expect(draft.isOverridden('aeg.release_rate')).toBe(true);
        draft.undo();
        expect(draft.dirty).toBe(false);
        expect(draft.values['aeg.attack_rate']).toBe(90);
        draft.redo();
        draft.clearOverride('aeg.attack_rate');
        expect(draft.values['aeg.attack_rate']).toBe(90);
        expect(draft.storedValues['aeg.attack_rate']).toBe(110);
        expect(draft.dirty).toBe(false);
    });
    it('preview switching never changes the editing state or active values', () => {
        const draft = new BankDraft(stored, units);
        draft.member = { level: 30 };
        draft.set('level', 30);
        expect(draft.isOverridden('level')).toBe(true);
        draft.member = { level: 70 };
        expect(draft.values.level).toBe(30);
        draft.clearOverride('level');
        expect(draft.values.level).toBe(70);
        expect(draft.isOverridden('level')).toBe(false);
        expect(draft.changes).toEqual({});
    });
    it('reset restores saved flags without activating an untouched sibling group', () => {
        const draft = new BankDraft(stored, units);
        draft.member = { level: 30 };
        draft.set('level', 90);
        draft.resetUnits(['level', 'aeg.attack_rate']);
        expect(draft.dirty).toBe(false);
        expect(draft.isOverridden('aeg.attack_rate')).toBe(false);
        expect(draft.isOverridden('level')).toBe(false);
        draft.undo();
        expect(draft.values.level).toBe(90);
    });
    it('a native bank override uses Peak/Dip while an inherited later member keeps its shelf type', () => {
        const draft = new BankDraft({ sample_eq_gain_db: 0, sample_eq_frequency: 30, sample_eq_width_tenths: 10 }, [
            {
                id: 49,
                selectors: [49, 50, 51],
                activeSelectors: [],
                keys: ['sample_eq_gain_db', 'sample_eq_frequency', 'sample_eq_width_tenths'],
            },
        ]);
        draft.member = { sample_eq_type: 2, sample_eq_gain_db: 3, sample_eq_frequency: 32, sample_eq_width_tenths: 20 };
        expect(draft.values.sample_eq_type).toBe(2);
        draft.set('sample_eq_gain_db', 4);
        expect(draft.values.sample_eq_type).toBe(0);
        expect(draft.values.sample_eq_frequency).toBe(32);
        draft.clearOverride('sample_eq_gain_db');
        expect(draft.values.sample_eq_type).toBe(2);
    });
});

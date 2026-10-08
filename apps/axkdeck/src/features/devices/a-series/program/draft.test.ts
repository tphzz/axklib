import { describe, expect, it } from 'vitest';
import { ProgramDraft } from './draft.svelte';

describe('ProgramDraft', () => {
    it('retains stable assignment identity through removal, append and undo', () => {
        const draft = new ProgramDraft(
            {
                'assignments.0.level_offset': 11,
                'assignments.1.level_offset': 22,
            },
            [
                { ordinal: 0, kind: 'SBNK', name: 'Same', targetObjectId: 'sample' },
                { ordinal: 1, kind: 'SBNK', name: 'Same', targetObjectId: 'sample' },
            ],
        );
        draft.removeAssignment(0);
        draft.set('assignments.1.level_offset', 33);
        const added = draft.addAssignment(
            { objectId: 'new', kind: 'SBNK', name: 'New' },
            { level_offset: 0, receive: -1 },
        );
        expect(added).toBe(2);
        expect(draft.assignments.map((row) => [row.id, row.retainOrdinal])).toEqual([
            [1, 1],
            [2, undefined],
        ]);
        expect(draft.values['assignments.2.level_offset']).toBe(0);
        expect(draft.membershipChanged).toBe(true);
        draft.undo();
        expect(draft.assignments.map((row) => row.id)).toEqual([1]);
        draft.undo();
        expect(draft.values['assignments.1.level_offset']).toBe(22);
        draft.undo();
        expect(draft.assignments.map((row) => row.id)).toEqual([0, 1]);
        expect(draft.dirty).toBe(false);
        draft.redo();
        expect(draft.assignments.map((row) => row.id)).toEqual([1]);
    });

    it('does not serialize edits on removed rows or treat a cancelled addition as dirty', () => {
        const draft = new ProgramDraft({}, []);
        const added = draft.addAssignment({ objectId: 'new', kind: 'SBNK', name: 'New' }, { level_offset: 0 });
        draft.set(`assignments.${added}.level_offset`, 50);
        draft.removeAssignment(added);
        expect(draft.dirty).toBe(false);
        expect(draft.changes).toEqual({});
        draft.undo();
        expect(draft.values[`assignments.${added}.level_offset`]).toBe(50);
        expect(draft.dirty).toBe(true);
    });
    it('groups gestures and keeps changes sparse across 999 assignments', () => {
        const values = Object.fromEntries(Array.from({ length: 999 }, (_, i) => [`assignments.${i}.level_offset`, 0]));
        const draft = new ProgramDraft(values);
        draft.beginGesture();
        for (let i = 1; i <= 100; i++) draft.set('assignments.998.level_offset', i);
        draft.endGesture();
        expect(draft.changes).toEqual({ 'assignments.998.level_offset': 100 });
        expect(draft.historyLeafCount).toBe(1);
        draft.undo();
        expect(draft.dirty).toBe(false);
        draft.redo();
        expect(draft.values['assignments.998.level_offset']).toBe(100);
    });

    it('retains all words through atomic type resets and undo', () => {
        const values = {
            'effects.1.type': 0,
            ...Object.fromEntries(Array.from({ length: 16 }, (_, i) => [`effects.1.words.${i}`, 100 + i])),
        };
        const draft = new ProgramDraft(values);
        draft.changeEffectType(1, 1, Array(16).fill(0));
        draft.set('effects.1.words.0', 100);
        expect(draft.values['effects.1.words.0']).toBe(100);
        expect(draft.values['effects.1.words.15']).toBe(0);
        draft.changeEffectType(1, 0, Array(16).fill(1));
        expect(draft.dirty).toBe(true);
        draft.undo();
        expect(draft.values['effects.1.type']).toBe(1);
        expect(draft.values['effects.1.words.0']).toBe(100);
        draft.undo();
        draft.undo();
        expect(draft.values).toEqual(values);
        expect(draft.dirty).toBe(false);
    });

    it('preserves redo after a no-op and bounds history to 100 gestures', () => {
        const draft = new ProgramDraft({ level: 0 });
        for (let i = 1; i <= 110; i++) draft.set('level', i);
        expect(draft.historyLeafCount).toBe(100);
        draft.undo();
        draft.set('level', 109);
        expect(draft.canRedo).toBe(true);
        draft.redo();
        expect(draft.values.level).toBe(110);
        draft.accept({ level: 110 });
        expect(draft.canUndo).toBe(false);
        expect(draft.dirty).toBe(false);
    });
});

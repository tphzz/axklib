import { describe, expect, it } from 'vitest';
import { EditorDraft } from './draft.svelte';

describe('EditorDraft', () => {
    it('publishes one atomic patch and preserves redo on a no-op patch', () => {
        const draft = new EditorDraft({ x: 1, y: 2 });
        const set = draft.set.bind(draft);
        draft.set = () => {
            throw new Error('patch must not publish intermediate fields');
        };
        draft.patch({ x: 3, y: 4 });
        expect(draft.values).toEqual({ x: 3, y: 4 });
        draft.undo();
        draft.patch({ x: 1, y: 2 });
        expect(draft.canRedo).toBe(true);
        draft.redo();
        expect(draft.values).toEqual({ x: 3, y: 4 });
        draft.set = set;
    });
    it('starts clean, records partial changes and groups a pointer gesture', () => {
        const draft = new EditorDraft({ level: 100, pan: 0, 'aeg.attack_rate': 127 });
        expect(draft.dirty).toBe(false);
        draft.beginGesture();
        draft.set('level', 90);
        draft.set('level', 80);
        draft.endGesture();
        expect(draft.changes).toEqual({ level: 80 });
        draft.undo();
        expect(draft.dirty).toBe(false);
        draft.redo();
        expect(draft.values.level).toBe(80);
    });
    it('does not make unrelated or unknown fields into an edit', () => {
        const draft = new EditorDraft({ level: 100, pan: -64, reserved: 241 });
        draft.set('level', 90);
        draft.set('level', 100);
        expect(draft.changes).toEqual({});
        expect(draft.values.reserved).toBe(241);
    });
    it('resets history only on discard or confirmed save', () => {
        const draft = new EditorDraft({ level: 100 });
        draft.set('level', 90);
        draft.accept({ level: 90 });
        expect(draft.dirty).toBe(false);
        expect(draft.canUndo).toBe(false);
        draft.set('level', 80);
        draft.discard();
        expect(draft.values.level).toBe(90);
        expect(draft.canUndo).toBe(false);
    });
});

import { afterEach, describe, expect, it, vi } from 'vitest';
import { MappingHeldNote } from './heldNote';
import { programEditorFixture } from '../../test/programEditorFixture';

afterEach(() => vi.useRealTimers());
describe('held mapping note ownership', () => {
    it('never starts a note delivered after its release, and ignores stale releases for a newer note', async () => {
        vi.useFakeTimers();
        const audio = { play: vi.fn().mockResolvedValue(undefined), release: vi.fn() },
            note = new MappingHeldNote(audio, 'program');
        const { document } = programEditorFixture();
        await note.handle({ kind: 'release', clientId: 'child', sequence: 1 }, document, false);
        await note.handle(
            { kind: 'note', clientId: 'child', sequence: 1, note: 60, velocity: 100, imageRevision: 1 },
            document,
            true,
        );
        expect(audio.play).not.toHaveBeenCalled();
        await note.handle(
            { kind: 'note', clientId: 'child', sequence: 2, note: 60, velocity: 100, imageRevision: 1 },
            document,
            true,
        );
        await note.handle({ kind: 'release', clientId: 'child', sequence: 1 }, document, false);
        expect(audio.release).not.toHaveBeenCalled();
        await note.handle({ kind: 'release', clientId: 'child', sequence: 2 }, document, false);
        expect(audio.release).toHaveBeenCalledExactlyOnceWith('child:2');
        note.reset();
    });
    it('expires a held note after window loss and renews only its current lease', async () => {
        vi.useFakeTimers();
        const audio = { play: vi.fn().mockResolvedValue(undefined), release: vi.fn() },
            note = new MappingHeldNote(audio, 'program');
        const { document } = programEditorFixture();
        await note.handle(
            { kind: 'note', clientId: 'child', sequence: 1, note: 60, velocity: 100, imageRevision: 1 },
            document,
            true,
        );
        vi.advanceTimersByTime(4000);
        await note.handle({ kind: 'lease', clientId: 'child', sequence: 1 }, document, false);
        vi.advanceTimersByTime(4000);
        expect(audio.release).not.toHaveBeenCalled();
        vi.advanceTimersByTime(1000);
        expect(audio.release).toHaveBeenCalledExactlyOnceWith('child:1');
        note.reset();
    });
});

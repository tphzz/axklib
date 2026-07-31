import { describe, expect, it } from 'vitest';
import { classifyDroppedNames } from './mediaDropWorkflow.svelte';

describe('media drop classification', () => {
    it.each([
        [['take.wav'], 'audio'],
        [['take.FLAC', 'notes.txt'], 'audio'],
        [['song.mid'], 'midi'],
        [['song.MIDI', 'notes.txt'], 'midi'],
        [['take.aiff', 'song.mid'], 'mixed'],
        [['notes.txt'], 'none'],
        [[], 'none'],
    ] as const)('classifies %j as %s', (names, expected) => {
        expect(classifyDroppedNames(names)).toBe(expected);
    });
});

import { describe, expect, it } from 'vitest';
import { classifyDroppedNames, mediaDropAdmission } from './mediaDropWorkflow.svelte';

describe('media drop admission', () => {
    it('requires an open hard-disk image when no media is open', () => {
        expect(mediaDropAdmission(null, null, false)).toEqual({
            title: 'Open a hard-disk image',
            message: 'Open a writable SFS hard-disk image before importing dropped files.',
        });
    });

    it('rejects writable non-SFS media with a format-specific explanation', () => {
        expect(mediaDropAdmission(12, 'iso9660', true)).toEqual({
            title: 'Drag and drop unavailable',
            message: 'Drag and drop import can only be performed on SFS hard-disk images.',
        });
    });

    it('requires an alterable SFS image', () => {
        expect(mediaDropAdmission(12, 'sfs', false)).toEqual({
            title: 'Image is read-only',
            message: 'Drag and drop import requires a writable SFS hard-disk image.',
        });
        expect(mediaDropAdmission(12, 'sfs', true)).toBeNull();
    });
});

describe('media drop classification', () => {
    it.each([
        [['take.wav'], 'audio'],
        [['take.FLAC', 'notes.txt'], 'audio'],
        [['song.mid'], 'midi'],
        [['song.MIDI', 'notes.txt'], 'midi'],
        [['sounds.ima'], 'tx16w'],
        [['sounds.IMG'], 'tx16w'],
        [['archive.a3k'], 'package'],
        [['volume.axkvol'], 'package'],
        [['program.AXKPRG'], 'package'],
        [['take.aiff', 'song.mid'], 'mixed'],
        [['take.wav', 'sounds.ima'], 'mixed'],
        [['archive.a3k', 'volume.axkvol'], 'package'],
        [['archive.a3k', 'take.wav'], 'mixed'],
        [['notes.txt'], 'none'],
        [[], 'none'],
    ] as const)('classifies %j as %s', (names, expected) => {
        expect(classifyDroppedNames(names)).toBe(expected);
    });
});

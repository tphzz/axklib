import { describe, expect, it } from 'vitest';
import { floppyVolumeName } from './floppySelection';

describe('floppy volume name suggestion', () => {
    it.each([
        ['', 'Yamaha/floppy/norddrms', 'directory', 'norddrms'],
        ['   ', '/Drum Kits/norddrms/disk1/', 'directory', 'disk1'],
        ['', ['C:', 'Drum Kits', 'norddrms', ''].join('\\'), 'directory', 'norddrms'],
        ['', '/folders/archive.img', 'directory', 'archive.img'],
        ['', '/images/norddrms.IMG', 'file', 'norddrms'],
        ['', ['C:', 'images', 'norddrms.IMA'].join('\\'), 'file', 'norddrms'],
        ['', '/images/long floppy filename.img', 'file', 'long floppy file'],
        [' Stored name ', '/images/other.img', 'file', 'Stored name'],
        [' Stored.img ', '/folders/other', 'directory', 'Stored.img'],
        ['', '/images/\u0001.img', 'file', 'Imported'],
        ['', '', 'directory', 'Imported'],
    ] as const)('uses label %j or %s (%s)', (label, path, kind, expected) => {
        expect(floppyVolumeName(label, path, kind)).toBe(expected);
    });
});

import { describe, expect, it } from 'vitest';
import { writableFilesRoot } from '../../lib/testing/filesystem';
import { filesystemNameError, normalizeFilesystemName, validFilesystemName } from './nameValidation';

export const fatNames = {
    ...writableFilesRoot,
    namePolicy: 'FAT_8_3_UPPERCASE' as const,
    maximumNameBytes: 12,
    namePattern: "^[A-Z0-9!#$%&'()@^_`{}~-]{1,8}(\\.[A-Z0-9!#$%&'()@^_`{}~-]{1,3})?$",
};

describe('filesystem naming policy', () => {
    it('normalizes only ASCII letters and preserves SFS names', () => {
        expect(normalizeFilesystemName('aZ-ß.é', fatNames)).toBe('AZ-ß.é');
        expect(normalizeFilesystemName('Mixed Case', writableFilesRoot)).toBe('Mixed Case');
        expect(validFilesystemName('mixed.wav', fatNames)).toBe(true);
        expect(validFilesystemName('12345678.abc', fatNames)).toBe(true);
    });
    it.each([
        ['', 'Enter a name'],
        ['.', 'path separators'],
        ['a/b', 'path separators'],
        ['123456789.wav', 'at most 8'],
        ['a.flac', 'at most 3'],
        ['a.', 'one dot'],
        ['a.b.c', 'one dot'],
        ['.wav', '1-8'],
        ['a b.wav', 'Spaces'],
        ['ß.wav', 'unsupported'],
    ])('explains rejected FAT name %j', (name, reason) => {
        expect(filesystemNameError(name, fatNames)).toContain(reason);
    });
});

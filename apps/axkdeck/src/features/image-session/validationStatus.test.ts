import { describe, expect, it } from 'vitest';

import { validationStatus } from './validationStatus';

describe('validationStatus', () => {
    it.each([
        [{ valid: true, infoCount: 0, warningCount: 0, errorCount: 0 }, 'Ready'],
        [{ valid: true, infoCount: 0, warningCount: 1, errorCount: 0 }, '1 validation warning'],
        [{ valid: true, infoCount: 0, warningCount: 3, errorCount: 0 }, '3 validation warnings'],
        [{ valid: false, infoCount: 0, warningCount: 0, errorCount: 1 }, '1 validation error'],
        [{ valid: false, infoCount: 0, warningCount: 0, errorCount: 2 }, '2 validation errors'],
        [{ valid: false, infoCount: 0, warningCount: 4, errorCount: 2 }, '2 validation errors · 4 warnings'],
    ])('formats %o as %s', (summary, expected) => {
        expect(validationStatus(summary)).toBe(expected);
    });
});

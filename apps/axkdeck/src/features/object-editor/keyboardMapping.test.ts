import { describe, expect, it } from 'vitest';
import { editKeyboardRange } from './keyboardMapping';
const range = { low: 36, high: 84, velocityLow: 10, velocityHigh: 110 };
describe('keyboard mapping limits', () => {
    it('moves without shrinking, inverting or exceeding MIDI boundaries', () => {
        expect(editKeyboardRange(range, 'move', 100, -100)).toEqual({
            low: 79,
            high: 127,
            velocityLow: 0,
            velocityHigh: 100,
        });
        expect(editKeyboardRange(range, 'move', -100, 100)).toEqual({
            low: 0,
            high: 48,
            velocityLow: 27,
            velocityHigh: 127,
        });
    });
    it('resizes inclusive limits with a one-note/one-velocity minimum', () => {
        expect(editKeyboardRange(range, 'low', 100, 0).low).toBe(84);
        expect(editKeyboardRange(range, 'high', -100, 0).high).toBe(36);
        expect(editKeyboardRange(range, 'velocityLow', 0, 1000).velocityLow).toBe(110);
        expect(editKeyboardRange(range, 'velocityHigh', 0, -1000).velocityHigh).toBe(10);
        expect(range.low).toBe(36);
    });
});

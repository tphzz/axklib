import { describe, expect, it } from 'vitest';
import { mappingDragPosition, mappingDragText, type MappingFeedback } from './mappingFeedback';

const feedback: MappingFeedback = {
    range: { low: 0, high: 127, velocityLow: 0, velocityHigh: 127 },
    handle: 'move',
    axes: { keys: true, velocity: true },
    identity: 'one',
    pointer: true,
};
describe('mapping gesture readouts', () => {
    it('reports both editable axes for block movement and only the active handle axis otherwise', () => {
        expect(mappingDragText(feedback, String)).toBe('Keys: 0 (0) - 127 (127) / Velocity: 0 - 127');
        expect(mappingDragText({ ...feedback, axes: { keys: false, velocity: true } }, String)).toBe(
            'Velocity: 0 - 127',
        );
        expect(mappingDragText({ ...feedback, handle: 'low' }, String)).toBe('Keys: 0 (0) - 127 (127)');
    });
    it.each(['low', 'high', 'velocityLow', 'velocityHigh', 'move'] as const)(
        'keeps %s readouts inside the plot',
        (handle) => {
            const position = mappingDragPosition({ ...feedback, handle }, 0, 128, 500, 320, 180, 22);
            expect(position.left).toBeGreaterThanOrEqual(4);
            expect(position.left + 180).toBeLessThanOrEqual(496);
            expect(position.top).toBeGreaterThanOrEqual(4);
            expect(position.top + 22).toBeLessThanOrEqual(316);
        },
    );
});

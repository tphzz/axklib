import { describe, expect, it } from 'vitest';
import { sampleFormatFixture } from '../../../../test/sampleFormatFixture';
import { validateSample } from './adapter';
import { parameterInactiveReason } from './parameterAvailability';
import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';

describe('Sample capability explanations', () => {
    it.each(['', 'The stereo channels store different values.'])(
        'never unlocks a blocked field when its reason is %j',
        (reason) => {
            const snapshot = {
                ...sampleFormatFixture(),
                editable: true,
                parameters: { expand_detune: 0 },
                blockedParameters: ['expand_detune'],
                blockedParameterReasons: reason ? { expand_detune: reason } : {},
                playbackWindow: { start_frame: 0, length_frames: 100 },
            } as unknown as SampleEditingSnapshot;
            expect(validateSample({ expand_detune: 1 }, { expand_detune: 1 }, snapshot)).toBe(
                reason || 'Detune is read-only for this Sample',
            );
        },
    );

    it.each([
        [0, 'portamento_rate', 'off'],
        [1, 'portamento_time', 'Program'],
        [2, 'portamento_time', 'Rate mode'],
        [3, 'portamento_time', 'Rate mode'],
        [4, 'portamento_rate', 'Time mode'],
        [5, 'portamento_rate', 'Time mode'],
    ])('explains inactive %s / %s', (type, key, message) => {
        expect(parameterInactiveReason(String(key), { portamento_type: Number(type) })).toContain(message);
    });

    it.each([
        [2, 'portamento_rate'],
        [3, 'portamento_rate'],
        [4, 'portamento_time'],
        [5, 'portamento_time'],
    ])('keeps active %s / %s editable', (type, key) => {
        expect(parameterInactiveReason(String(key), { portamento_type: Number(type) })).toBe('');
    });
});

import { describe, expect, it } from 'vitest';
import { draftPlaybackPlan } from './audition';
describe('draft audition timing', () => {
    it('uses absolute source frames and transposes loop coordinates with pitch', () => {
        const plan = draftPlaybackPlan(
            {
                'playback.start_frame': 100,
                'playback.length_frames': 400,
                loop_start_frame: 200,
                loop_length_frames: 100,
                loop_mode: 1,
                root_key: 60,
                level: 127,
                pan: 63,
            },
            22050,
            44100,
            72,
        );
        expect(plan).toMatchObject({
            speed: 2,
            start: 100,
            length: 400,
            frames: 400,
            loopStart: 100,
            loopLength: 100,
            gain: 1,
            pan: 1,
            reverse: false,
        });
    });
    it('respects fixed pitch and reverse one-shot without inferring a repeating reverse loop', () => {
        const plan = draftPlaybackPlan(
            { 'playback.start_frame': 0, 'playback.length_frames': 100, loop_mode: 5, fixed_pitch: true, pan: -64 },
            44100,
            44100,
            72,
        );
        expect(plan.speed).toBe(1);
        expect(plan.reverse).toBe(true);
        expect(plan.loopMode).toBe(5);
        expect(plan.pan).toBe(0);
    });
});

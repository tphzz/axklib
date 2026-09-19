import { describe, expect, it } from 'vitest';
import { markerBounds, markerValues, moveMarker, nearestCrossing, calculateTempo, sourceFrame } from './geometry';
import { sampleFields, sampleTabs } from './fields';
import { pageGroups } from './pageGroups';

const values = {
    'playback.start_frame': 10,
    'playback.length_frames': 90,
    loop_start_frame: 30,
    loop_length_frames: 40,
    loop_mode: 1,
};
describe('Sample editor geometry', () => {
    it('converts exclusive end boundaries without changing the stored window', () => {
        expect(markerValues(values)).toEqual([10, 100, 30, 70]);
        expect(moveMarker(values, 0, 20, 120)).toEqual({ 'playback.start_frame': 20, 'playback.length_frames': 80 });
        expect(moveMarker(values, 2, 40, 120)).toEqual({ loop_start_frame: 40, loop_length_frames: 30 });
    });
    it('uses actual PCM capacity and contained loop limits', () => {
        expect(markerBounds(values, 120)).toEqual([
            [0, 30],
            [70, 120],
            [10, 69],
            [31, 100],
        ]);
        expect(moveMarker(values, 1, 200, 120)).toEqual({ 'playback.length_frames': 110 });
        expect(moveMarker(values, 0, 99, 120)['playback.start_frame']).toBe(30);
    });
    it('allows a nonrepeating empty-loop sentinel without constraining playback to zero', () => {
        const empty = { ...values, loop_start_frame: 0, loop_length_frames: 0, loop_mode: 0 };
        expect(markerBounds(empty, 120).slice(0, 2)).toEqual([
            [0, 99],
            [11, 120],
        ]);
        expect(moveMarker(empty, 2, 40, 120)).toEqual({ loop_start_frame: 40, loop_length_frames: 60 });
    });
    it('snaps to source-rate crossings only inside the allowed window', () => {
        const pcm = new Float32Array([1, 1, -1, -1, 1, 1, -1, -1]);
        expect(nearestCrossing(pcm, 3, 0, 8)).toBe(2);
        expect(nearestCrossing(pcm, 3, 3, 7)).toBe(4);
        expect(nearestCrossing(pcm, 8, 8, 8)).toBe(8);
    });
    it('calculates native BPM hundredths and rejects out-of-range calculations', () => {
        expect(calculateTempo(44100, 44100, 2)).toBe(12000);
        expect(calculateTempo(44100, 44100, 8)).toBeNull();
        expect(calculateTempo(44100, 0, 2)).toBeNull();
    });
    it('maps rendered draft playheads back to absolute source frames, including reverse', () => {
        expect(sourceFrame(24000, 48000, 44100, 2, 100, 50000, false)).toBe(44200);
        expect(sourceFrame(0, 48000, 44100, 1, 100, 50000, true)).toBe(50099);
    });
});
describe('Sample page contracts', () => {
    it('groups every parameter once, without losing fields between subpages', () => {
        for (const page of sampleTabs.flatMap((tab) => tab.pages)) {
            const fields = pageGroups(page).flatMap((group) => group.fields.map((field) => field.key));
            expect(fields.toSorted()).toEqual(page.fields.map((field) => field.key).toSorted());
        }
    });
    it('restores Sample settings without duplicate editable fields', () => {
        expect(sampleTabs[0]!.pages.map((p) => p.id)).toEqual(['waveform', 'sample-settings']);
        expect(new Set(sampleFields.map((f) => f.key)).size).toBe(sampleFields.length);
    });
    it('uses stored MIDI controller function IDs', () => {
        const options = sampleFields.find((field) => field.key === 'controls.1.function')!.options!;
        expect(options.find((o) => o.value === 4)?.label).toBe('Cutoff Bias');
        expect(options.find((o) => o.value === 18)?.label).toBe('Start Address');
        expect(options.find((o) => o.value === 22)?.label).toBe('Portamento Rate/Time');
    });
});

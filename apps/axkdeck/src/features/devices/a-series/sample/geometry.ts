import type { EditorValues } from '../../../object-editor/draft.svelte';

export const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
export const noteName = (value: number) =>
    `${['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'][value % 12]}${Math.floor(value / 12) - 2}`;

export function markerValues(values: EditorValues): number[] {
    return [
        Number(values['playback.start_frame']),
        Number(values['playback.start_frame']) + Number(values['playback.length_frames']),
        Number(values.loop_start_frame),
        Number(values.loop_start_frame) + Number(values.loop_length_frames),
    ];
}

export function markerBounds(values: EditorValues, capacity: number): [number, number][] {
    const [start, end, loopStart, loopEnd] = markerValues(values) as [number, number, number, number];
    const loop = Number(values.loop_length_frames) > 0;
    return [
        [0, loop ? Math.min(loopStart, end - 1) : end - 1],
        [loop ? Math.max(loopEnd, start + 1) : start + 1, capacity],
        [start, loop ? loopEnd - 1 : end - 1],
        [loop ? Math.max(start, loopStart) + 1 : start + 1, end],
    ];
}

export function moveMarker(values: EditorValues, index: number, frame: number, capacity: number): EditorValues {
    const bounds = markerBounds(values, capacity)[index]!;
    const position = Math.round(clamp(frame, bounds[0], bounds[1]));
    const [start, end, loopStart, loopEnd] = markerValues(values) as [number, number, number, number];
    if (index === 0) return { 'playback.start_frame': position, 'playback.length_frames': end - position };
    if (index === 1) return { 'playback.length_frames': position - start };
    const empty = Number(values.loop_length_frames) === 0;
    if (index === 2) return { loop_start_frame: position, loop_length_frames: (empty ? end : loopEnd) - position };
    return { loop_start_frame: empty ? start : loopStart, loop_length_frames: position - (empty ? start : loopStart) };
}

export function nearestCrossing(pcm: Float32Array, frame: number, min: number, max: number): number {
    const center = Math.round(clamp(frame, min, max));
    for (let distance = 0; distance <= 128; distance++) {
        for (const index of distance ? [center - distance, center + distance] : [center]) {
            if (index < Math.max(1, min) || index > max || index >= pcm.length) continue;
            const before = pcm[index - 1]!;
            const after = pcm[index]!;
            if (after === 0 || (before < 0 && after > 0) || (before > 0 && after < 0)) return index;
        }
    }
    return center;
}

export function calculateTempo(sampleRate: number, frames: number, beats: number): number | null {
    const value = Math.round((60 * sampleRate * beats * 100) / frames);
    return Number.isFinite(value) && value >= 8000 && value <= 15999 ? value : null;
}

export function sourceFrame(
    renderedFrame: number,
    outputRate: number,
    sourceRate: number,
    speed: number,
    start: number,
    length: number,
    reverse: boolean,
): number {
    const offset = Math.floor((renderedFrame * sourceRate * speed) / outputRate);
    return clamp(start + (reverse ? length - 1 - offset : offset), start, start + length - 1);
}

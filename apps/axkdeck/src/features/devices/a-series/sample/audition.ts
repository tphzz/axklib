import type { ImageTransport } from '../../../../lib/transport';
import type { CachedAudition } from '../../../../lib/audio/auditionTypes';
import type { EditorValues } from '../../../object-editor/draft.svelte';
import { validateSample } from './adapter';
import type { SampleEditingSnapshot } from '../../../../lib/objectEditing';
import { loadEditorAudio, type EditorAudioSource } from '../../../object-editor/audioSource';

export function draftPlaybackPlan(values: EditorValues, rate: number, outputRate: number, note: number) {
    const n = (key: string, fallback: number) => Number(values[key] ?? fallback);
    const pitch =
        n('coarse_tune', 0) + n('fine_tune_cents', 0) / 100 + (values.fixed_pitch ? 0 : note - n('root_key', 60));
    const speed = 2 ** (pitch / 12);
    const scale = outputRate / rate / speed;
    const start = n('playback.start_frame', 0);
    const length = n('playback.length_frames', 0);
    const pan = n('pan', 0);
    return {
        speed,
        start,
        length,
        frames: Math.ceil(length * scale),
        gain: n('level', 127) / 127,
        pan: pan === -64 ? 0 : pan / 63,
        reverse: [3, 5].includes(n('loop_mode', 4)),
        loopStart: Math.round((n('loop_start_frame', start) - start) * scale),
        loopLength: Math.round(n('loop_length_frames', 0) * scale),
        loopMode: n('loop_mode', 4),
    };
}

export async function prepareSampleDraft(
    transport: ImageTransport,
    sessionId: number,
    objectId: string,
    snapshot: SampleEditingSnapshot,
    values: EditorValues,
    note: number,
    context: AudioContext,
    signal: AbortSignal,
    options: { source?: EditorAudioSource; output?: AudioNode } = {},
): Promise<CachedAudition> {
    const geometry = Object.fromEntries(
        ['loop_mode', 'loop_start_frame', 'loop_length_frames'].map((key) => [key, values[key]!]),
    );
    const error = validateSample(values, geometry, snapshot);
    if (error) throw new Error(error);
    if (!Number.isInteger(note) || note < 0 || note > 127) throw new Error('Preview note must be between 0 and 127');
    const audio = options.source ?? (await loadEditorAudio(transport, sessionId, objectId, context, signal));
    {
        const rate = audio.sampleRate;
        const plan = draftPlaybackPlan(values, rate, context.sampleRate, note);
        const fullFrames = Math.max(...audio.lanes.map((lane) => lane.length));
        const workingBytes = fullFrames * 16 + plan.frames * 16;
        if (!Number.isSafeInteger(plan.frames) || plan.frames < 1 || workingBytes > 128 * 1024 * 1024)
            throw new Error('Draft audio exceeds the 128 MiB preview working limit');
        if (plan.start < 0 || plan.length < 1 || audio.lanes.some((lane) => plan.start + plan.length > lane.length))
            throw new Error('Draft playback exceeds the stored Wave Data');
        signal.throwIfAborted();
        const input = context.createBuffer(audio.lanes.length, fullFrames, rate);
        audio.lanes.forEach((buffer, index) => input.copyToChannel(buffer.getChannelData(0), index));
        const renderer = new OfflineAudioContext(2, plan.frames, context.sampleRate);
        const source = renderer.createBufferSource();
        source.buffer = input;
        source.playbackRate.value = plan.speed;
        const gain = renderer.createGain();
        gain.gain.value = plan.gain;
        const pan = renderer.createStereoPanner();
        pan.pan.value = plan.pan;
        source.connect(gain).connect(pan).connect(renderer.destination);
        source.start(0, plan.start / rate, plan.length / rate);
        const buffer = await renderer.startRendering();
        signal.throwIfAborted();
        if (plan.reverse)
            for (let channel = 0; channel < buffer.numberOfChannels; channel++)
                buffer.getChannelData(channel).reverse();
        return {
            key: `draft:${sessionId}:${objectId}`,
            sessionId,
            objectId,
            buffer,
            transient: true,
            output: options.output,
            weightBytes: buffer.length * buffer.numberOfChannels * 4,
            descriptor: {
                objectId,
                sampleRate: context.sampleRate,
                channels: 2,
                sampleWidthBytes: 2,
                frameCount: buffer.length,
                wavSizeBytes: 0,
                loopMode: plan.loopMode,
                loopModeLabel: String(plan.loopMode),
                loopStartFrame: plan.loopStart,
                loopLengthFrames: plan.loopLength,
                warnings: [],
            },
        };
    }
}

import type { ImageTransport } from '../../lib/transport';
import { decodeRiffWavePcm } from '../../lib/audio/riffWavePcm';

export interface EditorAudioSource {
    lanes: AudioBuffer[];
    sampleRate: number;
    frames: number;
}

export async function loadEditorAudio(
    transport: ImageTransport,
    sessionId: number,
    objectId: string,
    context: BaseAudioContext,
    signal: AbortSignal,
): Promise<EditorAudioSource> {
    const bundle = await transport.prepareAuditionBundle(sessionId, [objectId], signal, true);
    try {
        const clip = bundle.clips[0];
        if (!clip || clip.lanes.length < 1 || clip.lanes.length > 2)
            throw new Error('Preview requires mono or stereo Wave Data');
        const sampleRate = clip.lanes[0]!.sampleRate;
        if (clip.lanes.some((lane) => lane.sampleRate !== sampleRate))
            throw new Error('Preview requires matching sample rates');
        const workingBytes =
            bundle.contentSizeBytes * 2 + clip.lanes.reduce((sum, lane) => sum + lane.frameCount * 4, 0);
        if (workingBytes > 128 * 1024 * 1024) throw new Error('Source audio exceeds the 128 MiB preview working limit');
        const content = await transport.readAuditionContent(bundle.auditionId, bundle.contentSizeBytes, signal);
        signal.throwIfAborted();
        const lanes = clip.lanes.map((lane) => {
            const decoded = decodeRiffWavePcm(
                context,
                content.slice(lane.contentOffsetBytes, lane.contentOffsetBytes + lane.wavSizeBytes),
            );
            if (
                !decoded ||
                decoded.sampleRate !== sampleRate ||
                decoded.length !== lane.frameCount ||
                decoded.numberOfChannels !== 1
            )
                throw new Error('Source PCM does not match the preview descriptor');
            return decoded;
        });
        signal.throwIfAborted();
        return { lanes, sampleRate, frames: Math.min(...lanes.map((lane) => lane.length)) };
    } finally {
        await transport.deleteAudition(bundle.auditionId).catch(() => undefined);
    }
}

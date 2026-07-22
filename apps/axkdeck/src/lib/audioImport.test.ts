import { describe, expect, it } from 'vitest';
import { defaultAudioImportNames, isSupportedAudioFile, samplerName, validSamplerName } from './audioImport';
import type { AudioSourceInfo } from './transport';

const inspection = (channels: 1 | 2): AudioSourceInfo => ({
    sourceFormat: 'WAV',
    sourceSubtype: 'PCM_16',
    channels,
    frameCount: 100,
    sourceSampleRate: 48_000,
    outputSampleRate: 44_100,
    sourceSampleWidthBits: 16,
    outputSampleWidthBits: 16,
    durationSeconds: 0.1,
    resampled: true,
    quantized: false,
    sampleWidthConverted: false,
    ditherAlgorithm: '',
    projectedOutputFrameCount: 92,
    projectedOutputBytesPerChannel: 184,
    projectedOutputBytesTotal: 184 * channels,
    maximumOutputFrameCountPerChannel: 1 << 24,
    maximumOutputBytesPerChannel: 32 * 1024 * 1024,
    valid: true,
    issues: [],
});

describe('audio import naming', () => {
    it('accepts only admitted audio extensions without case sensitivity', () => {
        expect(isSupportedAudioFile({ name: 'take.WAV' })).toBe(true);
        expect(isSupportedAudioFile({ name: 'take.aiff' })).toBe(true);
        expect(isSupportedAudioFile({ name: 'take.mp3' })).toBe(false);
    });

    it('creates bounded printable unique mono and stereo object names', () => {
        const samples = new Set<string>(['long recording n']);
        const waveforms = new Set<string>();
        const mono = defaultAudioImportNames('long recording name.wav', inspection(1), samples, waveforms);
        const stereo = defaultAudioImportNames('stéréo: take.flac', inspection(2), samples, waveforms);
        expect(mono.sampleName).toBe('long recording 2');
        expect(stereo.waveformNames).toEqual(['stereo take-L', 'stereo take-R']);
        expect(stereo.waveformNames.every(validSamplerName)).toBe(true);
    });

    it('removes filesystem punctuation and rejects names requiring normalization', () => {
        expect(samplerName(' Kick:/One ')).toBe('Kick One');
        expect(validSamplerName('Kick:/One')).toBe(false);
    });
});

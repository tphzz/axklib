import type { AudioSourceInfo } from './transport';

export const audioExtensions = ['wav', 'wave', 'flac', 'aif', 'aiff'] as const;
export const maximumSamplerNameBytes = 16;
export const defaultRootKey = 60;

export interface AudioImportNames {
    sampleName: string;
    waveformNames: string[];
}

export function isSupportedAudioFile(file: Pick<File, 'name'>): boolean {
    const extension = file.name.split('.').pop()?.toLocaleLowerCase() ?? '';
    return audioExtensions.includes(extension as (typeof audioExtensions)[number]);
}

export function samplerName(value: string, fallback = 'New Sample'): string {
    const ascii = value
        .normalize('NFKD')
        .replace(/[^\x20-\x7e]/g, '')
        .replace(/[\\/:*?"<>|]/g, ' ')
        .replace(/\s+/g, ' ')
        .trim();
    return (ascii || fallback).slice(0, maximumSamplerNameBytes).trimEnd();
}

function uniqueName(candidate: string, used: Set<string>): string {
    const normalized = samplerName(candidate);
    if (!used.has(normalized.toLocaleLowerCase())) {
        used.add(normalized.toLocaleLowerCase());
        return normalized;
    }
    for (let suffix = 2; suffix < 10_000; suffix += 1) {
        const text = ` ${suffix}`;
        const next = `${normalized.slice(0, maximumSamplerNameBytes - text.length).trimEnd()}${text}`;
        if (!used.has(next.toLocaleLowerCase())) {
            used.add(next.toLocaleLowerCase());
            return next;
        }
    }
    throw new Error('Could not allocate a unique sampler name');
}

export function defaultAudioImportNames(
    filename: string,
    inspection: AudioSourceInfo,
    usedSamples: Set<string>,
    usedWaveforms: Set<string>,
): AudioImportNames {
    const extension = filename.lastIndexOf('.');
    const stem = samplerName(extension > 0 ? filename.slice(0, extension) : filename);
    const sampleName = uniqueName(stem, usedSamples);
    if (inspection.channels === 1) return { sampleName, waveformNames: [uniqueName(stem, usedWaveforms)] };
    const base = stem.slice(0, maximumSamplerNameBytes - 2).trimEnd();
    return {
        sampleName,
        waveformNames: [uniqueName(`${base}-L`, usedWaveforms), uniqueName(`${base}-R`, usedWaveforms)],
    };
}

export function validSamplerName(value: string): boolean {
    return value.length > 0 && value === samplerName(value, '') && new TextEncoder().encode(value).length <= 16;
}

export function noteName(key: number): string {
    const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
    return `${names[key % 12]}${Math.floor(key / 12) - 2}`;
}

import { validSamplerName } from './audioImport';

export const midiExtensions = ['mid', 'midi'] as const;

export function midiMediaType(filename: string): string | null {
    const extension = filename.split('.').at(-1)?.toLocaleLowerCase();
    return extension && midiExtensions.includes(extension as (typeof midiExtensions)[number]) ? 'audio/midi' : null;
}

export function defaultSequenceName(filename: string, usedNames: Set<string>): string {
    const stem = filename.replace(/\.(?:mid|midi)$/i, '').trim() || 'Sequence';
    for (let suffix = 1; suffix < 10_000; suffix += 1) {
        const addition = suffix === 1 ? '' : ` ${suffix}`;
        const candidate = `${stem.slice(0, 16 - addition.length)}${addition}`;
        const key = candidate.toLocaleLowerCase();
        if (validSamplerName(candidate) && !usedNames.has(key)) {
            usedNames.add(key);
            return candidate;
        }
    }
    throw new Error('Could not assign a unique Sequence name');
}

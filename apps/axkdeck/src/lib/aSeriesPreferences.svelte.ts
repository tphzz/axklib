import { getContext, setContext } from 'svelte';
import type { SampleStorageFormat } from './objectEditing';

export type ASeriesGeneration = 'A3000' | 'A4000_A5000';
export type KnownSampleFormat = Exclude<SampleStorageFormat, 'UNKNOWN'>;
interface PreferencesAdapter {
    load(): Promise<ASeriesGeneration>;
    save(generation: ASeriesGeneration): Promise<void>;
}

export class ASeriesPreferences {
    generation = $state<ASeriesGeneration>('A3000');
    loadError = $state('');
    readonly ready: Promise<void>;

    constructor(
        private readonly adapter: PreferencesAdapter = {
            load: async () => 'A3000',
            save: async () => {},
        },
    ) {
        this.ready = adapter
            .load()
            .then((generation) => {
                this.generation = generation;
            })
            .catch((error) => {
                this.loadError = String(error);
            });
    }

    async save(generation: ASeriesGeneration): Promise<void> {
        await this.ready;
        if (this.loadError) throw new Error(this.loadError);
        await this.adapter.save(generation);
        this.generation = generation;
    }
}

const contextKey = Symbol('a-series-preferences');
export function provideASeriesPreferences(preferences: ASeriesPreferences): void {
    setContext(contextKey, preferences);
}
export function useASeriesPreferences(): ASeriesPreferences {
    return getContext<ASeriesPreferences>(contextKey) ?? new ASeriesPreferences();
}
export function generationSampleFormat(generation: ASeriesGeneration): KnownSampleFormat {
    return generation === 'A4000_A5000' ? 'A4000_A5000_224' : 'A3000_188';
}
export function initialBankSampleFormat(
    formats: (SampleStorageFormat | undefined)[],
    generation: ASeriesGeneration,
): KnownSampleFormat {
    if (formats.includes('A4000_A5000_224')) return 'A4000_A5000_224';
    if (formats.includes('A3000_188')) return 'A3000_188';
    return generationSampleFormat(generation);
}

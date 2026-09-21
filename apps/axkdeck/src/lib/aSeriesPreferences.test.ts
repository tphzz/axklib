import { describe, expect, it, vi } from 'vitest';
import { ASeriesPreferences, generationSampleFormat, initialBankSampleFormat } from './aSeriesPreferences.svelte';

describe('A-Series preferences', () => {
    it('loads a persisted generation and publishes only successful writes', async () => {
        const save = vi.fn().mockRejectedValueOnce(new Error('Disk full')).mockResolvedValue(undefined);
        const preferences = new ASeriesPreferences({ load: async () => 'A4000_A5000', save });
        await preferences.ready;
        expect(preferences.generation).toBe('A4000_A5000');
        await expect(preferences.save('A3000')).rejects.toThrow('Disk full');
        expect(preferences.generation).toBe('A4000_A5000');
        await preferences.save('A3000');
        expect(preferences.generation).toBe('A3000');
    });

    it('retains an explicit load error without overwriting settings', async () => {
        const save = vi.fn();
        const preferences = new ASeriesPreferences({
            load: async () => {
                throw new Error('Invalid settings');
            },
            save,
        });
        await preferences.ready;
        expect(preferences.generation).toBe('A3000');
        expect(preferences.loadError).toContain('Invalid settings');
        await expect(preferences.save('A4000_A5000')).rejects.toThrow('Invalid settings');
        expect(save).not.toHaveBeenCalled();
    });

    it('selects the highest known source format before consulting preferences', () => {
        expect(initialBankSampleFormat(['A3000_188'], 'A4000_A5000')).toBe('A3000_188');
        expect(initialBankSampleFormat(['A3000_188', 'A4000_A5000_224'], 'A3000')).toBe('A4000_A5000_224');
        expect(initialBankSampleFormat(['UNKNOWN', undefined], 'A4000_A5000')).toBe('A4000_A5000_224');
        expect(initialBankSampleFormat(['UNKNOWN', 'A3000_188'], 'A4000_A5000')).toBe('A3000_188');
        expect(generationSampleFormat('A3000')).toBe('A3000_188');
    });
});

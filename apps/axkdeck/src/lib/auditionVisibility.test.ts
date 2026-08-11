import { describe, expect, it } from 'vitest';
import { playbackRowVisible } from './auditionVisibility';

describe('playbackRowVisible', () => {
    it('accepts either the Sample Bank row or its current Sample row during sequence playback', () => {
        const context = {
            view: 'sample-banks' as const,
            playingSampleBankId: 'bank-1',
            playingObjectId: 'sample-1',
            visibleSampleBankIds: ['bank-1'],
            visibleSampleIds: [],
            visibleWaveDataIds: [],
        };
        expect(playbackRowVisible(context)).toBe(true);
        expect(
            playbackRowVisible({
                ...context,
                visibleSampleBankIds: [],
                visibleSampleIds: ['sample-1'],
            }),
        ).toBe(true);
    });

    it('rejects playback after its only row is filtered out', () => {
        expect(
            playbackRowVisible({
                view: 'samples',
                playingSampleBankId: '',
                playingObjectId: 'sample-1',
                visibleSampleBankIds: [],
                visibleSampleIds: [],
                visibleWaveDataIds: [],
            }),
        ).toBe(false);
    });

    it('rejects every active playback owner outside an audio view', () => {
        expect(
            playbackRowVisible({
                view: 'programs',
                playingSampleBankId: '',
                playingObjectId: 'wave-1',
                visibleSampleBankIds: [],
                visibleSampleIds: [],
                visibleWaveDataIds: ['wave-1'],
            }),
        ).toBe(false);
    });
});

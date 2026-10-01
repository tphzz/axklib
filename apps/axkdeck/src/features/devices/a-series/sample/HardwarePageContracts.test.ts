import { describe, expect, it } from 'vitest';
import { sampleFields, sampleTabs } from './fields';
import { pageGroups } from './pageGroups';

const pages = sampleTabs.flatMap((tab) => tab.pages);
const pageFor = (key: string) => pages.filter((page) => page.fields.some((field) => field.key === key));

describe('Hardware-aligned Sample page contracts', () => {
    it('uses Sample Info as the second Trim/Loop page', () => {
        const trim = sampleTabs.find((tab) => tab.id === 'trim-loop')!;
        expect(trim.pages.map(({ id, label }) => ({ id, label }))).toEqual([
            { id: 'waveform', label: 'Waveform' },
            { id: 'sample-info', label: 'Sample Info' },
        ]);
        expect(pageFor('loop_tempo_hundredths').map((page) => page.id)).toEqual(['sample-info']);
        expect(pageFor('wave_start_velocity_sensitivity').map((page) => page.id)).toEqual(['sample-info']);
    });

    it('places Expansion & Velocity before Level scaling', () => {
        expect(sampleTabs.find((tab) => tab.id === 'map-out')!.pages.map((page) => page.id)).toEqual([
            'mix-key',
            'pitch',
            'velocity',
            'level-scaling',
        ]);
    });

    it.each(['mono_mode', 'velocity_sensitivity'])('places %s only in Mix & Key', (key) => {
        expect(pageFor(key).map((page) => page.id)).toEqual(['mix-key']);
    });

    it('presents the hardware Poly/Mono choices instead of a MIDI Mono mode toggle', () => {
        const field = sampleFields.find((field) => field.key === 'mono_mode')!;
        expect(field.label).toMatch(/^poly\s*\/\s*mono$/i);
        expect(field.options?.map((option) => option.label)).toEqual(['Poly', 'Mono']);
    });

    it.each(['pitch_bend_type', 'pitch_bend_range', 'velocity_low_limit', 'velocity_offset'])(
        'places %s only in MIDI Set',
        (key) => {
            expect(pageFor(key).map(({ id, label }) => ({ id, label }))).toEqual([{ id: 'midi', label: 'MIDI Set' }]);
        },
    );

    it('keeps Loop Mode in Waveform rather than local audition settings', () => {
        expect(pageFor('loop_mode').map((page) => page.id)).toEqual(['waveform']);
        expect(sampleFields.find((field) => field.key === 'loop_mode')!.label).toMatch(/^loop mode$/i);
    });

    it('retains each saved parameter exactly once through page regrouping', () => {
        expect(new Set(sampleFields.map((field) => field.key)).size).toBe(sampleFields.length);
        for (const page of pages) {
            expect(
                pageGroups(page)
                    .flatMap((group) => group.fields.map((field) => field.key))
                    .toSorted(),
            ).toEqual(page.fields.map((field) => field.key).toSorted());
        }
    });
});

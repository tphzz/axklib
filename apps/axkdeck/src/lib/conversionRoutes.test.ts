import { describe, expect, it } from 'vitest';
import { conversionRoutes, preferredConversionRoute } from './conversionRoutes';
import type { ConversionProfile } from './conversions';

const profile: ConversionProfile = {
    id: 'test-a',
    name: 'Test route',
    sourceFormat: 'source',
    destinationFormat: 'target',
    outputKind: 'NEW_IMAGE',
    route: {
        targetLabel: 'Wave floppy',
        medium: 'floppy',
        intents: ['WAVE_DATA'],
        editorCapabilities: ['new-image-file'],
        priority: 0,
    },
    optionsSchema: {},
};
const query = { sourceFormat: 'source', intent: 'WAVE_DATA', medium: 'floppy' };

describe('converter registry', () => {
    it('matches advertised intent, medium, direction and output without a fallback', () => {
        const list = [
            profile,
            { ...profile, id: 'headless', route: null },
            { ...profile, id: 'reverse', sourceFormat: 'target' },
        ];
        expect(conversionRoutes(list, query).map((r) => r.profile.id)).toEqual(['test-a']);
        expect(conversionRoutes(list, { ...query, intent: 'PLAYABLE_INSTRUMENTS' })).toEqual([]);
        expect(conversionRoutes(list, { ...query, outputKind: 'EXISTING_IMAGE' })).toEqual([]);
        expect(conversionRoutes(list, { ...query, medium: 'optical' })).toEqual([]);
    });
    it('retains unknown editor requirements as unavailable and orders alternatives independently of server order', () => {
        const route = profile.route!;
        const list = [
            { ...profile, id: 'z', route: { ...route, priority: -1, editorCapabilities: ['future-editor'] } },
            { ...profile, id: 'b' },
            profile,
        ];
        const matches = conversionRoutes(list, query);
        expect(matches.map((r) => r.profile.id)).toEqual(['z', 'b', 'test-a']);
        expect(matches[0]).toMatchObject({ supported: false, missingCapabilities: ['future-editor'] });
        expect(matches[1].supported).toBe(true);
    });
    it('selects only an exact panel set and refuses to guess among equally preferred converters', () => {
        const extra = {
            ...profile,
            id: 'extra',
            route: { ...profile.route!, editorCapabilities: ['new-image-file', 'disk-set-selection'] },
        };
        const scoped = { ...query, editorPanels: ['new-image-file'] };
        expect(preferredConversionRoute(conversionRoutes([extra, profile], scoped))?.profile.id).toBe('test-a');
        expect(preferredConversionRoute(conversionRoutes([extra], scoped))).toBeUndefined();
        expect(() =>
            preferredConversionRoute(conversionRoutes([profile, { ...profile, id: 'other' }], scoped)),
        ).toThrow(/same priority/);
    });
});
